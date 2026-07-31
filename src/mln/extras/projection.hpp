#pragma once

#include <mln/extras/extras.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace mln {
namespace extras {

/// `extra: { "projection": "bd09mc" | "plate-carree" }`
///
/// 平面投影扩展,认领 "projection" 键,当前支持两种非墨卡托投影:
///
/// **plate-carree(等距柱面)** —— 高德 JSAPI 矢量瓦片的网格:横轴线性于经度、
/// 纵轴线性于纬度,`x=(lng+180)/360`,`y=(90−lat)/180`,闭式精确可逆。
/// 显示上纵向按 cos(纬度) 压扁(非保形),这是 1:1 使用高德原生网格的固有
/// 代价——想要墨卡托观感必须逐点重投影瓦片内容(代理端 /gcj 做的事)。
///
/// **bd09mc** —— 百度官方瓦片所用的 BD09MC 投影。纬度经一组分段多项式(LLBAND/MCBAND)
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
class ProjectionExtension final : public Extension, private PlanarProjection {
public:
    /// BD09MC 模式下归一化世界对应的米数(2^26)。
    static constexpr double worldMeters = 67108864.0;

    enum class Mode : uint8_t {
        Mercator,    ///< 原生,不接管
        BD09MC,      ///< 百度分段多项式
        PlateCarree, ///< 等距柱面(高德原生网格)
    };

    std::vector<std::string> keys() const override { return {"projection"}; }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    PlanarProjection* planarProjection() noexcept override {
        return mode.load(std::memory_order_relaxed) == Mode::Mercator ? nullptr : this;
    }

private:
    void project(double lng, double lat, double& x, double& y) const noexcept override;
    void unproject(double x, double y, double& lng, double& lat) const noexcept override;

    std::atomic<Mode> mode{Mode::Mercator};
};

} // namespace extras
} // namespace mln
