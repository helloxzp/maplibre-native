#include <mln/extras/projection.hpp>

#include <mln/util/logging.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace mln {
namespace extras {

namespace {

// BD09MC 的分段多项式表,自 Baidu coord-transform.js 移植(经 web fork 校验)。
constexpr std::array<double, 6> kLLBand = {{75, 60, 45, 30, 15, 0}};
constexpr double kLL2MC[6][10] = {
    {-0.0015702102444,
     111320.7020616939,
     1704480524535203.,
     -10338987376042340.,
     26112667856603880.,
     -35149669176653700.,
     26595700718403920.,
     -10725012454188240.,
     1800819912950474.,
     82.5},
    {0.0008277824516172526,
     111320.7020463578,
     647795574.6671607,
     -4082003173.641316,
     10774905663.51142,
     -15171875531.51559,
     12053065338.62167,
     -5124939663.577472,
     913311935.9512032,
     67.5},
    {0.00337398766765,
     111320.7020202162,
     4481351.045890365,
     -23393751.19931662,
     79682215.47186455,
     -115964993.2797253,
     97236711.15602145,
     -43661946.33752821,
     8477230.501135234,
     52.5},
    {0.00220636496208,
     111320.7020209128,
     51751.86112841131,
     3796837.749470245,
     992013.7397791013,
     -1221952.21711287,
     1340652.697009075,
     -620943.6990984312,
     144416.9293806241,
     37.5},
    {-0.0003441963504368392,
     111320.7020576856,
     278.2353980772752,
     2485758.690035394,
     6070.750963243378,
     54821.18345352118,
     9540.606633304236,
     -2710.55326746645,
     1405.483844121726,
     22.5},
    {-0.0003218135878613132,
     111320.7020701615,
     0.00369383431289,
     823725.6402795718,
     0.46104986909093,
     2351.343141331292,
     1.58060784298199,
     8.77738589078284,
     0.37238884252424,
     7.45},
};
constexpr std::array<double, 6> kMCBand = {{12890594.86, 8362377.87, 5591021, 3481989.83, 1678043.12, 0}};
constexpr double kMC2LL[6][10] = {
    {1.410526172116255e-8,
     0.00000898305509648872,
     -1.9939833816331,
     200.9824383106796,
     -187.2403703815547,
     91.6087516669843,
     -23.38765649603339,
     2.57121317296198,
     -0.03801003308653,
     17337981.2},
    {-7.435856389565537e-9,
     0.000008983055097726239,
     -0.78625201886289,
     96.32687599759846,
     -1.85204757529826,
     -59.36935905485877,
     47.40033549296737,
     -16.50741931063887,
     2.28786674699375,
     10260144.86},
    {-3.030883460898826e-8,
     0.00000898305509983578,
     0.30071316287616,
     59.74293618442277,
     7.357984074871,
     -25.38371002664745,
     13.45380521110908,
     -3.29883767235584,
     0.32710905363475,
     6856817.37},
    {-1.981981304930552e-8,
     0.000008983055099779535,
     0.03278182852591,
     40.31678527705744,
     0.65659298677277,
     -4.44255534477492,
     0.85341911805263,
     0.12923347998204,
     -0.04625736007561,
     4482777.06},
    {3.09191371068437e-9,
     0.000008983055096812155,
     0.00006995724062,
     23.10934304144901,
     -0.00023663490511,
     -0.6321817810242,
     -0.00663494467273,
     0.03430082397953,
     -0.00466043876332,
     2555164.4},
    {2.890871144776878e-9,
     0.000008983055095805407,
     -3.068298e-8,
     7.47137025468032,
     -0.00000353937994,
     -0.02145144861037,
     -0.00001234426596,
     0.00010322952773,
     -0.00000323890364,
     826088.5},
};

/// 经度方向是线性的:每度 111320.7020616939 米。
constexpr double kMetersPerDegree = 111320.7020616939;

// 求值顺序与 web fork 的 bd09Poly 完全一致,并关闭 FMA 收缩,
// 使两端逐位相同(否则跨端渲染会出现亚像素接缝)。
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
double bd09Poly(double cb, const double* c) {
    return c[2] + c[3] * cb + c[4] * cb * cb + c[5] * cb * cb * cb + c[6] * cb * cb * cb * cb +
           c[7] * cb * cb * cb * cb * cb + c[8] * cb * cb * cb * cb * cb * cb;
}

/// BD09 纬度 → BD09MC 平面 y(米)。
double mcYfromLat(double lat) {
    lat = std::clamp(lat, -74.0, 74.0); // BD09MC 的有效纬度带
    const double a = std::abs(lat);
    const double* c = kLL2MC[5];
    for (std::size_t i = 0; i < kLLBand.size(); i++) {
        if (a >= kLLBand[i]) {
            c = kLL2MC[i];
            break;
        }
    }
    const double y = bd09Poly(a / c[9], c);
    return lat < 0 ? -y : y;
}

/// BD09MC 平面 y(米) → BD09 纬度。
double latFromMcY(double mcY) {
    const double a = std::abs(mcY);
    const double* c = kMC2LL[5];
    for (std::size_t i = 0; i < kMCBand.size(); i++) {
        if (a >= kMCBand[i]) {
            c = kMC2LL[i];
            break;
        }
    }
    const double lat = bd09Poly(a / c[9], c);
    return mcY < 0 ? -lat : lat;
}

} // namespace

bool ProjectionExtension::configure(const std::string&, const JSValue& value) {
    if (!value.IsString()) {
        Log::Warning(Event::ParseStyle, "extra.projection 必须是字符串:\"bd09mc\" / \"plate-carree\" / \"mercator\"");
        return false;
    }
    const std::string_view name{value.GetString(), value.GetStringLength()};
    if (name == "bd09mc") {
        mode.store(Mode::BD09MC, std::memory_order_release);
        return true;
    }
    if (name == "plate-carree" || name == "platecarree" || name == "equirectangular") {
        mode.store(Mode::PlateCarree, std::memory_order_release);
        return true;
    }
    if (name == "mercator" || name == "webmercator") {
        mode.store(Mode::Mercator, std::memory_order_release);
        return true;
    }
    Log::Warning(Event::ParseStyle, "extra.projection 取值无法识别:" + std::string(name));
    return false;
}

void ProjectionExtension::reset() {
    mode.store(Mode::Mercator, std::memory_order_release);
}

void ProjectionExtension::project(double lng, double lat, double& x, double& y) const noexcept {
    if (mode.load(std::memory_order_relaxed) == Mode::PlateCarree) {
        // 等距柱面:两轴都是线性映射,世界恰好铺满 [0,1]²,闭式精确可逆。
        x = (lng + 180.0) / 360.0;
        y = (90.0 - lat) / 180.0;
        return;
    }
    // 归一化原点在西北角:x 向东为正,y 向南为正,而 BD09MC 的 y 向北为正。
    x = 0.5 + lng * kMetersPerDegree / worldMeters;
    y = 0.5 - mcYfromLat(lat) / worldMeters;
}

void ProjectionExtension::unproject(double x, double y, double& lng, double& lat) const noexcept {
    if (mode.load(std::memory_order_relaxed) == Mode::PlateCarree) {
        lng = x * 360.0 - 180.0;
        // 必须钳制:等距柱面的反投影是线性外推,屏幕角点因浮点误差(或世界比
        // 视口小时的界外像素)会让 y 微越 [0,1],算出 |lat|>90 —— Android SDK 的
        // LatLng 构造器对此直接抛 java.lang.Error。墨卡托从不炸是因为
        // atan(sinh(·)) 渐近钳在 ±85.05°,线性投影没有这层天然保护。
        lat = std::clamp(90.0 - y * 180.0, -90.0, 90.0);
        return;
    }
    lng = (x - 0.5) * worldMeters / kMetersPerDegree;

    // LL2MC 与 MC2LL 是两套独立拟合,并非严格互逆,直接用 MC2LL 会留下约
    // 1.5cm 残差;相机"读出再写回"一轮漂一次,手势反复操作会累积。因此以
    // MC2LL 取初值,再对正向做两步割线迭代,保证 unproject(project(p)) == p。
    const double mcY = (0.5 - y) * worldMeters;
    double guess = latFromMcY(mcY);
    for (int i = 0; i < 2; ++i) {
        constexpr double h = 1e-3;
        const double slope = (mcYfromLat(guess + h) - mcYfromLat(guess - h)) / (2 * h);
        if (slope == 0.0) break;
        guess += (mcY - mcYfromLat(guess)) / slope;
    }
    // 同 plate-carree 分支:界外 y 经带外多项式外推也可能越界,统一钳制。
    lat = std::clamp(guess, -90.0, 90.0);
}

} // namespace extras
} // namespace mln
