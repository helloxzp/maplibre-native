#pragma once

#include <mln/extras/extras.hpp>
#include <mln/extras/subdivided_tile_data.hpp>
#include <mln/tile/geometry_tile_data.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mln {

class AsyncRequest;
class FileSource;

namespace extras {

/// `extra: { "tile-grid": "plate-carree" }` —— dual-fetch 架构。
///
/// 高德 JSAPI 矢量瓦片按度数网格索引(等距柱面:行线性于纬度),官方显示是墨卡
/// 托。mln 渲染器深度假设瓦片框各向同性(字形四边形、线宽挤出、AA 渐变、SDF
/// 采样都按两轴等比算),把度数瓦片直接摆进墨卡托世界会系统性发虚/斜切(真机
/// 验证过)。因此瓦片 ID 保持原生墨卡托槽位,渲染/覆盖/查询零改动,一切在数据
/// 层解决:
///
///   - 一张墨卡托瓦片的纬度带覆盖 2·cos(纬度) ≤ 2 个度数行(中国区 1.4~1.9,
///     错位时最多跨 3 行);
///   - leg0(覆盖北缘的度数行)沿用引擎自身的 TileLoader —— Resource::tile()
///     里经 tileGridLeg0Row() 把墨卡托行号换成该度数行;
///   - 其余行由 TileGridDualFetch(挂在 VectorMVTTile 上)自行请求;
///   - 到齐后合成:每个度数行先走稀疏层级子分(复用 SubdividedGeometryTileData
///     与兄弟共享索引),再经"度数-local → 墨卡托-local"的非线性 y warp 与裁剪
///     (复用 detail:: 裁剪机械),拼进墨卡托瓦片的本地框。
///
/// 与 `projection: "plate-carree"` 的区别:那个把显示也换成等距柱面(纵向按
/// cos(纬度) 压扁);本扩展显示保持墨卡托。两者不应同时声明。
class TileGridExtension final : public Extension, private TileGridMapper {
public:
    std::vector<std::string> keys() const override { return {"tile-grid"}; }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    TileGridMapper* tileGridMapper() noexcept override {
        return active.load(std::memory_order_relaxed) ? this : nullptr;
    }

    /// 归一化墨卡托 y(0=北缘)。纬度钳在 ±85.0511(墨卡托世界边缘)。
    static double mercNormY(double lat) noexcept;
    /// mercNormY 的逆(y 钳在 [0,1])。
    static double latFromMercNormY(double normY) noexcept;

private:
    void sourceRows(uint8_t z, uint32_t y, uint32_t& r0, uint32_t& r1) const noexcept override;

    std::atomic<bool> active{false};
};

/// sidecar 用 Resource::tile() 复用完整的 URL 组装(子分重映射/标签/模板替换),
/// 但那条路径会再次触发 leg0 的行号重映射;本 RAII 在当前线程内临时旁路它。
/// 当前线程是否处于旁路作用域(供 tileGridLeg0Row 查询)。
bool tileGridResourceBypassed() noexcept;

class TileGridResourceBypass {
public:
    TileGridResourceBypass();
    ~TileGridResourceBypass();
    TileGridResourceBypass(const TileGridResourceBypass&) = delete;
    TileGridResourceBypass& operator=(const TileGridResourceBypass&) = delete;
};

/// "度数行-local → 墨卡托瓦片-local"的 y warp + 裁剪装饰器。
///
/// 输入几何是某个度数行的瓦片内坐标(线性于纬度,EXTENT 制);输出映射进墨卡
/// 托瓦片的本地框并按 [-2048, EXTENT+2048] 裁剪(度数行的纬度带一般超出墨卡托
/// 瓦片的带,越界部分属于相邻墨卡托瓦片)。x 原样。y 单调,相邻瓦片共享边两侧
/// 算出同一条线,无接缝。
class DegreeRowToMercatorFeature final : public GeometryTileFeature {
public:
    /// φ(yLocal) = phiTop − yLocal·degPerUnit;
    /// y′ = (mercNormY(φ) − myA) · scaleY(单位:EXTENT)。
    struct Params {
        double phiTop;
        double degPerUnit;
        double myA;
        double scaleY;
    };

    DegreeRowToMercatorFeature(std::unique_ptr<GeometryTileFeature> inner_, const Params& params_)
        : inner(std::move(inner_)),
          params(params_) {}

    FeatureType getType() const override { return inner->getType(); }
    std::optional<Value> getValue(const std::string& key) const override { return inner->getValue(key); }
    const PropertyMap& getProperties() const override { return inner->getProperties(); }
    FeatureIdentifier getID() const override { return inner->getID(); }

