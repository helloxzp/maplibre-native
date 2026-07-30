#pragma once

#include <mln/extras/extras.hpp>

#include <array>
#include <atomic>
#include <cstdint>

namespace mln {
namespace extras {

/// `extra: { "tile-zoom": { "2": 3, "3": 3, "4": 3, "5": 5 } }`
///
/// 声明一个稀疏瓦片金字塔:键是 canonical 瓦片层级,值是该层级实际取数据的
/// 层级;对象中未列出的层级按"本层级自带数据"处理。
///
/// 平滑过渡的关键在于 **网格稠密、数据稀疏**:canonical 网格仍是每级 2× 的
/// 标准四叉树,相机连续缩放时每一级都有自己的瓦片;只是中间层级的瓦片改为
/// 取用祖先数据瓦片的几何,按子格做仿射变换(见 subdivided_tile_data.hpp)。
/// 若反过来把取瓦层级直接映射到稀疏层级,网格本身就变稀疏了,缩放时会出现
/// "上一级被拉大数倍、下一级才突然出现"的跳变。
///
/// 值比键更细时无法由更粗的数据细分而来(那需要合并多张瓦片),这种条目被
/// 当作层级下限:取瓦层级会被抬高到该值。
class TileZoomExtension final : public Extension, private TileScheme {
public:
    /// 超过此层级一律透传;该上限远高于任何真实瓦片金字塔。
    static constexpr int32_t maxLevel = 30;

    TileZoomExtension();

    std::string key() const override { return "tile-zoom"; }
    bool configure(const JSValue& value) override;
    void reset() override;
    TileScheme* tileScheme() noexcept override { return active.load(std::memory_order_relaxed) ? this : nullptr; }

private:
    int32_t coveringZoom(int32_t level) const noexcept override;
    TileSubdivision subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept override;

    /// 以 canonical 层级为下标的数据层级;`identity` 表示该层级自带数据。
    /// 用原子元素而非加锁:该表在渲染路径上被读取、在 style 加载时被改写,
    /// 竞争的最坏结果只是某一帧混用了新旧两套映射。
    static constexpr int8_t identity = -1;
    std::array<std::atomic<int8_t>, maxLevel + 1> table;
    std::atomic<bool> active{false};
};

} // namespace extras
} // namespace mln
