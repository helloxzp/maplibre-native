#pragma once

#include <mln/extras/extras.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace mln {
namespace extras {

/// `extra: { "world-bounds": [west, south, east, north] }`
///
/// 声明数据实际覆盖的经纬度范围,相机据此收紧缩放下限与平移边界,不再能缩到
/// 或拖到露出空白的位置。
///
/// 引擎原本按整块归一化世界方块约束相机(`TransformState::constrain`),这对
/// Web 墨卡托是对的——那里数据正好铺满方块。但 BD09MC 为了让百度网格退化成
/// 标准四叉树,世界边长必须取 2^26 米,数据只占其中宽 59.7%(经度 ±180° =
/// ±20037726 米)、高 38.4%(纬度 ±74° = ±12890595 米),于是缩到最小时上下
/// 各露出约 30% 空白。
///
/// 经纬度按引擎对外 API 的基准面书写(WGS-84),换算成归一化坐标时会自动经过
/// 当前生效的 `offset` 与 `projection` 两个 hook,因此这里填的就是"肉眼看到的
/// 地图边界",不需要预先做偏移或投影。
///
/// 也可以填得比数据范围更小,用来把相机锁在某个区域(例如只让看中国):
/// `"world-bounds": [73.5, 3.8, 135.1, 53.6]`。
class WorldBoundsExtension final : public Extension, private WorldExtent {
public:
    std::vector<std::string> keys() const override { return {"world-bounds"}; }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    WorldExtent* worldExtent() noexcept override {
        return active.load(std::memory_order_relaxed) ? this : nullptr;
    }

private:
    void bounds(double& west, double& south, double& east, double& north) const noexcept override;

    /// 渲染路径上会读,用四个独立原子而非加锁:竞争的最坏结果只是某一帧
    /// 混用了新旧两组边界,下一帧即恢复一致。
    std::atomic<double> west{-180.0};
    std::atomic<double> south{-85.0};
    std::atomic<double> east{180.0};
    std::atomic<double> north{85.0};
    std::atomic<bool> active{false};
};

} // namespace extras
} // namespace mln
