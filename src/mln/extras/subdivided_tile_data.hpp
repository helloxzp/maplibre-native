#pragma once

#include <mln/tile/geometry_tile_data.hpp>
#include <mln/util/constants.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace mln {
namespace extras {

/// 客户端"层级复用":一个瓦片直接渲染其祖先数据瓦片的几何——坐标按 2^steps
/// 放大,再平移到本瓦片对应的子矩形。纯仿射变换,不涉及重投影,因此无接缝。
///
/// 这是稀疏金字塔能够平滑过渡的关键:瓦片网格保持每级 2× 的稠密排列,只有
/// 数据来源是稀疏的。完全落在(带缓冲的)瓦片范围之外的要素会被丢弃,以免
/// 祖先瓦片的全量几何撑大每个子瓦片的 bucket。
class SubdividedGeometryTileFeature final : public GeometryTileFeature {
public:
    SubdividedGeometryTileFeature(std::unique_ptr<GeometryTileFeature> inner_, int32_t scale_, int32_t dx_, int32_t dy_)
        : inner(std::move(inner_)),
          scale(scale_),
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

        // 第一遍只在源坐标上求包围盒:纯比较,不做乘法、不分配。
        // 稀疏金字塔下一张祖先瓦片会被 2^steps × 2^steps 个子瓦片复用
        // (steps=2 即 16 个),绝大多数要素落在本子格之外,先判掉就省下
        // 整趟坐标变换和两级 vector 分配。
        int32_t minX = std::numeric_limits<int32_t>::max();
        int32_t minY = std::numeric_limits<int32_t>::max();
        int32_t maxX = std::numeric_limits<int32_t>::min();
        int32_t maxY = std::numeric_limits<int32_t>::min();
        for (const auto& ring : source) {
            for (const auto& p : ring) {
                minX = std::min<int32_t>(minX, p.x);
                minY = std::min<int32_t>(minY, p.y);
                maxX = std::max<int32_t>(maxX, p.x);
                maxY = std::max<int32_t>(maxY, p.y);
            }
        }
        if (maxX < minX) { // 空要素
            transformed.emplace();
            return *transformed;
        }

        // 变换是单调的(scale > 0),源包围盒可直接映射成目标包围盒,
        // 于是整个要素只需 4 次乘法,而不是每个点 2 次。
        constexpr int32_t cullBuffer = 1024; // 超出 extent 多少瓦片单位仍保留
        const int32_t loX = minX * scale - dx;
        const int32_t hiX = maxX * scale - dx;
        const int32_t loY = minY * scale - dy;
        const int32_t hiY = maxY * scale - dy;
        if (hiX < -cullBuffer || hiY < -cullBuffer || loX > util::EXTENT + cullBuffer ||
            loY > util::EXTENT + cullBuffer) {
            transformed.emplace();
            return *transformed;
        }

        // 只有横跨到 int16 之外的要素才需要逐点钳制;完整落在本子格内的
        // 要素(最常见)走免钳制快路径。
        constexpr int32_t lo16 = std::numeric_limits<int16_t>::min();
        constexpr int32_t hi16 = std::numeric_limits<int16_t>::max();
        const bool needsClamp = loX < lo16 || hiX > hi16 || loY < lo16 || hiY > hi16;

        GeometryCollection out;
        out.reserve(source.size());
        for (const auto& ring : source) {
            GeometryCoordinates line;
            line.reserve(ring.size());
            if (needsClamp) {
                for (const auto& p : ring) {
                    line.emplace_back(
                        static_cast<int16_t>(std::clamp(p.x * scale - dx, lo16, hi16)),
                        static_cast<int16_t>(std::clamp(p.y * scale - dy, lo16, hi16)));
                }
            } else {
                for (const auto& p : ring) {
                    line.emplace_back(static_cast<int16_t>(p.x * scale - dx),
                                      static_cast<int16_t>(p.y * scale - dy));
                }
            }
            out.push_back(std::move(line));
        }
        transformed = std::move(out);
        return *transformed;
    }

private:
    std::unique_ptr<GeometryTileFeature> inner;
    const int32_t scale;
    const int32_t dx;
    const int32_t dy;
    mutable std::optional<GeometryCollection> transformed;
};

class SubdividedGeometryTileLayer final : public GeometryTileLayer {
public:
    SubdividedGeometryTileLayer(std::unique_ptr<GeometryTileLayer> inner_, int32_t scale_, int32_t dx_, int32_t dy_)
        : inner(std::move(inner_)),
          scale(scale_),
          dx(dx_),
          dy(dy_) {}

    std::size_t featureCount() const override { return inner->featureCount(); }
    std::unique_ptr<GeometryTileFeature> getFeature(std::size_t i) const override {
        return std::make_unique<SubdividedGeometryTileFeature>(inner->getFeature(i), scale, dx, dy);
    }
    std::string getName() const override { return inner->getName(); }

private:
    std::unique_ptr<GeometryTileLayer> inner;
    const int32_t scale;
    const int32_t dx;
    const int32_t dy;
};

class SubdividedGeometryTileData final : public GeometryTileData {
public:
    /// `steps` 为本瓦片相对数据瓦片的层级差,`offX/offY` 为本瓦片在祖先瓦片
    /// 内的子格坐标(0..2^steps-1)。
    SubdividedGeometryTileData(std::unique_ptr<GeometryTileData> inner_, uint8_t steps, uint32_t offX, uint32_t offY)
        : inner(std::move(inner_)),
          scale(1 << steps),
          dx(static_cast<int32_t>(offX) * util::EXTENT),
          dy(static_cast<int32_t>(offY) * util::EXTENT) {}

    SubdividedGeometryTileData(std::unique_ptr<GeometryTileData> inner_, int32_t scale_, int32_t dx_, int32_t dy_)
        : inner(std::move(inner_)),
          scale(scale_),
          dx(dx_),
          dy(dy_) {}

    std::unique_ptr<GeometryTileData> clone() const override {
        return std::make_unique<SubdividedGeometryTileData>(inner->clone(), scale, dx, dy);
    }

    std::unique_ptr<GeometryTileLayer> getLayer(const std::string& name) const override {
        auto layer = inner->getLayer(name);
        if (!layer) return nullptr;
        return std::make_unique<SubdividedGeometryTileLayer>(std::move(layer), scale, dx, dy);
    }

private:
    std::unique_ptr<GeometryTileData> inner;
    const int32_t scale;
    const int32_t dx;
    const int32_t dy;
};

} // namespace extras
} // namespace mln
