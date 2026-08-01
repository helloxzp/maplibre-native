#pragma once

#include <mln/tile/geometry_tile_data.hpp>
#include <mln/util/constants.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mln {
namespace extras {

/// "缩小 + 平移" —— `SubdividedGeometryTileData` 的镜像。
///
/// 用在**负子分**上:本瓦片比数据层级更粗,于是它**包含** 2^k × 2^k 张数据瓦片,
/// 每张源瓦片整幅落进目标的一个子格:
///
///     x' = (x >> k) + offX · (EXTENT >> k)
///
/// 为什么需要它:高德最粗的数据是 z3,显示层级 1/2 拿不到不比自己更粗的数据。
/// 从前的做法是让 `coveringZoom` 把取瓦层级**抬高**到 3 —— 但那会把显示层级
/// 0/1/2/3 全塌到同一批 z3 瓦片上,而 `SymbolLayout` 对每张瓦片只求值一次图层
/// filter,于是那几级共用同一个 `["zoom"]`,按层级的门在原理上分不开。保持网格
/// 稠密(每级各有自己的瓦片)才能让 `overscaledZ` 恒等于显示层级。
///
/// **不做裁剪**:各子格互不重叠,子格之间不需要切。唯一要收的是源瓦片的
/// **缓冲带** —— 超出 `[0, EXTENT)` 的部分是邻格内容的副本,不收的话同一个标注
/// 会在相邻两个子格里各出现一次。线和面的重复落在**完全相同**的位置(道路不
/// 透明、region 关了抗锯齿),看不出来,而且原样通过才保得住缓冲带里的连续性;
/// 所以只对**点**收边。
class MergedGeometryTileFeature final : public GeometryTileFeature {
public:
    /// `shift` = k(层级差),`dx`/`dy` = 本子格在目标瓦片内的偏移(EXTENT 制)。
    MergedGeometryTileFeature(std::unique_ptr<GeometryTileFeature> inner_, int32_t shift_, int32_t dx_, int32_t dy_)
        : inner(std::move(inner_)),
          shift(shift_),
          dx(dx_),
          dy(dy_) {}

    FeatureType getType() const override { return inner->getType(); }
    std::optional<Value> getValue(const std::string& key) const override { return inner->getValue(key); }
    const PropertyMap& getProperties() const override { return inner->getProperties(); }
    FeatureIdentifier getID() const override { return inner->getID(); }

    const GeometryCollection& getGeometries() const override {
        if (transformed) {
            return *transformed;
        }
        const auto& source = inner->getGeometries();
        const bool isPoint = inner->getType() == FeatureType::Point;

        GeometryCollection out;
        out.reserve(source.size());
        for (const auto& ring : source) {
            GeometryCoordinates mapped;
            mapped.reserve(ring.size());
            for (const auto& p : ring) {
                // 点:只留落在本子格自己那一格里的,把缓冲带里的副本丢掉。
                if (isPoint && (p.x < 0 || p.x >= util::EXTENT || p.y < 0 || p.y >= util::EXTENT)) {
                    continue;
                }
                // 算术右移:负坐标(缓冲带)向下取整,与相邻子格算出同一条边,无缝。
                mapped.emplace_back(static_cast<int16_t>((p.x >> shift) + dx),
                                    static_cast<int16_t>((p.y >> shift) + dy));
            }
            if (!mapped.empty()) {
                out.push_back(std::move(mapped));
            }
        }
        transformed = std::move(out);
        return *transformed;
    }

private:
    std::unique_ptr<GeometryTileFeature> inner;
    const int32_t shift;
    const int32_t dx;
    const int32_t dy;
    mutable std::optional<GeometryCollection> transformed;
};

class MergedGeometryTileLayer final : public GeometryTileLayer {
public:
    MergedGeometryTileLayer(std::unique_ptr<GeometryTileLayer> inner_, int32_t shift_, int32_t dx_, int32_t dy_)
        : inner(std::move(inner_)),
          shift(shift_),
          dx(dx_),
          dy(dy_) {}

    std::size_t featureCount() const override { return inner->featureCount(); }
    std::unique_ptr<GeometryTileFeature> getFeature(std::size_t i) const override {
        return std::make_unique<MergedGeometryTileFeature>(inner->getFeature(i), shift, dx, dy);
    }
    std::string getName() const override { return inner->getName(); }

private:
    std::unique_ptr<GeometryTileLayer> inner;
    const int32_t shift;
    const int32_t dx;
    const int32_t dy;
};

class MergedGeometryTileData final : public GeometryTileData {
public:
    /// `steps` 为层级差 k(正数),`offX`/`offY` 为本数据瓦片在目标瓦片内的子格坐标
    /// (0..2^k-1)。
    MergedGeometryTileData(std::unique_ptr<GeometryTileData> inner_, uint8_t steps, uint32_t offX, uint32_t offY)
        : inner(std::move(inner_)),
          shift(steps),
          dx(static_cast<int32_t>(offX) * (util::EXTENT >> steps)),
          dy(static_cast<int32_t>(offY) * (util::EXTENT >> steps)) {}

    std::unique_ptr<GeometryTileData> clone() const override {
        return std::unique_ptr<GeometryTileData>(new MergedGeometryTileData(CloneTag{}, inner->clone(), shift, dx, dy));
    }

    std::unique_ptr<GeometryTileLayer> getLayer(const std::string& name) const override {
        auto layer = inner->getLayer(name);
        if (!layer) return nullptr;
        return std::make_unique<MergedGeometryTileLayer>(std::move(layer), shift, dx, dy);
    }

private:
    struct CloneTag {};
    MergedGeometryTileData(CloneTag, std::unique_ptr<GeometryTileData> inner_, int32_t shift_, int32_t dx_, int32_t dy_)
        : inner(std::move(inner_)),
          shift(shift_),
          dx(dx_),
          dy(dy_) {}

    std::unique_ptr<GeometryTileData> inner;
    const int32_t shift;
    const int32_t dx;
    const int32_t dy;
};

} // namespace extras
} // namespace mln
