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
};

/// 应用当前生效的取瓦层级钳制,没有扩展生效时原样返回 `level`。
/// 热路径:每帧每个数据源调用一次,无扩展时仅一次原子读取加一次分支。
int32_t mapCoveringZoom(int32_t level) noexcept;

/// 查询瓦片的数据来源。无扩展生效时返回 `{z, 0, 0, 0}`,即本层级直接取数据。
TileSubdivision tileSubdivision(uint8_t z, uint32_t x, uint32_t y) noexcept;

} // namespace extras
} // namespace mln
