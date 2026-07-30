#pragma once

#include <cstdint>

/// 引擎侧 hook 调用点包含的头文件。
///
/// 这里刻意不引入 rapidjson 等配置期依赖:被 hook 的上游文件只需要看到
/// "当前是否有扩展生效"这一件事,配置与注册相关的接口在 extras.hpp 中。
namespace mln {
namespace extras {

/// 一个瓦片相对其数据来源瓦片的关系。
///
/// 稀疏金字塔中,瓦片网格保持每级 2× 的稠密排列(过渡才能平滑),而数据只在
/// 部分层级存在,因此中间层级的瓦片改为取用祖先数据瓦片、再做仿射变换。
struct TileSubdivision {
    uint8_t dataZ = 0;  ///< 实际请求的数据层级
    uint8_t steps = 0;  ///< 层级差 z - dataZ;为 0 表示本层级自带数据,无需变换
    uint32_t offX = 0;  ///< 本瓦片在数据瓦片内的子格坐标(0..2^steps-1)
    uint32_t offY = 0;
};

/// 稀疏瓦片金字塔的层级方案。
///
/// 由 Extension 安装,在下列 hook 点被读取:
///   - `util::coveringZoomLevel()`  取瓦层级下限钳制
///   - `Resource::tile()`           瓦片 URL 改指数据层级祖先
///   - `VectorMVTTile::setData()`   几何按子格做仿射变换
class TileScheme {
public:
    virtual ~TileScheme() = default;

    /// `level` 是引擎原生公式算出的层级。仅在该层级比最粗的数据层级还粗时
    /// 才需要抬高(无法由更粗的数据细分而来);其余情况原样返回,保持网格稠密。
    /// 必须 noexcept:调用方本身处于 noexcept 路径。
    virtual int32_t coveringZoom(int32_t level) const noexcept = 0;

    /// 查询 canonical 瓦片 (z,x,y) 的数据来源。
    virtual TileSubdivision subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept = 0;

    /// 把引擎的瓦片编号换算成数据源的编号(原点位置与 y 轴朝向的约定)。
    /// 默认恒等;百度一类以世界中心为原点、y 轴朝北的网格需要平移加翻转。
    virtual void translate(uint8_t z, int64_t& x, int64_t& y) const noexcept = 0;
};

/// 经纬度基准面偏移。
///
/// 国内瓦片常以 GCJ-02(火星坐标)或 BD09 为基准面,而引擎对外的 API 一律是
/// WGS-84。这里在"经纬度 ↔ 墨卡托像素"这一层做一次基准面换算,瓦片编号、
/// 投影公式、渲染管线全部保持原样。
///
/// 用裸 double 而非 LatLng,使本头文件不引入任何依赖。
class DatumShift {
public:
    virtual ~DatumShift() = default;

    /// WGS-84 → 瓦片基准面。就地修改。
    virtual void forward(double& lng, double& lat) const noexcept = 0;

    /// 瓦片基准面 → WGS-84。就地修改。
    virtual void inverse(double& lng, double& lat) const noexcept = 0;
};

/// 经纬度到归一化世界坐标的平面投影。
///
/// 引擎默认用 Web 墨卡托;声明本 hook 可整体替换投影公式,例如百度官方瓦片
/// 用的 BD09MC——纬度经分段多项式映射,与 log-tan 完全不同。
///
/// 与 DatumShift 正交:偏移负责"WGS-84 → 数据基准面的经纬度",本 hook 负责
/// "基准面经纬度 → 平面",两者可自由组合。
///
/// 归一化坐标为 [0,1]²,原点在西北角,与引擎内部约定一致。
class PlanarProjection {
public:
    virtual ~PlanarProjection() = default;

    virtual void project(double lng, double lat, double& x, double& y) const noexcept = 0;
    virtual void unproject(double x, double y, double& lng, double& lat) const noexcept = 0;
};

/// 应用当前生效的取瓦层级钳制,没有扩展生效时原样返回 `level`。
/// 热路径:每帧每个数据源调用一次,无扩展时仅一次原子读取加一次分支。
int32_t mapCoveringZoom(int32_t level) noexcept;

/// 查询瓦片的数据来源。无扩展生效时返回 `{z, 0, 0, 0}`,即本层级直接取数据。
TileSubdivision tileSubdivision(uint8_t z, uint32_t x, uint32_t y) noexcept;

/// 按当前生效的编号约定换算瓦片编号;无扩展生效时原样返回。
void translateTileCoords(uint8_t z, int64_t& x, int64_t& y) noexcept;

/// 当前生效的基准面偏移;没有扩展生效时返回 nullptr(调用方走原生路径)。
const DatumShift* datumShift() noexcept;

/// 当前生效的平面投影;没有扩展生效时返回 nullptr(调用方用原生 Web 墨卡托)。
const PlanarProjection* planarProjection() noexcept;

} // namespace extras
} // namespace mln
