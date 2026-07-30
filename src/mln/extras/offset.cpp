#include <mln/extras/offset.hpp>

#include <mln/util/logging.hpp>

#include <cmath>
#include <string_view>

namespace mln {
namespace extras {

namespace {

// 克拉索夫斯基椭球,GCJ-02 偏移算法沿用的参数。
constexpr double kAxis = 6378245.0;
constexpr double kEccSq = 0.00669342162296594323;
const double kXPi = 3000.0 / 180.0 * M_PI; // BD09 位移用的角频率

/// GCJ-02 的"境外不偏移"范围,与 gaode-mvt-proxy 一致。
bool outOfChinaGCJ(double lng, double lat) noexcept {
    return !(lng > 73.66 && lng < 135.05 && lat > 3.86 && lat < 53.55);
}

/// BD09 链路上的同名判定,与 baidu-mvt-proxy 一致(范围略宽)。
bool outOfChinaBD(double lng, double lat) noexcept {
    return lng < 72.004 || lng > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

double transformLat(double x, double y) noexcept {
    double ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * std::sqrt(std::fabs(x));
    ret += (20.0 * std::sin(6.0 * x * M_PI) + 20.0 * std::sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    ret += (20.0 * std::sin(y * M_PI) + 40.0 * std::sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    ret += (160.0 * std::sin(y / 12.0 * M_PI) + 320.0 * std::sin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return ret;
}

double transformLng(double x, double y) noexcept {
    double ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * std::sqrt(std::fabs(x));
    ret += (20.0 * std::sin(6.0 * x * M_PI) + 20.0 * std::sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    ret += (20.0 * std::sin(x * M_PI) + 40.0 * std::sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    ret += (150.0 * std::sin(x / 12.0 * M_PI) + 300.0 * std::sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return ret;
}

/// WGS-84 → GCJ-02。`outOfChina` 由调用方传入,以匹配各数据源的判定范围。
void wgs84ToGcj02(double& lng, double& lat, bool (*outOfChina)(double, double)) noexcept {
    if (outOfChina(lng, lat)) return;
    double dLat = transformLat(lng - 105.0, lat - 35.0);
    double dLng = transformLng(lng - 105.0, lat - 35.0);
    const double radLat = lat / 180.0 * M_PI;
    double magic = std::sin(radLat);
    magic = 1 - kEccSq * magic * magic;
    const double sqrtMagic = std::sqrt(magic);
    dLat = (dLat * 180.0) / ((kAxis * (1 - kEccSq)) / (magic * sqrtMagic) * M_PI);
    dLng = (dLng * 180.0) / (kAxis / sqrtMagic * std::cos(radLat) * M_PI);
    lng += dLng;
    lat += dLat;
}

/// GCJ-02 → WGS-84。该偏移没有闭式解,用不动点迭代反解(与两个代理端一致)。
void gcj02ToWgs84(double& lng, double& lat, bool (*outOfChina)(double, double)) noexcept {
    if (outOfChina(lng, lat)) return;
    const double gLng = lng;
    const double gLat = lat;
    double wLng = gLng;
    double wLat = gLat;
    for (int i = 0; i < 5; ++i) {
        double tLng = wLng;
        double tLat = wLat;
        wgs84ToGcj02(tLng, tLat, outOfChina);
        wLng += gLng - tLng;
        wLat += gLat - tLat;
    }
    lng = wLng;
    lat = wLat;
}

void gcj02ToBd09(double& lng, double& lat) noexcept {
    const double x = lng;
    const double y = lat;
    const double z = std::sqrt(x * x + y * y) + 0.00002 * std::sin(y * kXPi);
    const double theta = std::atan2(y, x) + 0.000003 * std::cos(x * kXPi);
    lng = z * std::cos(theta) + 0.0065;
    lat = z * std::sin(theta) + 0.006;
}

void bd09ToGcj02(double& lng, double& lat) noexcept {
    const double x = lng - 0.0065;
    const double y = lat - 0.006;
    const double z = std::sqrt(x * x + y * y) - 0.00002 * std::sin(y * kXPi);
    const double theta = std::atan2(y, x) - 0.000003 * std::cos(x * kXPi);
    lng = z * std::cos(theta);
    lat = z * std::sin(theta);
}

} // namespace

bool OffsetExtension::configure(const std::string&, const JSValue& value) {
    if (!value.IsString()) {
        Log::Warning(Event::ParseStyle, "extra.offset 必须是字符串:\"gcj02\" / \"bd09\" / \"none\"");
        return false;
    }
    const std::string_view name{value.GetString(), value.GetStringLength()};
    Datum datum;
    if (name == "gcj02" || name == "gcj") {
        datum = Datum::GCJ02;
    } else if (name == "bd09" || name == "baidu") {
        datum = Datum::BD09;
    } else if (name == "none" || name == "wgs84") {
        datum = Datum::None;
    } else {
        Log::Warning(Event::ParseStyle, "extra.offset 取值无法识别:" + std::string(name));
        return false;
    }
    active.store(datum, std::memory_order_release);
    return true;
}

void OffsetExtension::reset() {
    active.store(Datum::None, std::memory_order_release);
}

void OffsetExtension::forward(double& lng, double& lat) const noexcept {
    switch (active.load(std::memory_order_relaxed)) {
        case Datum::GCJ02:
            wgs84ToGcj02(lng, lat, outOfChinaGCJ);
            break;
        case Datum::BD09:
            wgs84ToGcj02(lng, lat, outOfChinaBD);
            gcj02ToBd09(lng, lat);
            break;
        case Datum::None:
            break;
    }
}

void OffsetExtension::inverse(double& lng, double& lat) const noexcept {
    switch (active.load(std::memory_order_relaxed)) {
        case Datum::GCJ02:
            gcj02ToWgs84(lng, lat, outOfChinaGCJ);
            break;
        case Datum::BD09: {
            // 闭式的 bd09ToGcj02 与 gcj02ToBd09 并非严格互逆,直接串接会留下
            // 约 10cm 残差;相机"读出再写回"一轮就漂一次,手势反复操作会累积
            // 成可见偏移。因此先用闭式解取一个好初值,再对 forward() 做不动点
            // 迭代,保证正反变换严格闭合。
            const double bLng = lng;
            const double bLat = lat;
            double wLng = bLng;
            double wLat = bLat;
            bd09ToGcj02(wLng, wLat);
            gcj02ToWgs84(wLng, wLat, outOfChinaBD);
            for (int i = 0; i < 3; ++i) {
                double tLng = wLng;
                double tLat = wLat;
                forward(tLng, tLat);
                wLng += bLng - tLng;
                wLat += bLat - tLat;
            }
            lng = wLng;
            lat = wLat;
            break;
        }
        case Datum::None:
            break;
    }
}

} // namespace extras
} // namespace mln
