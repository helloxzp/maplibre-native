#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/// 引擎侧 hook 调用点包含的头文件。
///
/// 这里刻意不引入 rapidjson 等配置期依赖,也不暴露任何具体扩展的类型:被 hook
/// 的上游文件**只需 include 本头文件**,看到的全是"当前是否有扩展生效"与不透明
/// 接口。配置与注册在 extras.hpp,各扩展的实现细节在各自的 .hpp 中。
namespace mln {

class GeometryTileData;
class FileSource;

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

    /// 把四叉树层级翻成数据源在 URL 里使用的层级标签。
    ///
    /// 两者不一定相等:百度的 z 只是标签,与分辨率没有固定倍率关系
    /// (z {3,5,7,9,10,12,15,17,19} 对应的四叉树层级是 {3,4,6,8,10,11,14,15,17}),
    /// 而子分步数必须按四叉树层级算,所以标签要单独映射。默认恒等。
    virtual int32_t levelId(uint8_t z) const noexcept = 0;

    /// 把引擎的瓦片编号换算成数据源的编号(原点位置与 y 轴朝向的约定)。
    /// 默认恒等;百度一类以世界中心为原点、y 轴朝北的网格需要平移加翻转。
    virtual void translate(uint8_t z, int64_t& x, int64_t& y) const noexcept = 0;

    /// 渲染清单是否改由"瓦片桶的存在"决定(extra.layer-zoom-gate: "tile")。
    /// 只适用于 per-level 样式表;常规 zoom 门控的样式必须保持 false(默认)。
    virtual bool layerZoomGateByTile() const noexcept { return false; }
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

/// 瓦片数据源:把"一张引擎瓦片的字节 → 可渲染的 GeometryTileData"这一步交给
/// extras,使上游不必知道任何扩展类型。
///
/// 无扩展生效时 `makeTileDataSource()` 返回 nullptr,上游走原生路径;返回非空
/// 时上游把 TileLoader 拿到的字节交给 `setPrimaryData()`,`ready()` 为真后调
/// `build()` 取合成结果。实现可以在内部再发请求(dual-fetch 的其余度数行),
/// 到货时通过构造时传入的回调通知上游。
class TileDataSource {
public:
    virtual ~TileDataSource() = default;

    /// 注入引擎 TileLoader 取到的字节(nullptr = 空瓦片)。
    virtual void setPrimaryData(std::shared_ptr<const std::string> data) = 0;

    /// 全部所需数据是否到位(成功或失败均计)。
    virtual bool ready() const noexcept = 0;

    /// 合成当前已有数据;全空时返回 nullptr。
    virtual std::unique_ptr<GeometryTileData> build() const = 0;
};

/// 稀疏层级复用的瓦片数据适配:本瓦片取的是祖先数据瓦片时(steps>0)包一层
/// 子分装饰器(几何按子格仿射 + 裁剪,兄弟共享祖先包围盒索引);否则原样返回。
std::unique_ptr<GeometryTileData> adaptTileData(
    std::unique_ptr<GeometryTileData> data, uint8_t z, uint32_t x, uint32_t y, const std::string& sourceID);

/// 为一张引擎瓦片创建数据源;不需要额外处理时返回 nullptr(上游走原生路径)。
/// `onDataArrived` 在实现内部异步取到数据后于同一线程回调。
std::unique_ptr<TileDataSource> makeTileDataSource(std::shared_ptr<FileSource> fileSource,
                                                   const std::string& urlTemplate,
                                                   float pixelRatio,
                                                   uint8_t scheme,
                                                   const std::string& sourceID,
                                                   uint8_t z,
                                                   uint32_t x,
                                                   uint32_t y,
                                                   std::function<void()> onDataArrived);

/// 取瓦网格与显示世界不是同一套坐标时的数据层适配(dual-fetch 架构)。
///
/// 高德 JSAPI 矢量瓦片按**度数网格**索引(等距柱面,行线性于纬度),官方显示是
/// **墨卡托**。mln 的渲染器深度假设瓦片框各向同性(字形四边形、线宽挤出、AA
/// 渐变、SDF 采样都在瓦片单位里按两轴等比算),把度数瓦片直接摆进墨卡托世界
/// 会系统性发虚/斜切(真机验证过)。因此本扩展**不碰渲染侧**:瓦片 ID 保持
/// 原生墨卡托槽位,一切在数据层解决——
///   - 一张墨卡托瓦片的纬度带覆盖 2·cos(纬度) ≤ 2 个度数行(中国区约 1.4~1.9,
///     错位时最多跨 3 行),加载层对同列的这 1~3 个度数行各发一个请求;
///   - 到齐后在 setData 层合成:每个度数行先走稀疏层级子分(复用现有机械),
///     再经"度数-local → 墨卡托-local"的非线性 y warp 与裁剪,拼进墨卡托瓦
///     片的本地框(见 tile_grid.hpp);
///   - 渲染、覆盖、查询、符号全部走原生路径,零改动。
///
/// 首行(leg0)沿用引擎自身的 TileLoader:Resource::tile() 里把墨卡托行号换成
/// 覆盖其北缘的度数行;其余行由 extras 的 sidecar 自行请求。
class TileGridMapper {
public:
    virtual ~TileGridMapper() = default;

