#include <mln/extras/tile_scheme.hpp>

#include <mln/util/logging.hpp>
#include <mln/util/string.hpp>

#include <charconv>
#include <string_view>
#include <cmath>

namespace mln {
namespace extras {

namespace {

/// JSON 对象的键一律是字符串,这里要求整串都是合法层级数字。
bool parseLevel(const char* text, std::size_t length, int32_t& out) {
    const char* end = text + length;
    const auto result = std::from_chars(text, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

} // namespace

TileSchemeExtension::TileSchemeExtension() {
    reset();
}

void TileSchemeExtension::reset() {
    for (auto& entry : table) {
        entry.store(identity, std::memory_order_relaxed);
    }
    sizeShift.store(0, std::memory_order_relaxed);
    centeredOrigin.store(false, std::memory_order_relaxed);
    active.store(false, std::memory_order_release);
}

bool TileSchemeExtension::configure(const std::string& key, const JSValue& value) {
    if (key == "tile-origin") {
        if (!value.IsString()) {
            Log::Warning(Event::ParseStyle, "extra.tile-origin 必须是字符串:\"center\" / \"topleft\"");
            return false;
        }
        const std::string_view name{value.GetString(), value.GetStringLength()};
        if (name == "center") {
            centeredOrigin.store(true, std::memory_order_release);
            return true;
        }
        if (name == "topleft" || name == "top-left") {
            centeredOrigin.store(false, std::memory_order_release);
            return true;
        }
        Log::Warning(Event::ParseStyle, "extra.tile-origin 取值无法识别:" + std::string(name));
        return false;
    }

    if (key == "tile-size") {
        if (!value.IsNumber()) {
            Log::Warning(Event::ParseStyle, "extra.tile-size 必须是数字(瓦片像素边长)");
            return false;
        }
        const double size = value.GetDouble();
        // 只接受 2 的整数次幂,且平移量落在合理范围内。
        const double shift = std::log2(512.0 / size);
        const double rounded = std::round(shift);
        if (!(size > 0) || std::fabs(shift - rounded) > 1e-9 || std::fabs(rounded) > 4) {
            Log::Warning(Event::ParseStyle, "extra.tile-size 必须是 512 的 2 次幂倍数(如 128/256/512/1024)");
            return false;
        }
        sizeShift.store(static_cast<int8_t>(rounded), std::memory_order_release);
        return true;
    }

    if (!value.IsObject()) {
        Log::Warning(Event::ParseStyle, "extra.tile-zoom 必须是 {层级: 层级} 形式的对象");
        return false;
    }

    // 先写入临时表,保证某一项格式错误时不会留下半生效的映射。
    std::array<int8_t, maxLevel + 1> pending;
    pending.fill(identity);

    for (const auto& member : value.GetObject()) {
        int32_t from = 0;
        if (!member.name.IsString() || !parseLevel(member.name.GetString(), member.name.GetStringLength(), from)) {
            Log::Warning(Event::ParseStyle, "extra.tile-zoom 的键必须是缩放层级");
            return false;
        }
        if (!member.value.IsNumber()) {
            Log::Warning(Event::ParseStyle, "extra.tile-zoom[" + util::toString(from) + "] 的值必须是缩放层级");
            return false;
        }
        const auto to = static_cast<int32_t>(member.value.GetDouble());
        if (from < 0 || from > maxLevel || to < 0 || to > maxLevel) {
            Log::Warning(Event::ParseStyle,
                         "extra.tile-zoom 条目 " + util::toString(from) + ":" + util::toString(to) + " 超出 0.." +
                             util::toString(maxLevel));
            return false;
        }
        pending[static_cast<std::size_t>(from)] = static_cast<int8_t>(to);
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        table[i].store(pending[i], std::memory_order_relaxed);
    }
    active.store(true, std::memory_order_release);
    return true;
}

int32_t TileSchemeExtension::coveringZoom(int32_t level) const noexcept {
    // 先按瓦片尺寸平移(256px 源比 512px 源多取一级),再查稀疏映射表:
    // 表里写的是数据层级,应当作用在平移之后的层级上。
    level += sizeShift.load(std::memory_order_relaxed);
    if (level < 0 || level > maxLevel) {
        return level;
    }
    const int32_t mapped = table[static_cast<std::size_t>(level)].load(std::memory_order_relaxed);
    // 只有当数据层级比 canonical 层级更细时才抬高取瓦层级:更粗的数据可以
    // 细分复用,更细的数据却无法合并。其余情况保持网格稠密,交给 subdivision()。
    return (mapped != identity && mapped > level) ? mapped : level;
}

TileSubdivision TileSchemeExtension::subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept {
    TileSubdivision sub;
    sub.dataZ = z;
    if (static_cast<int32_t>(z) > maxLevel) {
        return sub;
    }
    const int32_t mapped = table[z].load(std::memory_order_relaxed);
    if (mapped == identity || mapped >= static_cast<int32_t>(z)) {
        return sub; // 本层级自带数据(或已被 coveringZoom 抬高),无需变换
    }
    sub.dataZ = static_cast<uint8_t>(mapped);
    sub.steps = static_cast<uint8_t>(static_cast<int32_t>(z) - mapped);
    // 本瓦片在祖先瓦片内的子格坐标。
    sub.offX = x - ((x >> sub.steps) << sub.steps);
    sub.offY = y - ((y >> sub.steps) << sub.steps);
    return sub;
}

} // namespace extras
} // namespace mln

namespace mln {
namespace extras {

void TileSchemeExtension::translate(uint8_t z, int64_t& x, int64_t& y) const noexcept {
    if (!centeredOrigin.load(std::memory_order_relaxed) || z == 0) {
        return; // z=0 的半块偏移无法用整数表示,数据源应从 z1 起
    }
    const int64_t half = int64_t{1} << (z - 1);
    x -= half;          // 原点从左上角移到世界中心
    y = half - 1 - y;   // 同时把 y 轴由朝南翻成朝北
}

} // namespace extras
} // namespace mln
