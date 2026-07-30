#pragma once

#include <mln/extras/extras.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

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
///
/// 同时认领 `extra: { "tile-origin": "center" }`:引擎的瓦片编号是左上角原点、
/// y 轴朝南、索引 0..2^z-1;百度官方网格以世界中心为原点、y 轴朝北、索引可负。
/// 两者只差一个平移加翻转:`X = x - 2^(z-1)`、`Y = 2^(z-1) - 1 - y`。平移与翻转
/// 都与四叉树运算可交换(子瓦片仍是 2p/2p+1),所以瓦片编号、父子关系、掩码
/// 全部无需改动——这正是能与百度瓦片做到 1:1 的原因。z=0 落在半块偏移上,
/// 数据源应声明 minzoom >= 1。
///
/// 同时认领 `extra: { "tile-size": 256 }`。引擎的矢量源把瓦片尺寸硬编码成
/// 512(只有栅格源解析 style 里的 tileSize),而百度、高德等国内数据是按
/// 256 设计的;不声明就会把 256 的瓦片拉到 512 渲染,细节层级差一级、线宽
/// 字号全部翻倍。声明后取瓦层级整体平移 log2(512/size),与引擎原生对 256
/// 栅格源的处理完全一致。
class TileSchemeExtension final : public Extension, private TileScheme {
public:
    /// 超过此层级一律透传;该上限远高于任何真实瓦片金字塔。
    static constexpr int32_t maxLevel = 30;

    TileSchemeExtension();

    std::vector<std::string> keys() const override { return {"tile-zoom", "tile-size", "tile-origin"}; }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    TileScheme* tileScheme() noexcept override {
        return (active.load(std::memory_order_relaxed) || sizeShift.load(std::memory_order_relaxed) != 0 ||
                centeredOrigin.load(std::memory_order_relaxed))
                   ? this
                   : nullptr;
    }

private:
    int32_t coveringZoom(int32_t level) const noexcept override;
    TileSubdivision subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept override;
    void translate(uint8_t z, int64_t& x, int64_t& y) const noexcept override;

    /// 以 canonical 层级为下标的数据层级;`identity` 表示该层级自带数据。
    /// 用原子元素而非加锁:该表在渲染路径上被读取、在 style 加载时被改写,
    /// 竞争的最坏结果只是某一帧混用了新旧两套映射。
    static constexpr int8_t identity = -1;
    std::array<std::atomic<int8_t>, maxLevel + 1> table;
    std::atomic<bool> active{false};
    /// log2(512/tileSize):256px 源为 +1,512px 源为 0。
    std::atomic<int8_t> sizeShift{0};
    /// 编号原点是否在世界中心(y 轴同时朝北),百度官方网格即如此。
    std::atomic<bool> centeredOrigin{false};
};

} // namespace extras
} // namespace mln
