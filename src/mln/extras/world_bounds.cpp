#include <mln/extras/world_bounds.hpp>

#include <mln/util/logging.hpp>
#include <mln/util/string.hpp>

namespace mln {
namespace extras {

bool WorldBoundsExtension::configure(const std::string&, const JSValue& value) {
    if (!value.IsArray() || value.Size() != 4) {
        Log::Warning(Event::ParseStyle, "extra.world-bounds 必须是 [west, south, east, north] 四元数组");
        return false;
    }
    double v[4];
    for (rapidjson::SizeType i = 0; i < 4; ++i) {
        if (!value[i].IsNumber()) {
            Log::Warning(Event::ParseStyle, "extra.world-bounds 的元素必须是数字");
            return false;
        }
        v[i] = value[i].GetDouble();
    }
    // 纬度留出余量而非贴死 ±90:墨卡托类投影在极点发散,贴死会算出无穷大。
    if (!(v[0] < v[2]) || !(v[1] < v[3]) || v[0] < -180.0 || v[2] > 180.0 || v[1] < -89.9 || v[3] > 89.9) {
        Log::Warning(Event::ParseStyle,
                     "extra.world-bounds 需满足 west<east、south<north 且落在 ±180 / ±89.9 内:[" +
                         util::toString(v[0]) + "," + util::toString(v[1]) + "," + util::toString(v[2]) + "," +
                         util::toString(v[3]) + "]");
        return false;
    }
    west.store(v[0], std::memory_order_relaxed);
    south.store(v[1], std::memory_order_relaxed);
    east.store(v[2], std::memory_order_relaxed);
    north.store(v[3], std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
    return true;
}

void WorldBoundsExtension::reset() {
    active.store(false, std::memory_order_release);
}

void WorldBoundsExtension::bounds(double& w, double& s, double& e, double& n) const noexcept {
    w = west.load(std::memory_order_relaxed);
    s = south.load(std::memory_order_relaxed);
    e = east.load(std::memory_order_relaxed);
    n = north.load(std::memory_order_relaxed);
}

} // namespace extras
} // namespace mln