    const GeometryCollection& getGeometries() const override;

private:
    std::unique_ptr<GeometryTileFeature> inner;
    const Params params;
    mutable std::optional<GeometryCollection> transformed;
};

class DegreeRowToMercatorLayer final : public GeometryTileLayer {
public:
    DegreeRowToMercatorLayer(std::unique_ptr<GeometryTileLayer> inner_, const DegreeRowToMercatorFeature::Params& params_)
        : inner(std::move(inner_)),
          params(params_) {}

    std::size_t featureCount() const override { return inner->featureCount(); }
    std::unique_ptr<GeometryTileFeature> getFeature(std::size_t i) const override {
        return std::make_unique<DegreeRowToMercatorFeature>(inner->getFeature(i), params);
    }
    std::string getName() const override { return inner->getName(); }

private:
    std::unique_ptr<GeometryTileLayer> inner;
    const DegreeRowToMercatorFeature::Params params;
};

class DegreeRowToMercatorTileData final : public GeometryTileData {
public:
    DegreeRowToMercatorTileData(std::unique_ptr<GeometryTileData> inner_, const DegreeRowToMercatorFeature::Params& params_)
        : inner(std::move(inner_)),
          params(params_) {}

    std::unique_ptr<GeometryTileData> clone() const override {
        return std::make_unique<DegreeRowToMercatorTileData>(inner->clone(), params);
    }
    std::unique_ptr<GeometryTileLayer> getLayer(const std::string& name) const override {
        auto layer = inner->getLayer(name);
        if (!layer) return nullptr;
        return std::make_unique<DegreeRowToMercatorLayer>(std::move(layer), params);
    }

private:
    std::unique_ptr<GeometryTileData> inner;
    const DegreeRowToMercatorFeature::Params params;
};

/// 多个内层数据的拼合视图:getLayer 返回把各内层同名图层按序连接的图层。
class ComposedGeometryTileData final : public GeometryTileData {
public:
    explicit ComposedGeometryTileData(std::vector<std::unique_ptr<GeometryTileData>> inners_)
        : inners(std::move(inners_)) {}

    std::unique_ptr<GeometryTileData> clone() const override;
    std::unique_ptr<GeometryTileLayer> getLayer(const std::string& name) const override;

private:
    std::vector<std::unique_ptr<GeometryTileData>> inners;
};

/// 墨卡托瓦片的多度数行取瓦 sidecar。
///
/// leg0(覆盖北缘的度数行)由引擎自身的 TileLoader 走完整生命周期(缓存、重试、
/// 过期刷新),字节经 setLeg0() 注入;其余行由本类经 FileSource 一次性请求
/// (LoadingMethod::All)。全部到位(或失败置空)后 ready() 为真,build() 产出
/// 合成数据。所有回调都在创建线程(Map 线程)上,无需加锁。
class TileGridDualFetch final : public TileDataSource {
public:
    TileGridDualFetch(std::shared_ptr<FileSource> fileSource,
                      std::string urlTemplate,
                      float pixelRatio,
                      uint8_t scheme, ///< Tileset::Scheme 的整型值(避免头依赖)
                      std::string sourceID,
                      uint8_t z,
                      uint32_t x,
                      uint32_t yMercator,
                      std::function<void()> onLegArrived);
    ~TileGridDualFetch();

    /// 注入 leg0(引擎 TileLoader 的字节;nullptr = 空瓦片/无数据)。
    void setPrimaryData(std::shared_ptr<const std::string> data) override;

    /// leg0 已注入且其余行都已返回(成功或失败)。
    bool ready() const noexcept override;

    /// 合成当前已有的各 leg。至少 leg0 已注入时可调用;失败的 leg 跳过。
    /// 返回 nullptr 表示所有 leg 都为空。
    std::unique_ptr<GeometryTileData> build() const override;

private:
    struct Leg {
        uint32_t row = 0;
        bool done = false;
        std::shared_ptr<const std::string> data;
        std::unique_ptr<AsyncRequest> request;
    };

    std::shared_ptr<FileSource> fileSource;
    std::string urlTemplate;
    float pixelRatio;
    uint8_t scheme;
    std::string sourceID;
    uint8_t z;
    uint32_t x;
    uint32_t yMercator;
    std::function<void()> onLegArrived;

    bool leg0Set = false;
    std::vector<Leg> legs; ///< legs[0] = leg0(无 request)
};

} // namespace extras
} // namespace mln
