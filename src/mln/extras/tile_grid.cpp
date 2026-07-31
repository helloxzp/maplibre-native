#include <mln/extras/tile_grid.hpp>

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
                GeometryCoordinates kept;
                for (const auto& p : mapped) {
                    if (p.first >= detail::clipMin && p.first <= detail::clipMax && p.second >= detail::clipMin &&
                        p.second <= detail::clipMax) {
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
                                     std::function<void()> onLegArrived_)
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
    // leg0 走引擎 TileLoader;其余行自行请求(缓存优先,一次性)。
    for (std::size_t i = 1; i < legs.size(); ++i) {
        if (!fileSource) {
            legs[i].done = true;
            continue;
        }
        Resource resource = [&] {
            TileGridResourceBypass bypass;
            return Resource::tile(urlTemplate,
                                  pixelRatio,
                                  static_cast<int32_t>(x),
                                  static_cast<int32_t>(legs[i].row),
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
                sub.steps,
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

std::unique_ptr<TileDataSource> makeTileDataSource(std::shared_ptr<FileSource> fileSource,
                                                   const std::string& urlTemplate,
                                                   float pixelRatio,
                                                   uint8_t scheme,
                                                   const std::string& sourceID,
                                                   uint8_t z,
                                                   uint32_t x,
                                                   uint32_t y,
                                                   std::function<void()> onDataArrived) {
    // 目前只有 tile-grid 需要额外的数据源;其余情况上游走原生路径。
    if (!tileGridActive() || urlTemplate.empty()) {
        return nullptr;
    }
    return std::make_unique<TileGridDualFetch>(
        std::move(fileSource), urlTemplate, pixelRatio, scheme, sourceID, z, x, y, std::move(onDataArrived));
}

} // namespace extras
} // namespace mln