    /// 墨卡托瓦片 (z,y) 的纬度带覆盖的度数行区间 [r0,r1](闭区间,已钳制)。
    virtual void sourceRows(uint8_t z, uint32_t y, uint32_t& r0, uint32_t& r1) const noexcept = 0;
};

/// 当前是否有取瓦网格映射生效。
bool tileGridActive() noexcept;

/// leg0:墨卡托行 y 的北缘所在度数行;无扩展生效时原样返回 y。
/// Resource::tile() 在子分重映射之前调用。
uint32_t tileGridLeg0Row(uint8_t z, uint32_t y) noexcept;

/// 墨卡托瓦片覆盖的度数行区间;无扩展生效时 r0=r1=y。
void tileGridSourceRows(uint8_t z, uint32_t y, uint32_t& r0, uint32_t& r1) noexcept;

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

/// 数据实际覆盖的地理范围。
///
/// 归一化世界方块不一定被数据填满:BD09MC 为了让百度网格退化成标准四叉树,
/// 世界边长取 2^26 米,而数据只占其中宽 59.7%(经度 ±180°)、高 38.4%
/// (纬度 ±74°)。相机若按整块方块约束,缩到最小时四周会露出大片空白。
///
/// 用裸 double 而非 LatLngBounds,使本头文件不引入任何依赖。
class WorldExtent {
public:
    virtual ~WorldExtent() = default;

    /// 数据覆盖的经纬度范围,基准面与引擎对外 API 一致(WGS-84)。
    virtual void bounds(double& west, double& south, double& east, double& north) const noexcept = 0;
};

/// 数据在归一化世界方块中占据的矩形,原点在西北角,y 轴朝南。
struct WorldBounds {
    double x0 = 0.0; ///< 西
    double y0 = 0.0; ///< 北
    double x1 = 1.0; ///< 东
    double y1 = 1.0; ///< 南
};

/// 应用当前生效的取瓦层级钳制,没有扩展生效时原样返回 `level`。
/// 热路径:每帧每个数据源调用一次,无扩展时仅一次原子读取加一次分支。
int32_t mapCoveringZoom(int32_t level) noexcept;

/// 查询瓦片的数据来源。无扩展生效时返回 `{z, 0, 0, 0}`,即本层级直接取数据。
TileSubdivision tileSubdivision(uint8_t z, uint32_t x, uint32_t y) noexcept;

/// 把四叉树层级翻成数据源的层级标签;无扩展生效时原样返回。
int32_t tileLevelId(uint8_t z) noexcept;

/// 按当前生效的编号约定换算瓦片编号;无扩展生效时原样返回。
void translateTileCoords(uint8_t z, int64_t& x, int64_t& y) noexcept;

/// 当前生效的基准面偏移;没有扩展生效时返回 nullptr(调用方走原生路径)。
const DatumShift* datumShift() noexcept;

/// 当前生效的平面投影;没有扩展生效时返回 nullptr(调用方用原生 Web 墨卡托)。
const PlanarProjection* planarProjection() noexcept;

/// 当前生效的世界范围,已换算成归一化坐标;没有扩展生效时返回整块 {0,0,1,1},
/// 相机约束因此与原版逐位相同。
WorldBounds worldBounds() noexcept;

/// world-bounds 生效时经度回卷必须禁用:数据矩形不是 360° 周期的,把越界经度
/// 按 ±360 回卷会把相机传送到另一侧(边界处重滑即"从左边跑到右边")。
/// 无扩展生效时返回 false,回卷行为与原版完全一致。
bool worldWrapDisabled() noexcept;

/// 稀疏层级方案生效时,渲染侧"按相机 zoom 隐藏图层"的闸门必须禁用。
///
/// 桶本来就是按【瓦片自身层级】建的(geometry_tile.cpp 的 layout 过滤),每张
/// 瓦片只持有自己那级图层的桶、配色在解析期烘焙——样式天然是瓦片的属性。而
/// 渲染清单却按【相机 zoom】筛图层:跨数据级的瞬间,引擎 retain 旧级瓦片顶着
/// 渲染,旧级图层已被相机 zoom 踢出清单,瓦片里颜色正确的桶无人问津,画面上
/// 整片要素消失(闪白)。放开闸门后由"哪些瓦片有它的桶"决定图层画在哪,
/// 过渡期旧级瓦片继续由旧级图层正确着色。
/// 无扩展生效时返回 false,渲染行为与原版完全一致。
bool renderZoomGateDisabled() noexcept;

} // namespace extras
} // namespace mln
