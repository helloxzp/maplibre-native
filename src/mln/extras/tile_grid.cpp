#include <mln/extras/tile_grid.hpp>

#include <mln/extras/merged_tile_data.hpp>

#include <mln/storage/file_source.hpp>
#include <mln/util/async_request.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/response.hpp>
#include <mln/tile/vector_mvt_tile_data.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/tileset.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>

namespace mln {
namespace extras {

namespace {
/// 墨卡托世界的纬度边缘:atan(sinh(π)),即归一化 y 恰好落满 [0,1]。
constexpr double kMaxMercLat = 85.05112877980659;

/// sidecar 经 Resource::tile() 组装 URL 时旁路 leg0 行号重映射(见头文件)。
thread_local int resourceBypassDepth = 0;
} // namespace

bool tileGridResourceBypassed() noexcept {
    return resourceBypassDepth > 0;
}

TileGridResourceBypass::TileGridResourceBypass() {
    ++resourceBypassDepth;
}
TileGridResourceBypass::~TileGridResourceBypass() {
    --resourceBypassDepth;
}

double TileGridExtension::mercNormY(double lat) noexcept {
    lat = std::clamp(lat, -kMaxMercLat, kMaxMercLat);
    // 与引擎 Projection::project_ 的 log-tan 同式,归一化到 [0,1](0 = 北缘)。
    return 0.5 - std::log(std::tan(std::numbers::pi / 4 + lat * std::numbers::pi / 360.0)) /
                     (2 * std::numbers::pi);
}

double TileGridExtension::latFromMercNormY(double normY) noexcept {
    normY = std::clamp(normY, 0.0, 1.0);
    return 2.0 * std::atan(std::exp(std::numbers::pi * (1.0 - 2.0 * normY))) * 180.0 / std::numbers::pi - 90.0;
}

bool TileGridExtension::configure(const std::string&, const JSValue& value) {
    if (!value.IsString()) {
        Log::Warning(Event::ParseStyle, "extra.tile-grid 必须是字符串:\"plate-carree\" / \"none\"");
        return false;
    }
    const std::string_view name{value.GetString(), value.GetStringLength()};
    if (name == "plate-carree" || name == "platecarree" || name == "equirectangular") {
        active.store(true, std::memory_order_release);
        return true;
    }
    if (name == "none" || name == "mercator") {
        active.store(false, std::memory_order_release);
        return true;
    }
    Log::Warning(Event::ParseStyle, "extra.tile-grid 取值无法识别:" + std::string(name));
    return false;
}

void TileGridExtension::reset() {
    active.store(false, std::memory_order_release);
}

void TileGridExtension::sourceRows(uint8_t z, uint32_t y, uint32_t& r0, uint32_t& r1) const noexcept {
    const double n = std::pow(2.0, double(z));
    const double step = 180.0 / n;
    // 墨卡托行的纬度带(北缘纬度更大)
    const double phiTop = latFromMercNormY(double(y) / n);
    const double phiBot = latFromMercNormY(double(y + 1) / n);
    constexpr double eps = 1e-9;
    const double maxRow = n - 1.0;
    r0 = static_cast<uint32_t>(std::clamp(std::floor((90.0 - phiTop) / step + eps), 0.0, maxRow));
    r1 = static_cast<uint32_t>(std::clamp(std::floor((90.0 - phiBot) / step - eps), 0.0, maxRow));
    if (r1 < r0) {
        r1 = r0;
    }
    // 2·cosφ ≤ 2 ⇒ 最多跨 3 行;防御性封顶。
    if (r1 - r0 > 3) {
        r1 = r0 + 3;
    }
}

const GeometryCollection& DegreeRowToMercatorFeature::getGeometries() const {
    if (transformed) {
        return *transformed;
    }
    const auto& source = inner->getGeometries();
    GeometryCollection out;
    out.reserve(source.size());

    const auto type = inner->getType();
    std::vector<detail::Point32> mapped;
    for (const auto& ring : source) {
        mapped.clear();
        mapped.reserve(ring.size());
        for (const auto& p : ring) {
            const double phi = params.phiTop - double(p.y) * params.degPerUnit;
            const double warped = (TileGridExtension::mercNormY(phi) - params.myA) * params.scaleY;
            // 越出 int32 的极端外推(极区缓冲)直接钳制;随后裁剪会丢弃。
            const double clamped = std::clamp(warped, -1.0e9, 1.0e9);
            mapped.emplace_back(p.x, static_cast<int32_t>(std::llround(clamped)));
        }
        switch (type) {
            case FeatureType::Polygon:
                detail::clipRing(mapped, out);
                break;
            case FeatureType::Point: {
                // **点只归它自己那一格**,判据是 [0, EXTENT) —— 与
                // subdivided_tile_data.hpp / merged_tile_data.hpp 同一条规则。
                //
                // clipMin/clipMax 那 ±2048 是给线和面留的:跨界几何要靠缓冲接得上。
                // 点不需要,而且放宽会产生重复:x 方向源行与目标瓦片是 1:1,源瓦片自带
                // 的 ±256 缓冲会让列缝上的 POI 被左右两张各发一份;y 方向一张墨卡托
                // 瓦片要合成 1~3 条纬度行,行缝上的点同样会被相邻两行各映一份 —— 而
                // 它们在合成瓦片里落到同一个位置。poi-cover 那层(canCovered)把避让
                // 整个关掉了,重复压不掉,叠出来就是同一个标注被画两遍、笔画变粗。
                //
                // 这一处目前没实测到咬人(az 4 全屏 38 个标注逐个比对,修子分之后已经
                // 没有残留重复了),但形状与那两处完全相同,三处同一件事该只有一种写法。
                GeometryCoordinates kept;
                for (const auto& p : mapped) {
                    if (p.first >= 0 && p.first < util::EXTENT && p.second >= 0 && p.second < util::EXTENT) {
                        kept.push_back(detail::narrow(p));
                    }
                }
                if (!kept.empty()) out.push_back(std::move(kept));
                break;
            }
            default: // LineString 及未知类型按折线处理
                detail::clipLine(mapped, out);
                break;
        }
    }
    transformed = std::move(out);
    return *transformed;
}

std::unique_ptr<GeometryTileData> ComposedGeometryTileData::clone() const {
    std::vector<std::unique_ptr<GeometryTileData>> copies;
    copies.reserve(inners.size());
    for (const auto& inner : inners) {
        copies.push_back(inner->clone());
    }
    return std::make_unique<ComposedGeometryTileData>(std::move(copies));
}

namespace {

class ComposedLayer final : public GeometryTileLayer {
public:
    ComposedLayer(std::string name_, std::vector<std::unique_ptr<GeometryTileLayer>> parts_)
        : name(std::move(name_)),
          parts(std::move(parts_)) {
        offsets.reserve(parts.size());
        std::size_t total = 0;
        for (const auto& part : parts) {
            offsets.push_back(total);
            total += part->featureCount();
        }
        count = total;
    }

