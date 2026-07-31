#pragma once

#include <mln/extras/extras.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace mln {
namespace extras {

/// ```json
/// "tile-levels": [
///   { "zoom": 3,  "id": 3  },
///   { "zoom": 4,  "id": 5  },
///   { "zoom": 15, "id": 17, "from": 16 }
/// ]
/// ```
///
/// 声明一个稀疏瓦片金字塔:数组里**一条 = 一个真实存在的数据层级**。
///   - `zoom` 该层数据所在的四叉树层级(必填)
///   - `id`   它在 URL 的 `{z}` 位置上写什么(选填,默认等于 `zoom`)
///   - `from` 从哪个 canonical 层级开始启用这一条(选填,默认等于 `zoom`)
///
/// canonical 层级 n 使用 `from <= n` 中 `from` 最大的那一条;比最小的 `from`
/// 还粗的层级一律落到最粗的那一条(所以不必特地写 `"from": 0`)。
///
/// `id` 单列是因为数据源在 URL 里用的层级标签不一定等于四叉树层级——百度的 z
/// 只是标签,与分辨率没有固定倍率关系(z {3,5,7,9,10,12,15,17,19} 对应四叉树
/// {3,4,6,8,10,11,14,15,17}),而子分步数必须按四叉树层级算,两者不能混。
///
/// `from` 单列是为了表达"某一级数据故意多服务几级"。默认 `from == zoom` 时它
/// 就是纯粹的"有数据就用";百度官方引擎在 canonical 15 / 17 两处刻意晚一级
/// 切换(`from` 分别是 16 / 18),照抄即可,不必再造一套阈值表。
///
/// 平滑过渡的关键在于 **网格稠密、数据稀疏**:canonical 网格仍是每级 2× 的
/// 标准四叉树,相机连续缩放时每一级都有自己的瓦片;只是中间层级的瓦片改为
/// 取用祖先数据瓦片的几何,按子格做仿射变换(见 subdivided_tile_data.hpp)。
/// 若反过来把取瓦层级直接映射到稀疏层级,网格本身就变稀疏了,缩放时会出现
/// "上一级被拉大数倍、下一级才突然出现"的跳变。
///
/// 数据层级比 canonical 层级更细时无法由更粗的数据细分而来(那需要合并多张
/// 瓦片),这种情况被当作层级下限:取瓦层级会被抬高到该值。
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

    std::vector<std::string> keys() const override {
        return {"tile-levels", "tile-size", "tile-origin", "layer-zoom-gate"};
    }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    TileScheme* tileScheme() noexcept override {
        return (active.load(std::memory_order_relaxed) || sizeShift.load(std::memory_order_relaxed) != 0 ||
                centeredOrigin.load(std::memory_order_relaxed) || aliased.load(std::memory_order_relaxed))
                   ? this
                   : nullptr;
    }

private:
    int32_t coveringZoom(int32_t level) const noexcept override;
    TileSubdivision subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept override;
    void translate(uint8_t z, int64_t& x, int64_t& y) const noexcept override;
    int32_t levelId(uint8_t z) const noexcept override;

    /// 以 canonical 层级为下标的数据层级;`identity` 表示该层级自带数据。
    /// 用原子元素而非加锁:该表在渲染路径上被读取、在 style 加载时被改写,
    /// 竞争的最坏结果只是某一帧混用了新旧两套映射。
    static constexpr int8_t identity = -1;
    std::array<std::atomic<int8_t>, maxLevel + 1> table;
    std::atomic<bool> active{false};
    std::atomic<bool> aliased{false};
    /// log2(512/tileSize):256px 源为 +1,512px 源为 0。
    std::atomic<int8_t> sizeShift{0};
    /// 编号原点是否在世界中心(y 轴同时朝北),百度官方网格即如此。
    std::atomic<bool> centeredOrigin{false};
    /// 四叉树层级 → URL 中的层级标签;`identity` 表示直接用四叉树层级。
    std::array<std::atomic<int8_t>, maxLevel + 1> levelIds;

public:
    /// `extra: { "layer-zoom-gate": "tile" }` —— 渲染清单不再按相机 zoom 隐藏
    /// 图层,改由"哪些瓦片有它的桶"决定(桶本就按瓦片自身层级建)。只适用于
    /// **per-level 样式表**(每级图层用 zoom 钳制绑定数据层级,如百度):跨级
    /// 过渡时 retained 旧级瓦片才能由旧级表继续正确着色(消闪白)。常规 zoom
    /// 门控的样式(如高德,minzoom 控制标签密度)绝不能开,否则所有层级的
    /// 标签会同时涌出。默认 "camera"(原版行为)。
    bool layerZoomGateByTile() const noexcept override { return gateByTile.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> gateByTile{false};
};

} // namespace extras
} // namespace mln
