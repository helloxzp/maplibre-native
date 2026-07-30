#include <mln/extras/tile_zoom.hpp>

#include <mln/util/logging.hpp>
#include <mln/util/string.hpp>

#include <charconv>

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

TileZoomExtension::TileZoomExtension() {
    reset();
}

void TileZoomExtension::reset() {
    for (auto& entry : table) {
        entry.store(identity, std::memory_order_relaxed);
    }
    active.store(false, std::memory_order_release);
}

bool TileZoomExtension::configure(const JSValue& value) {
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

int32_t TileZoomExtension::coveringZoom(int32_t level) const noexcept {
    if (level < 0 || level > maxLevel) {
        return level;
    }
    const int32_t mapped = table[static_cast<std::size_t>(level)].load(std::memory_order_relaxed);
    // 只有当数据层级比 canonical 层级更细时才抬高取瓦层级:更粗的数据可以
    // 细分复用,更细的数据却无法合并。其余情况保持网格稠密,交给 subdivision()。
    return (mapped != identity && mapped > level) ? mapped : level;
}

TileSubdivision TileZoomExtension::subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept {
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
