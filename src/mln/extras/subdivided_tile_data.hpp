#pragma once

#include <mln/tile/geometry_tile_data.hpp>
#include <mln/util/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mln {
namespace extras {

namespace detail {

/// 裁剪矩形在瓦片范围外保留的缓冲,单位为瓦片坐标。
/// 取值需同时满足:足够大,使宽线条的端帽落在可视区之外不露馅;足够小,
/// 使 EXTENT + 缓冲 远在 int16 上限之内,裁剪结果永不溢出。
constexpr int32_t clipBuffer = 2048;
constexpr int32_t clipMin = -clipBuffer;
constexpr int32_t clipMax = util::EXTENT + clipBuffer;

using Point32 = std::pair<int32_t, int32_t>;

inline GeometryCoordinate narrow(const Point32& p) {
    return {static_cast<int16_t>(p.first), static_cast<int16_t>(p.second)};
}

/// Liang–Barsky:把线段裁到矩形内。返回 false 表示整条落在矩形外。
inline bool clipSegment(const Point32& a, const Point32& b, Point32& outA, Point32& outB) {
    const double dx = static_cast<double>(b.first) - a.first;
    const double dy = static_cast<double>(b.second) - a.second;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {static_cast<double>(a.first) - clipMin,
                         static_cast<double>(clipMax) - a.first,
                         static_cast<double>(a.second) - clipMin,
                         static_cast<double>(clipMax) - a.second};
    double t0 = 0.0;
    double t1 = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false; // 平行于该边且在外侧
            continue;
        }
        const double r = q[i] / p[i];
        if (p[i] < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    outA = {static_cast<int32_t>(std::llround(a.first + t0 * dx)),
            static_cast<int32_t>(std::llround(a.second + t0 * dy))};
    outB = {static_cast<int32_t>(std::llround(a.first + t1 * dx)),
            static_cast<int32_t>(std::llround(a.second + t1 * dy))};
    return true;
}

/// 把折线裁进矩形。一条折线可能被切成多段,因此结果直接追加到 out。
inline void clipLine(const std::vector<Point32>& ring, GeometryCollection& out) {
    if (ring.size() < 2) return;
    GeometryCoordinates current;
    const auto flush = [&] {
        if (current.size() > 1) out.push_back(std::move(current));
        current.clear();
    };
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
        Point32 a;
        Point32 b;
        if (!clipSegment(ring[i], ring[i + 1], a, b)) {
            flush(); // 该段完全在外,断开当前折线
            continue;
        }
        const auto a16 = narrow(a);
        const auto b16 = narrow(b);
        if (current.empty()) {
            current.push_back(a16);
        } else if (current.back() != a16) {
            flush(); // 与上一段不相接,说明中间被裁掉过
            current.push_back(a16);
        }
        current.push_back(b16);
    }
    flush();
}

/// Sutherland–Hodgman 的单边裁剪。side: 0=x>=bound 1=x<=bound 2=y>=bound 3=y<=bound。
inline void clipRingAgainstEdge(const std::vector<Point32>& in, std::vector<Point32>& out, int side, int32_t bound) {
    out.clear();
    if (in.empty()) return;
    const auto isInside = [&](const Point32& p) {
        switch (side) {
            case 0: return p.first >= bound;
            case 1: return p.first <= bound;
            case 2: return p.second >= bound;
            default: return p.second <= bound;
        }
    };
    // 只有一端在内一端在外时才会被调用,故对应轴上的差值必不为零。
    const auto intersect = [&](const Point32& a, const Point32& b) -> Point32 {
        if (side <= 1) {
            const double t = static_cast<double>(bound - a.first) / (static_cast<double>(b.first) - a.first);
            return {bound,
                    static_cast<int32_t>(std::llround(a.second + t * (static_cast<double>(b.second) - a.second)))};
        }
        const double t = static_cast<double>(bound - a.second) / (static_cast<double>(b.second) - a.second);
        return {static_cast<int32_t>(std::llround(a.first + t * (static_cast<double>(b.first) - a.first))), bound};
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        const Point32& cur = in[i];
        const Point32& prev = in[(i + in.size() - 1) % in.size()];
        const bool curIn = isInside(cur);
        const bool prevIn = isInside(prev);
        if (curIn) {
            if (!prevIn) out.push_back(intersect(prev, cur));
            out.push_back(cur);
        } else if (prevIn) {
            out.push_back(intersect(prev, cur));
        }
    }
}

/// 把多边形环裁进矩形,保持闭合与绕向。
inline void clipRing(const std::vector<Point32>& ring, GeometryCollection& out) {
    if (ring.size() < 3) return;
    std::vector<Point32> a = ring;
    std::vector<Point32> b;
    clipRingAgainstEdge(a, b, 0, clipMin);
    clipRingAgainstEdge(b, a, 1, clipMax);
    clipRingAgainstEdge(a, b, 2, clipMin);
    clipRingAgainstEdge(b, a, 3, clipMax);
    if (a.size() < 3) return;
    GeometryCoordinates result;
    result.reserve(a.size() + 1);
    for (const auto& p : a) {
        result.push_back(narrow(p));
    }
    if (result.front() != result.back()) {
        result.push_back(result.front()); // 环必须闭合
    }
    out.push_back(std::move(result));
}

} // namespace detail

