#pragma once

#include <mln/extras/extras.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace mln {
namespace extras {

/// `extra: { "projection": "bd09mc" }`
///
/// 百度官方瓦片所用的 BD09MC 投影。纬度经一组分段多项式(LLBAND/MCBAND)
/// 映射到平面米,与 Web 墨卡托的 log-tan 完全不同,中纬度相差可达数百米。
///
/// **世界尺度取 2^26 米**,这是让百度瓦片与引擎瓦片 1:1 对应的关键:百度
/// zoom b 的瓦片跨度是 2^(26-b) 米,取 2^26 米的世界后每级正好 2^b 块,于是
/// 引擎的标准四叉树与百度网格逐块重合(编号差一个平移与 y 翻转,而平移和
/// 翻转都与四叉树运算可交换,因此瓦片编号、掩码、父子关系全部无需改动)。
/// 代价是世界比地球宽 1.67 倍,多出来的是空白——靠 source bounds 与相机
/// maxBounds 挡住即可。
///
/// 与 `extra.offset: "bd09"` 组合使用:偏移把 WGS-84 挪到 BD09 经纬度,
/// 本投影再把 BD09 经纬度映射到平面。
class BD09MCExtension final : public Extension, private PlanarProjection {
public:
    /// 归一化世界对应的 BD09MC 米数(2^26)。
    static constexpr double worldMeters = 67108864.0;

    std::vector<std::string> keys() const override { return {"projection"}; }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    PlanarProjection* planarProjection() noexcept override {
        return active.load(std::memory_order_relaxed) ? this : nullptr;
    }

private:
    void project(double lng, double lat, double& x, double& y) const noexcept override;
    void unproject(double x, double y, double& lng, double& lat) const noexcept override;

    std::atomic<bool> active{false};
};

} // namespace extras
} // namespace mln
