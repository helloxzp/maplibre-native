#include <mln/renderer/tile_pyramid.hpp>
#include <mln/extras/hooks.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/renderer/query.hpp>
#include <mln/map/transform.hpp>
#include <mln/math/clamp.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/util/tile_cover.hpp>
#include <mln/util/tile_range.hpp>
#include <mln/util/enum.hpp>
#include <mln/util/logging.hpp>

#include <mln/algorithm/update_renderables.hpp>

#include <mapbox/geometry/envelope.hpp>

#include <cmath>
#include <algorithm>

namespace mln {

using namespace style;

namespace {
TileObserver nullObserver;
const std::map<OverscaledTileID, std::unique_ptr<Tile>> emptyPrefetchedTiles;
} // namespace

TilePyramid::TilePyramid(const TaggedScheduler& threadPool_)
    : cache(threadPool_),
      observer(&nullObserver) {}

TilePyramid::~TilePyramid() = default;

bool TilePyramid::isLoaded() const {
    for (const auto& pair : tiles) {
        if (!pair.second->isComplete()) {
            return false;
        }
    }

    return true;
}

Tile* TilePyramid::getTile(const OverscaledTileID& tileID) {
    auto it = tiles.find(tileID);
    return it == tiles.end() ? cache.get(tileID) : it->second.get();
}

const Tile* TilePyramid::getRenderedTile(const UnwrappedTileID& tileID) const {
    auto it = renderedTiles.find(tileID);
    return it != renderedTiles.end() ? &it->second.get() : nullptr;
}

void TilePyramid::update(const std::vector<Immutable<style::LayerProperties>>& layers,
                         const bool needsRendering,
                         const bool needsRelayout,
                         const TileParameters& parameters,
                         const style::Source::Impl& sourceImpl,
                         const uint16_t tileSize,
                         const Range<uint8_t> zoomRange,
                         std::optional<LatLngBounds> bounds,
                         std::function<std::unique_ptr<Tile>(const OverscaledTileID&, TileObserver*)> createTile) {
    // If we need a relayout, abandon any cached tiles; they're now stale.
    if (needsRelayout) {
        cache.clear();
    }

    // If we're not going to render anything, move our existing tiles into
    // the cache (if they're not stale) or abandon them, and return.
    if (!needsRendering) {
        for (auto& entry : tiles) {
            if (!needsRelayout) {
                // These tiles are invisible, we set optional necessity
                // for them and thus suppress network requests on
                // tiles expiration (see `OnlineFileRequest`).
                entry.second->setNecessity(TileNecessity::Optional);
                cache.add(entry.first, std::move(entry.second));
            } else {
                cache.deferredRelease(std::move(entry.second));
            }
        }

        tiles.clear();
        renderedTiles.clear();
        cache.deferPendingReleases();

        return;
    }

    handleWrapJump(static_cast<float>(parameters.transformState.getLatLng().longitude()));

    // Optionally shift the zoom level
    double zoom = util::clamp<double>(parameters.transformState.getZoom() + parameters.tileLodZoomShift,
                                      parameters.transformState.getMinZoom(),
                                      parameters.transformState.getMaxZoom());

    const bool zoomingIn = zoom > prevZoom;
    prevZoom = zoom;

    const auto type = sourceImpl.type;
    // Determine the overzooming/underzooming amounts and required tiles.
    int32_t overscaledZoom = util::coveringZoomLevel(zoom, type, tileSize);
    int32_t tileZoom = overscaledZoom;
    int32_t panZoom = zoomRange.max;

    const std::optional<uint8_t>& sourcePrefetchZoomDelta = sourceImpl.getPrefetchZoomDelta();
    const std::optional<uint8_t>& maxParentTileOverscaleFactor = sourceImpl.getMaxOverscaleFactorForParentTiles();
    const Duration minimumUpdateInterval = sourceImpl.getMinimumTileUpdateInterval();
    const bool isVolatile = sourceImpl.isVolatile();

    std::vector<OverscaledTileID> idealTiles;
    std::vector<OverscaledTileID> panTiles;

    util::TileCoverParameters tileCoverParameters = {.transformState = parameters.transformState,
                                                     .tileLodMinRadius = parameters.tileLodMinRadius,
                                                     .tileLodScale = parameters.tileLodScale,
                                                     .tileLodPitchThreshold = parameters.tileLodPitchThreshold,
                                                     .tileLodMode = parameters.tileLodMode};

    if (std::cmp_greater_equal(overscaledZoom, zoomRange.min)) {
        int32_t idealZoom = std::min<int32_t>(zoomRange.max, overscaledZoom);

        // Make sure we're not reparsing overzoomed raster tiles.
        if (type == SourceType::Raster) {
            tileZoom = idealZoom;
        }

        // Only attempt prefetching in continuous mode.
        if (parameters.mode == MapMode::Continuous && type != style::SourceType::GeoJSON &&
            type != style::SourceType::Annotations) {
            // Request lower zoom level tiles (if configured to do so) in an attempt
            // to show something on the screen faster at the cost of a little of bandwidth.
            const uint8_t prefetchZoomDelta = sourcePrefetchZoomDelta ? *sourcePrefetchZoomDelta
                                                                      : parameters.prefetchZoomDelta;
            if (prefetchZoomDelta) {
                panZoom = std::max<int32_t>(tileZoom - prefetchZoomDelta, zoomRange.min);
            }

            // hook:占位层级换不到**更粗的数据层**(稀疏金字塔最粗档以下,z-delta
            // 还是同一批源瓦的负子分合成 —— 更贵、更空)就不发。无扩展生效时
            // tileSubdivision 恒等,条件退化为 panZoom < idealZoom,行为与原版一致。
            if (panZoom < idealZoom &&
                extras::tileSubdivision(static_cast<uint8_t>(panZoom), 0, 0).dataZ <
                    extras::tileSubdivision(static_cast<uint8_t>(idealZoom), 0, 0).dataZ) {
                panTiles = util::tileCover(tileCoverParameters, panZoom, zoomRange);
            }

            // hook:**边界预取** —— 正在放大且逼近"下一个更细数据级"时,先把那一级
            // 的源数据灌进缓存,搭 panTiles 的顺风车(同样只取不渲染)。
            //
            // 跨过数据级边界的一瞬手上只有粗一级的瓦,而粗瓦的符号不予显示(见
            // hooks.hpp 的 symbolDataCoarse),POI 要等新数据到货 —— 视觉上是
            // "一瞬间消失又出现",实测全新区域约 1 秒。预取该数据级里**最粗的一档**
            // 而非即将进入的那一档:两者请求同一批 URL(URL 层级由数据级决定),但
            // 最粗档覆盖同样视野只要几张瓦,目标档要几十张;源数据进了缓存,跨过去
            // 时细档瓦只需解析、不必联网。便宜,所以提前量敢给到一整档。
            //
            // 只在放大时预取:停着或平移时相机不会跨边界,预热纯属浪费(实测停在
            // 13.0 平移就白拉 237 张细级瓦)。缩小方向也不需要:残留的细瓦本就允许
            // 继续供符号。门控复用 symbolDataCoarse(理想档瓦, zoom+1)——语义恰是
            // "提前一档看,理想档的数据是否已算粗";无扩展生效时它恒假,原版
            // **绝不会**走进预取(直接拿 dataZ(f)>dataZ(ideal) 判是不行的:恒等时
            // f=ideal+1 恒满足,原版放大会白预取下一层级)。
            if (zoomingIn &&
                extras::symbolDataCoarse(static_cast<uint8_t>(std::clamp(idealZoom, 0, 30)), zoom + 1.0)) {
                const uint8_t curDataZ = extras::tileSubdivision(static_cast<uint8_t>(idealZoom), 0, 0).dataZ;
                const int32_t reachZoom = std::min<int32_t>(
                    zoomRange.max, util::coveringZoomLevel(zoom + 1.0, type, tileSize));
                for (int32_t f = idealZoom + 1; f <= reachZoom; ++f) {
                    if (extras::tileSubdivision(static_cast<uint8_t>(f), 0, 0).dataZ > curDataZ) {
                        const auto ahead = util::tileCover(tileCoverParameters, f, zoomRange);
                        panTiles.insert(panTiles.end(), ahead.begin(), ahead.end());
                        break; // 该数据级最粗的一档,瓦最少
                    }
                }
            }
        }

        idealTiles = util::tileCover(tileCoverParameters, idealZoom, zoomRange, tileZoom);
        if (parameters.mode == MapMode::Tile && type != SourceType::Raster && type != SourceType::RasterDEM &&
            idealTiles.size() > 1) {
            mln::Log::Warning(mln::Event::General,
                              "Provided camera options returned " + std::to_string(idealTiles.size()) +
                                  " tiles, only " + util::toString(idealTiles[0]) + " is taken in Tile mode.");
            idealTiles = {idealTiles[0]};
        }
    }

    // Stores a list of all the tiles that we're definitely going to retain.
    // There are two kinds of tiles we need: the ideal tiles determined by the
    // tile cover. They may not yet be in use because they're still loading. In
    // addition to that, we also need to retain all tiles that we're actively
    // using, e.g. as a replacement for tile that aren't loaded yet.
    std::set<OverscaledTileID> retain;

    auto retainTileFn = [&](Tile& tile, TileNecessity necessity) -> void {
        if (retain.emplace(tile.id).second) {
            tile.setUpdateParameters({.minimumUpdateInterval = minimumUpdateInterval, .isVolatile = isVolatile});
            tile.setNecessity(necessity);
        }

        if (needsRelayout) {
            tile.setLayers(layers);
        }
    };
    auto getTileFn = [&](const OverscaledTileID& tileID) -> Tile* {
        auto it = tiles.find(tileID);
        return it == tiles.end() ? nullptr : it->second.get();
    };

    // The min and max zoom for TileRange are based on the updateRenderables
    // algorithm. Tiles are created at the ideal tile zoom or at lower zoom
    // levels. Child tiles are used from the cache, but not created.
    std::optional<util::TileRange> tileRange = std::nullopt;
    if (bounds) {
        int32_t maxZoom = (parameters.tileLodMode == TileLodMode::Distance)
                              ? zoomRange.max
                              : std::min(tileZoom, static_cast<int32_t>(zoomRange.max));
        tileRange = util::TileRange::fromLatLngBounds(*bounds, zoomRange.min, maxZoom);
    }
    auto createTileFn = [&](const OverscaledTileID& tileID) -> Tile* {
        if (tileRange && !tileRange->contains(tileID.canonical)) {
            return nullptr;
        }
        // hook:父链回退同理 —— 祖先换不到更粗的数据层(负子分把同一批源瓦
        // 合成得更大更空)就不值得为回退**新建**;已存在的仍照常复用。
        if (static_cast<int32_t>(tileID.overscaledZ) < tileZoom &&
            extras::tileSubdivision(static_cast<uint8_t>(tileID.overscaledZ), 0, 0).dataZ >=
                extras::tileSubdivision(static_cast<uint8_t>(std::min(tileZoom, 30)), 0, 0).dataZ) {
            return nullptr;
        }
        std::unique_ptr<Tile> tile = cache.pop(tileID);
        if (!tile) {
            tile = createTile(tileID, observer);
            if (!tile) return nullptr;
            tile->setLayers(layers);
        }

        return tiles.emplace(tileID, std::move(tile)).first->second.get();
    };

    auto previouslyRenderedTiles = std::move(renderedTiles);

    // hook:子瓦回退不吃"数据更细、且上一帧没显示过"的瓦。
    //
    // 边界预取(见上)会把下一数据级的瓦提前灌进 tiles;理想瓦尚未建好时,
    // updateRenderables 的子瓦回退会发现这些**数据细两级**的瓦已就绪并直接
    // 渲染 —— 为更深相机准备的内容(路网密度天差地别)突然铺满屏,理想瓦
    // 一到又整片退场,肉眼就是"路网提前出现、闪一下消失"。
    //
    // 判据带"上一帧显示过"是为了放行另一种同形状的情况:缩小跨档时,刚显示
    // 过的细级子瓦要继续垫场到粗级理想瓦就绪,那是连续性,不是闪现。
    // 恒等(无扩展)时 dataZ == z,子瓦(z+1)必"数据更细"——但原版金字塔各级
    // 同源,子瓦只会来自"刚显示过"的缩小残留,预取也不存在,行为不变。
    const auto idealPassZ = static_cast<uint8_t>(std::clamp(tileZoom, 0, 30));
    const uint8_t idealPassDataZ = extras::tileSubdivision(idealPassZ, 0, 0).dataZ;
    auto getTileForIdealFn = [&](const OverscaledTileID& tileID) -> Tile* {
        if (static_cast<int32_t>(tileID.overscaledZ) > tileZoom &&
            previouslyRenderedTiles.find(tileID.toUnwrapped()) == previouslyRenderedTiles.end()) {
            const uint8_t dz = extras::tileSubdivision(tileID.overscaledZ, 0, 0).dataZ;
            // 恒等(无扩展)时子瓦 dataZ==z 恒大于理想 dataZ,单靠大小比较会把原版
            // 的"后台加载完成的子瓦垫场"也挡掉 —— 补一条"确有跨数据级映射参与"
            // 的判据(子瓦或理想档任一非恒等),原版下恒不成立,行为逐位一致。
            if (dz > idealPassDataZ && (dz != tileID.overscaledZ || idealPassDataZ != idealPassZ)) {
                return nullptr;
            }
        }
        auto it = tiles.find(tileID);
        return it == tiles.end() ? nullptr : it->second.get();
    };

    auto renderTileFn = [&](const UnwrappedTileID& tileID, Tile& tile) {
        addRenderTile(tileID, tile);
        previouslyRenderedTiles.erase(tileID); // Still rendering this tile, no need for special fading logic.
        tile.markRenderedIdeal();
    };

    renderedTiles.clear();

    if (!panTiles.empty()) {
        algorithm::updateRenderables(
            getTileFn,
            createTileFn,
            retainTileFn,
            [](const UnwrappedTileID&, Tile&) {},
            panTiles,
            emptyPrefetchedTiles,
            zoomRange,
            maxParentTileOverscaleFactor);
    }

    algorithm::updateRenderables(getTileForIdealFn,
                                 createTileFn,
                                 retainTileFn,
                                 renderTileFn,
                                 idealTiles,
                                 tiles,
                                 zoomRange,
                                 maxParentTileOverscaleFactor);

    for (auto previouslyRenderedTile : previouslyRenderedTiles) {
        Tile& tile = previouslyRenderedTile.second;
        tile.markRenderedPreviously();
        if (tile.holdForFade()) {
            // Since it was rendered in the last frame, we know we have it
            // Don't mark the tile "Required" to avoid triggering a new network request
            retainTileFn(tile, TileNecessity::Optional);
            addRenderTile(previouslyRenderedTile.first, tile);
        }
    }

    if (type != SourceType::Annotations && cacheEnabled) {
        auto conservativeCacheSize = static_cast<size_t>(
            std::max(static_cast<double>(parameters.transformState.getSize().width) / tileSize, 1.0) *
            std::max(static_cast<double>(parameters.transformState.getSize().height) / tileSize, 1.0) *
            (parameters.transformState.getMaxZoom() - parameters.transformState.getMinZoom() + 1) * 0.5);
        cache.setSize(conservativeCacheSize);
    } else {
        cache.setSize(0);
    }

    // Remove stale tiles. This goes through the (sorted!) tiles map and retain
    // set in lockstep and removes items from tiles that don't have the
    // corresponding key in the retain set.
    {
        auto tilesIt = tiles.begin();
        auto retainIt = retain.begin();
        while (tilesIt != tiles.end()) {
            if (retainIt == retain.end() || tilesIt->first < *retainIt) {
                // Remove the tile from the map.
                // If it requires re-layout, discard it asynchronously, otherwise keep it in the cache
                const auto key = tilesIt->first;
                if (std::unique_ptr<Tile> tile = std::move(tiles.extract(tilesIt++).mapped())) {
                    if (needsRelayout) {
                        cache.deferredRelease(std::move(tile));
                    } else {
                        tile->setNecessity(TileNecessity::Optional);
                        cache.add(key, std::move(tile));
                    }
                }
            } else {
                if (!(*retainIt < tilesIt->first)) {
                    ++tilesIt;
                }
                ++retainIt;
            }
        }
    }

    for (auto& pair : tiles) {
        pair.second->setShowCollisionBoxes(parameters.debugOptions & MapDebugOptions::Collision);
    }

    // Initialize renderable tiles and update the contained layer render data.
    for (auto& entry : renderedTiles) {
        Tile& tile = entry.second;
        assert(tile.isRenderable());
        tile.usedByRenderedLayers = false;

        const bool holdForFade = tile.holdForFade();
        for (const auto& layerProperties : layers) {
            const auto* typeInfo = layerProperties->baseImpl->getTypeInfo();
            if (holdForFade && typeInfo->fadingTiles == LayerTypeInfo::FadingTiles::NotRequired) {
                continue;
            }
            tile.usedByRenderedLayers |= tile.layerPropertiesUpdated(layerProperties);
        }
    }

    cache.deferPendingReleases();
}

void TilePyramid::handleWrapJump(float lng) {
    // On top of the regular z/x/y values, TileIDs have a `wrap` value that specify
    // which cppy of the world the tile belongs to. For example, at `lng: 10` you
    // might render z/x/y/0 while at `lng: 370` you would render z/x/y/1.
    //
    // When lng values get wrapped (going from `lng: 370` to `long: 10`) you expect
    // to see the same thing on the screen (370 degrees and 10 degrees is the same
    // place in the world) but all the TileIDs will have different wrap values.
    //
    // In order to make this transition seamless, we calculate the rounded difference of
    // "worlds" between the last frame and the current frame. If the map panned by
    // a world, then we can assign all the tiles new TileIDs with updated wrap values.
    // For example, assign z/x/y/1 a new id: z/x/y/0. It is the same tile, just rendered
    // in a different position.
    //
    // This enables us to reuse the tiles at more ideal locations and prevent flickering.

    const float lngDifference = lng - prevLng;
    const float worldDifference = lngDifference / 360.f;
    const auto wrapDelta = static_cast<int16_t>(std::round(worldDifference));
    prevLng = lng;

    if (wrapDelta) {
        std::map<OverscaledTileID, std::unique_ptr<Tile>> newTiles;
        std::map<UnwrappedTileID, std::reference_wrapper<Tile>> newRenderTiles;
        for (auto& tile : tiles) {
            auto newID = tile.second->id.unwrapTo(tile.second->id.wrap + wrapDelta);
            tile.second->id = newID;
            newTiles.emplace(newID, std::move(tile.second));
        }
        tiles = std::move(newTiles);

        for (auto& tile : renderedTiles) {
            UnwrappedTileID newID = tile.first.unwrapTo(tile.first.wrap + wrapDelta);
            newRenderTiles.emplace(newID, tile.second);
        }
        renderedTiles = std::move(newRenderTiles);
    }
}

std::unordered_map<std::string, std::vector<Feature>> TilePyramid::queryRenderedFeatures(
    const ScreenLineString& geometry,
    const TransformState& transformState,
    const std::unordered_map<std::string, const RenderLayer*>& layers,
    const RenderedQueryOptions& options,
    const mat4& projMatrix,
    const SourceFeatureState& featureState) const {
    std::unordered_map<std::string, std::vector<Feature>> result;
    if (renderedTiles.empty() || geometry.empty()) {
        return result;
    }

    LineString<double> queryGeometry;
    queryGeometry.reserve(geometry.size());

    for (const auto& p : geometry) {
        queryGeometry.push_back(
            TileCoordinate::fromScreenCoordinate(transformState, 0, {p.x, transformState.getSize().height - p.y}).p);
    }

    mapbox::geometry::box<double> box = mapbox::geometry::envelope(queryGeometry);

    auto cmp = [](const UnwrappedTileID& a, const UnwrappedTileID& b) {
        return std::tie(a.canonical.z, a.canonical.y, a.wrap, a.canonical.x) <
               std::tie(b.canonical.z, b.canonical.y, b.wrap, b.canonical.x);
    };

    std::map<UnwrappedTileID, std::reference_wrapper<Tile>, decltype(cmp)> sortedTiles{
        renderedTiles.begin(), renderedTiles.end(), cmp};

    auto maxPitchScaleFactor = transformState.maxPitchScaleFactor();

    for (const auto& entry : sortedTiles) {
        const UnwrappedTileID& id = entry.first;
        Tile& tile = entry.second;

        const auto scale = static_cast<float>(transformState.getScale() /
                                              (1 << id.canonical.z)); // equivalent to std::pow(2,
                                                                      // transformState.getZoom() - id.canonical.z);
        auto queryPadding = maxPitchScaleFactor * tile.getQueryPadding(layers) * util::EXTENT / util::tileSize_D /
                            scale;

        GeometryCoordinate tileSpaceBoundsMin = TileCoordinate::toGeometryCoordinate(id, box.min);
        if (tileSpaceBoundsMin.x - queryPadding >= util::EXTENT ||
            tileSpaceBoundsMin.y - queryPadding >= util::EXTENT) {
            continue;
        }

        GeometryCoordinate tileSpaceBoundsMax = TileCoordinate::toGeometryCoordinate(id, box.max);
        if (tileSpaceBoundsMax.x + queryPadding < 0 || tileSpaceBoundsMax.y + queryPadding < 0) {
            continue;
        }

        GeometryCoordinates tileSpaceQueryGeometry;
        tileSpaceQueryGeometry.reserve(queryGeometry.size());
        for (const auto& c : queryGeometry) {
            tileSpaceQueryGeometry.push_back(TileCoordinate::toGeometryCoordinate(id, c));
        }

        tile.queryRenderedFeatures(
            result, tileSpaceQueryGeometry, transformState, layers, options, projMatrix, featureState);
    }

    return result;
}

std::vector<Feature> TilePyramid::querySourceFeatures(const SourceQueryOptions& options) const {
    std::vector<Feature> result;

    for (const auto& pair : tiles) {
        pair.second->querySourceFeatures(result, options);
    }

    return result;
}

void TilePyramid::setCacheEnabled(bool enable) {
    cacheEnabled = enable;
}

void TilePyramid::reduceMemoryUse() {
    cache.clear();
}

void TilePyramid::setObserver(TileObserver* observer_) {
    observer = observer_;
}

void TilePyramid::dumpDebugLogs() const {
    for (const auto& pair : tiles) {
        pair.second->dumpDebugLogs();
    }
}

void TilePyramid::clearAll() {
    fadingTiles = false;
    tiles.clear();
    renderedTiles.clear();
    cache.clear();
}

void TilePyramid::addRenderTile(const UnwrappedTileID& tileID, Tile& tile) {
    assert(tile.isRenderable());
    renderedTiles.emplace(tileID, tile);
}

void TilePyramid::updateFadingTiles() {
    fadingTiles = false;
    for (auto& entry : renderedTiles) {
        Tile& tile = entry.second;
        if (tile.holdForFade()) {
            fadingTiles = true;
            tile.performedFadePlacement();
        }
    }
}

} // namespace mln