/// 客户端"层级复用":一个瓦片直接渲染其祖先数据瓦片的几何——坐标按 2^steps
/// 放大,再平移到本瓦片对应的子矩形。纯仿射变换,不涉及重投影,因此无接缝。
///
/// 这是稀疏金字塔能够平滑过渡的关键:瓦片网格保持每级 2× 的稠密排列,只有
/// 数据来源是稀疏的。
///
/// 超出本子格的几何必须**裁剪**而不是把坐标钳到 int16 边界:放大 2^steps 倍后
/// 祖先瓦片的坐标可达几十万(steps=5 时约 28 万),钳制会把远端顶点全部拍到
/// 32767,线段方向随之被改写——那是凭空伪造几何,深层级下地图会碎成互相乱穿的
/// 色带。裁剪只保留矩形内的部分并补上正确的边界交点,顺带大幅减少顶点数。
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
        const int32_t loX = minX * scale - dx;
        const int32_t hiX = maxX * scale - dx;
        const int32_t loY = minY * scale - dy;
        const int32_t hiY = maxY * scale - dy;
        if (hiX < detail::clipMin || hiY < detail::clipMin || loX > detail::clipMax || loY > detail::clipMax) {
            transformed.emplace();
            return *transformed;
        }

        GeometryCollection out;
        out.reserve(source.size());

        // 整个要素都在裁剪矩形内(常见:要素完整落在本子格),直接变换即可。
        if (loX >= detail::clipMin && hiX <= detail::clipMax && loY >= detail::clipMin && hiY <= detail::clipMax) {
            for (const auto& ring : source) {
                GeometryCoordinates line;
                line.reserve(ring.size());
                for (const auto& p : ring) {
                    line.emplace_back(static_cast<int16_t>(p.x * scale - dx), static_cast<int16_t>(p.y * scale - dy));
                }
                out.push_back(std::move(line));
            }
            transformed = std::move(out);
            return *transformed;
        }

        // 跨越边界:按要素类型裁剪。
        const auto type = inner->getType();
        std::vector<detail::Point32> scaled;
        for (const auto& ring : source) {
            scaled.clear();
            scaled.reserve(ring.size());
            for (const auto& p : ring) {
                scaled.emplace_back(p.x * scale - dx, p.y * scale - dy);
            }
            switch (type) {
                case FeatureType::Polygon:
                    detail::clipRing(scaled, out);
                    break;
                case FeatureType::Point: {
                    GeometryCoordinates kept;
                    for (const auto& p : scaled) {
                        if (p.first >= detail::clipMin && p.first <= detail::clipMax && p.second >= detail::clipMin &&
                            p.second <= detail::clipMax) {
                            kept.push_back(detail::narrow(p));
                        }
                    }
                    if (!kept.empty()) out.push_back(std::move(kept));
                    break;
                }
                default: // LineString 及未知类型按折线处理
                    detail::clipLine(scaled, out);
                    break;
            }
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