    std::size_t featureCount() const override { return count; }
    std::unique_ptr<GeometryTileFeature> getFeature(std::size_t i) const override {
        for (std::size_t k = parts.size(); k-- > 0;) {
            if (i >= offsets[k]) {
                return parts[k]->getFeature(i - offsets[k]);
            }
        }
        return parts.front()->getFeature(i);
    }
    std::string getName() const override { return name; }

private:
    std::string name;
    std::vector<std::unique_ptr<GeometryTileLayer>> parts;
    std::vector<std::size_t> offsets;
    std::size_t count = 0;
};

} // namespace

std::unique_ptr<GeometryTileLayer> ComposedGeometryTileData::getLayer(const std::string& name) const {
    std::vector<std::unique_ptr<GeometryTileLayer>> parts;
    for (const auto& inner : inners) {
        if (auto layer = inner->getLayer(name)) {
            parts.push_back(std::move(layer));
        }
    }
    if (parts.empty()) {
        return nullptr;
    }
    if (parts.size() == 1) {
        return std::move(parts.front());
    }
    return std::make_unique<ComposedLayer>(name, std::move(parts));
}

TileGridDualFetch::TileGridDualFetch(std::shared_ptr<FileSource> fileSource_,
                                     std::string urlTemplate_,
                                     float pixelRatio_,
                                     uint8_t scheme_,
                                     std::string sourceID_,
                                     uint8_t z_,
                                     uint32_t x_,
                                     uint32_t yMercator_,
                                     std::function<void()> onLegArrived_,
                                     bool fetchPrimary)
    : fileSource(std::move(fileSource_)),
      urlTemplate(std::move(urlTemplate_)),
      pixelRatio(pixelRatio_),
      scheme(scheme_),
      sourceID(std::move(sourceID_)),
      z(z_),
      x(x_),
      yMercator(yMercator_),
      onLegArrived(std::move(onLegArrived_)) {
    uint32_t r0 = yMercator;
    uint32_t r1 = yMercator;
    tileGridSourceRows(z, yMercator, r0, r1);
    for (uint32_t r = r0; r <= r1; ++r) {
        Leg leg;
        leg.row = r;
        legs.push_back(std::move(leg));
    }
    // leg0 通常走引擎 TileLoader(字节经 setPrimaryData 注入);其余行自行请求
    // (缓存优先,一次性)。fetchPrimary 时 leg0 也自取——负子分的非西北角子格
    // 没有对应的引擎取瓦。
    for (std::size_t i = fetchPrimary ? 0 : 1; i < legs.size(); ++i) {
        if (!fileSource) {
            legs[i].done = true;
            if (i == 0) {
                leg0Set = true;
            }
            continue;
        }
        Resource resource = [&] {
            // leg0 的行号本来就该由 Resource::tile() 映射(它就是"覆盖北缘的度数
            // 行"),所以只有其余行才旁路;否则会被重复映射一次。
            std::optional<TileGridResourceBypass> bypass;
            if (i > 0) {
                bypass.emplace();
            }
            return Resource::tile(urlTemplate,
                                  pixelRatio,
                                  static_cast<int32_t>(x),
                                  static_cast<int32_t>(i > 0 ? legs[i].row : yMercator),
                                  static_cast<int8_t>(z),
                                  static_cast<Tileset::Scheme>(scheme),
                                  Resource::LoadingMethod::All);
        }();
        legs[i].request = fileSource->request(resource, [this, i](const Response& response) {
            Leg& leg = legs[i];
            leg.done = true;
            leg.request.reset();
            if (!response.error && response.data) {
                leg.data = response.data;
            }
            if (i == 0) {
                leg0Set = true;
            }
            if (onLegArrived) {
                onLegArrived();
            }
        });
    }
}

TileGridDualFetch::~TileGridDualFetch() = default;

void TileGridDualFetch::setPrimaryData(std::shared_ptr<const std::string> data) {
    leg0Set = true;
    legs.front().done = true;
    legs.front().data = std::move(data);
}

bool TileGridDualFetch::ready() const noexcept {
    if (!leg0Set) {
        return false;
    }
    for (const auto& leg : legs) {
        if (!leg.done) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<GeometryTileData> TileGridDualFetch::build() const {
    const double n = std::pow(2.0, double(z));
    const double step = 180.0 / n;
    const double myA = double(yMercator) / n;
    // 墨卡托瓦片高度恒为 1/2^z,scaleY = EXTENT / (1/2^z)。
    const double scaleY = double(util::EXTENT) * n;

    std::vector<std::unique_ptr<GeometryTileData>> parts;
    for (const auto& leg : legs) {
        if (!leg.data) {
            continue;
        }
        std::unique_ptr<GeometryTileData> data = std::make_unique<VectorMVTTileData>(leg.data);
        // 第一步:该度数行的稀疏层级子分(数据可能来自更粗的祖先)。
        if (const auto sub = tileSubdivision(z, x, leg.row); sub.steps > 0) {
            data = std::make_unique<SubdividedGeometryTileData>(
                std::move(data),
                static_cast<uint8_t>(sub.steps),
                sub.offX,
                sub.offY,
                sharedParentIndex(sourceID, sub.dataZ, x >> sub.steps, leg.row >> sub.steps));
        }
        // 第二步:度数行-local → 墨卡托瓦片-local(warp + 裁剪)。
        DegreeRowToMercatorFeature::Params params;
        params.phiTop = 90.0 - double(leg.row) * step;
        params.degPerUnit = step / double(util::EXTENT);
        params.myA = myA;
        params.scaleY = scaleY;
        parts.push_back(std::make_unique<DegreeRowToMercatorTileData>(std::move(data), params));
    }
    if (parts.empty()) {
        return nullptr;
    }
    if (parts.size() == 1) {
        return std::move(parts.front());
    }
    return std::make_unique<ComposedGeometryTileData>(std::move(parts));
}

namespace {

/// 负子分下的一个子格数据源:就一张瓦片,没有度数行拆分(tile-grid 未生效时用)。
class SingleTileFetch final : public TileDataSource {
public:
    SingleTileFetch(const std::shared_ptr<FileSource>& fileSource,
                    const std::string& urlTemplate,
                    float pixelRatio,
                    uint8_t scheme,
                    uint8_t z,
                    uint32_t x,
                    uint32_t y,
                    bool fetchSelf,
                    std::function<void()> onArrived_)
        : onArrived(std::move(onArrived_)) {
        if (!fetchSelf) {
            return; // 字节由引擎经 setPrimaryData 注入
        }
        if (!fileSource) {
            done = true;
            return;
        }
        const Resource resource = Resource::tile(urlTemplate,
                                                 pixelRatio,
                                                 static_cast<int32_t>(x),
                                                 static_cast<int32_t>(y),
                                                 static_cast<int8_t>(z),
                                                 static_cast<Tileset::Scheme>(scheme),
                                                 Resource::LoadingMethod::All);
        request = fileSource->request(resource, [this](const Response& response) {
            done = true;
            request.reset();
            if (!response.error && response.data) {
                data = response.data;
            }
            if (onArrived) {
                onArrived();
            }
        });
    }

    void setPrimaryData(std::shared_ptr<const std::string> data_) override {
        data = std::move(data_);
        done = true;
    }
    bool ready() const noexcept override { return done; }
    std::unique_ptr<GeometryTileData> build() const override {
        if (!data) {
            return nullptr;
        }
        return std::make_unique<VectorMVTTileData>(data);
    }

private:
    std::function<void()> onArrived;
    bool done = false;
    std::shared_ptr<const std::string> data;
    std::unique_ptr<AsyncRequest> request;
};

/// 负子分:本瓦片**包含** 2^k × 2^k 张数据瓦片,逐格取回后缩小平移拼合。
///
/// 引擎自己那一次取瓦落在**西北角**那一格(见 Resource::tile 的负 steps 分支),
/// 所以 cells[0] 等它的 setPrimaryData,其余格全部自取。每一格内部仍走完整的
/// 常规路径 —— tile-grid 生效时它自己就是一个 TileGridDualFetch,度数行拆分在
/// 子格这一层完成,两种变换因此天然复合,不必把 leg 集合泛化成行×列。
class MergedTileDataSource final : public TileDataSource {
public:
    MergedTileDataSource(std::shared_ptr<FileSource> fileSource,
                         const std::string& urlTemplate,
                         float pixelRatio,
                         uint8_t scheme,
                         const std::string& sourceID,
                         uint32_t x,
                         uint32_t y,
                         uint8_t dataZ,
                         uint8_t steps,
                         std::function<void()> onArrived)
        : k(steps) {
        // 扇出是 4^k。现实配置下 k ≤ 2(相机下限 0 ⇒ canonical 最低 1,而最粗数据
        // 是 z3),即 16 张源瓦片。这里只给写错的 tile-levels 兜底,免得一个手误让
        // 单张瓦片去拉几千个请求。
        if (k > 3) {
            Log::Warning(Event::ParseStyle,
                         "extra.tile-levels:本层级比数据层级低 " + std::to_string(int(k)) +
                             " 级,扇出过大,放弃合成(该层级无数据)");
            return;
        }
        const uint32_t n = 1u << k;
        const uint32_t baseX = x << k;
        const uint32_t baseY = y << k;
        cells.reserve(static_cast<std::size_t>(n) * n);
        for (uint32_t oy = 0; oy < n; ++oy) {
            for (uint32_t ox = 0; ox < n; ++ox) {
                const bool primary = (ox == 0 && oy == 0);
                Cell cell;
                cell.offX = ox;
                cell.offY = oy;
                if (tileGridActive()) {
                    cell.src = std::make_unique<TileGridDualFetch>(fileSource,
                                                                   urlTemplate,
                                                                   pixelRatio,
                                                                   scheme,
                                                                   sourceID,
                                                                   dataZ,
                                                                   baseX + ox,
                                                                   baseY + oy,
                                                                   onArrived,
                                                                   /*fetchPrimary=*/!primary);
                } else {
                    cell.src = std::make_unique<SingleTileFetch>(fileSource,
                                                                 urlTemplate,
                                                                 pixelRatio,
                                                                 scheme,
                                                                 dataZ,
                                                                 baseX + ox,
                                                                 baseY + oy,
                                                                 /*fetchSelf=*/!primary,
                                                                 onArrived);
                }
                cells.push_back(std::move(cell));
            }
        }
    }

    void setPrimaryData(std::shared_ptr<const std::string> data) override {
        if (cells.empty()) {
            return; // 扇出兜底已放弃合成
        }
        cells.front().src->setPrimaryData(std::move(data));
    }

    bool ready() const noexcept override {
        for (const auto& cell : cells) {
            if (!cell.src->ready()) {
                return false;
            }
        }
        return true;
    }

    /// 渐进式:16 格全等齐才出图会让世界视图整块整块地跳出来。每格到达就用当前
    /// 已有的部分重建——`build()` 本来就会跳过还没到的格子。
    bool progressive() const noexcept override { return true; }

    std::unique_ptr<GeometryTileData> build() const override {
        std::vector<std::unique_ptr<GeometryTileData>> parts;
        parts.reserve(cells.size());
        for (const auto& cell : cells) {
            auto data = cell.src->build();
            if (!data) {
                continue; // 空瓦片/失败的格子跳过,其余照常出图
            }
            parts.push_back(std::make_unique<MergedGeometryTileData>(std::move(data), k, cell.offX, cell.offY));
        }
        if (parts.empty()) {
            return nullptr;
        }
        if (parts.size() == 1) {
            return std::move(parts.front());
        }
        return std::make_unique<ComposedGeometryTileData>(std::move(parts));
    }

private:
    struct Cell {
        uint32_t offX = 0;
        uint32_t offY = 0;
        std::unique_ptr<TileDataSource> src;
    };

    const uint8_t k;
    std::vector<Cell> cells; ///< cells[0] = 西北角(引擎注入的那一格)
};

} // namespace

std::unique_ptr<TileDataSource> makeTileDataSource(std::shared_ptr<FileSource> fileSource,
                                                   const std::string& urlTemplate,
                                                   float pixelRatio,
                                                   uint8_t scheme,
                                                   const std::string& sourceID,
                                                   uint8_t z,
                                                   uint32_t x,
                                                   uint32_t y,
                                                   std::function<void()> onDataArrived) {
    if (urlTemplate.empty()) {
        return nullptr;
    }
    // 负子分优先:它内部会按需再套 tile-grid 的度数行拆分。
    if (const auto sub = tileSubdivision(z, x, y); sub.steps < 0) {
        return std::make_unique<MergedTileDataSource>(std::move(fileSource),
                                                      urlTemplate,
                                                      pixelRatio,
                                                      scheme,
                                                      sourceID,
                                                      x,
                                                      y,
                                                      sub.dataZ,
                                                      static_cast<uint8_t>(-sub.steps),
                                                      std::move(onDataArrived));
    }
    // 其余情况只有 tile-grid 需要额外的数据源;否则上游走原生路径。
    if (!tileGridActive()) {
        return nullptr;
    }
    return std::make_unique<TileGridDualFetch>(
        std::move(fileSource), urlTemplate, pixelRatio, scheme, sourceID, z, x, y, std::move(onDataArrived));
}

} // namespace extras
} // namespace mln
