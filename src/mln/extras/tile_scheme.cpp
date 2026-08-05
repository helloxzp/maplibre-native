#include <mln/extras/tile_scheme.hpp>

#include <mln/util/logging.hpp>
#include <mln/util/string.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace mln {
namespace extras {

namespace {

/// tile-levels 的一条:数据所在四叉树层级 / URL 标签 / 起始 canonical 层级。
struct LevelEntry {
    int32_t zoom = 0;
    int32_t id = 0;
    int32_t from = 0;
};

/// 层级字段一律是整数。小数在这里没有意义:zoom/from 落在"瓦片 → 数据"这条
/// 链上,而 subdivision() 只看得到瓦片 id 看不到相机,同一张瓦片不可能因为相机
/// 停在 15.2 还是 15.7 而取不同的数据源。直接拒绝,不做静默截断。
bool readLevel(const JSValue& value, int32_t& out) {
    if (!value.IsNumber()) {
        return false;
    }
    const double raw = value.GetDouble();
    if (std::floor(raw) != raw) {
        return false;
    }
    out = static_cast<int32_t>(raw);
    return true;
}

/// 读一个可选的整数字段;缺省时保持 `out` 不变。返回 false 表示字段存在但不是整数。
bool readOptionalLevel(const JSValue& entry, const char* name, int32_t& out) {
    const auto it = entry.FindMember(name);
    if (it == entry.MemberEnd()) {
        return true;
    }
    return readLevel(it->value, out);
}

} // namespace

TileSchemeExtension::TileSchemeExtension() {
    reset();
}

void TileSchemeExtension::reset() {
    for (auto& entry : table) {
        entry.store(identity, std::memory_order_relaxed);
    }
    for (auto& entry : levelIds) {
        entry.store(identity, std::memory_order_relaxed);
    }
    aliased.store(false, std::memory_order_relaxed);
    gateByTile.store(false, std::memory_order_relaxed);
    mergeEnabled.store(false, std::memory_order_relaxed);
    zoomCarry.store(1.0, std::memory_order_relaxed);
    sizeShift.store(0, std::memory_order_relaxed);
    centeredOrigin.store(false, std::memory_order_relaxed);
    active.store(false, std::memory_order_release);
}

bool TileSchemeExtension::configure(const std::string& key, const JSValue& value) {
    if (key == "layer-zoom-gate") {
        if (!value.IsString()) {
            Log::Warning(Event::ParseStyle, "extra.layer-zoom-gate 必须是字符串:\"tile\" / \"camera\"");
            return false;
        }
        const std::string_view name{value.GetString(), value.GetStringLength()};
        if (name == "tile") {
            gateByTile.store(true, std::memory_order_release);
            return true;
        }
        if (name == "camera") {
            gateByTile.store(false, std::memory_order_release);
            return true;
        }
        Log::Warning(Event::ParseStyle, "extra.layer-zoom-gate 取值无法识别:" + std::string(name));
        return false;
    }

    if (key == "zoom-carry") {
        if (!value.IsNumber()) {
            Log::Warning(Event::ParseStyle, "extra.zoom-carry 必须是数字:整数层级进位点,取值 (0, 1],1 = 原版");
            return false;
        }
        const double carry = value.GetDouble();
        if (!(carry > 0.0) || carry > 1.0) {
            Log::Warning(Event::ParseStyle, "extra.zoom-carry 必须落在 (0, 1]");
            return false;
        }
        zoomCarry.store(carry, std::memory_order_release);
        return true;
    }

    if (key == "tile-merge") {
        if (!value.IsBool()) {
            Log::Warning(Event::ParseStyle, "extra.tile-merge 必须是布尔值(是否对比数据更粗的层级做负子分)");
            return false;
        }
        mergeEnabled.store(value.GetBool(), std::memory_order_release);
        return true;
    }

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

    // 余下的就是 tile-levels(keys() 只声明了这三个键)。
    if (!value.IsArray() || value.Empty()) {
        Log::Warning(Event::ParseStyle, "extra.tile-levels 必须是非空数组,元素形如 {\"zoom\":4,\"id\":5,\"from\":4}");
        return false;
    }

    std::vector<LevelEntry> entries;
    entries.reserve(value.Size());
    for (const auto& item : value.GetArray()) {
        if (!item.IsObject()) {
            Log::Warning(Event::ParseStyle, "extra.tile-levels 的元素必须是对象");
            return false;
        }
        LevelEntry entry;
        const auto zoomIt = item.FindMember("zoom");
        if (zoomIt == item.MemberEnd() || !readLevel(zoomIt->value, entry.zoom)) {
            Log::Warning(Event::ParseStyle, "extra.tile-levels 的每条都必须有整数字段 zoom(数据所在四叉树层级)");
            return false;
        }
        entry.id = entry.zoom;   // 默认 URL 标签就是四叉树层级
        entry.from = entry.zoom; // 默认从本层级起启用
        if (!readOptionalLevel(item, "id", entry.id) || !readOptionalLevel(item, "from", entry.from)) {
            Log::Warning(Event::ParseStyle, "extra.tile-levels 的 id / from 必须是整数(小数在此无意义)");
            return false;
        }
        if (entry.zoom < 0 || entry.zoom > maxLevel || entry.id < 0 || entry.id > maxLevel || entry.from < 0 ||
            entry.from > maxLevel) {
            Log::Warning(Event::ParseStyle,
                         "extra.tile-levels 条目超出 0.." + util::toString(maxLevel) +
                             ":zoom=" + util::toString(entry.zoom) + " id=" + util::toString(entry.id) +
                             " from=" + util::toString(entry.from));
            return false;
        }
        entries.push_back(entry);
    }

    // 允许乱序书写,但 from 不能重复——否则同一 canonical 层级有两个数据来源。
    std::sort(entries.begin(), entries.end(), [](const LevelEntry& a, const LevelEntry& b) { return a.from < b.from; });
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].from == entries[i - 1].from) {
            Log::Warning(Event::ParseStyle, "extra.tile-levels 有重复的 from=" + util::toString(entries[i].from));
            return false;
        }
    }

    // 先写入临时表,保证某一项格式错误时不会留下半生效的映射。
    std::array<int8_t, maxLevel + 1> pendingData;
    std::array<int8_t, maxLevel + 1> pendingIds;
    pendingData.fill(identity);
    pendingIds.fill(identity);

    // canonical 层级 n 用 from <= n 中 from 最大的一条;比最小的 from 还粗的层级
    // 一并落到最粗那条(它们无法由更粗的数据细分而来,coveringZoom 会抬高取瓦层级)。
    std::size_t cursor = 0;
    for (int32_t level = 0; level <= maxLevel; ++level) {
        while (cursor + 1 < entries.size() && entries[cursor + 1].from <= level) {
            ++cursor;
        }
        pendingData[static_cast<std::size_t>(level)] = static_cast<int8_t>(entries[cursor].zoom);
    }
    for (const auto& entry : entries) {
        pendingIds[static_cast<std::size_t>(entry.zoom)] = static_cast<int8_t>(entry.id);
    }

    bool anyAlias = false;
    for (const auto& entry : entries) {
        anyAlias = anyAlias || entry.id != entry.zoom;
    }

    for (std::size_t i = 0; i < pendingData.size(); ++i) {
        table[i].store(pendingData[i], std::memory_order_relaxed);
        levelIds[i].store(pendingIds[i], std::memory_order_relaxed);
    }
    aliased.store(anyAlias, std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
    return true;
}

void TileSchemeExtension::cameraBand(uint8_t z, double& lo, double& hi) const noexcept {
    // 与 coveringZoom 严格互逆:取瓦 z = floor(c + (1-carry)) + sizeShift,
    // 于是 z 服务的相机区间是 [z - sizeShift - 1 + carry, 上界+1)。
    // 缺省(carry=1, sizeShift=0)退化为恒等 [z, z+1)。
    // 稀疏映射表把"数据比本级细"的层级抬高的那条路不在此考虑:开着 tile-merge 时
    // 网格处处稠密,取瓦层级与本公式一致;没开 merge 的抬高路径里多个相机档共用
    // 一张瓦片,本就无法给图层门控一个唯一区间,维持保守的一格宽区间即可。
    const double carry = zoomCarry.load(std::memory_order_relaxed);
    const int32_t shift = sizeShift.load(std::memory_order_relaxed);
    lo = static_cast<double>(z) - static_cast<double>(shift) - 1.0 + carry;
    hi = lo + 1.0;
}

int32_t TileSchemeExtension::coveringZoom(double zoom, int32_t level) const noexcept {
    // 进位点前移:引擎原生"满 1 才进位"(floor),而高德的样式波段在小数 .8 处就
    // 进位。不同步的话,相机小数落在 [carry, 1) 的那 20% 区间里,样式 filter 拿到
    // 的瓦片层级会比高德的显示层级低一级 —— 标注整整晚一档才出现。
    //
    // `zoom - level` 在 floor 路径上就是小数部分;栅格源走的是 round,level 可能
    // 已经比 zoom 大、差值为负,条件自然不成立,所以这里对栅格天然无副作用。
    const double carry = zoomCarry.load(std::memory_order_relaxed);
    if (carry < 1.0 && (zoom - static_cast<double>(level)) >= carry) {
        level += 1;
    }
    // 再按瓦片尺寸平移(256px 源比 512px 源多取一级),然后查稀疏映射表:
    // 表里写的是数据层级,应当作用在平移之后的层级上。
    level += sizeShift.load(std::memory_order_relaxed);
    if (level < 0 || level > maxLevel) {
        return level;
    }
    const int32_t mapped = table[static_cast<std::size_t>(level)].load(std::memory_order_relaxed);
    if (mapped == identity || mapped <= level) {
        // 数据不比本级更细:正向细分复用,网格本就稠密。
        return level;
    }
    // 数据比本级更细。开了 tile-merge 就**不抬高**,交给负子分把 2^k × 2^k 张
    // 合成一张(见 hooks.hpp 的 TileSubdivision),每级各有自己的瓦片,
    // `overscaledZ` 因此恒等于显示层级,样式 filter 里的 ["zoom"] 处处准确。
    //
    // 没开就走原版:把取瓦层级抬到数据层级。代价是这几级塌到同一批瓦片上,而
    // SymbolLayout 对每张瓦片只求值一次图层 filter,那几级只能共用一个 zoom 值
    // ——真机上表现为世界层级画出了官方在该级根本不画的一堆国家名和岛礁名。
    return mergeEnabled.load(std::memory_order_relaxed) ? level : mapped;
}

TileSubdivision TileSchemeExtension::subdivision(uint8_t z, uint32_t x, uint32_t y) const noexcept {
    TileSubdivision sub;
    sub.dataZ = z;
    if (static_cast<int32_t>(z) > maxLevel) {
        return sub;
    }
    const int32_t mapped = table[z].load(std::memory_order_relaxed);
    if (mapped == identity || mapped == static_cast<int32_t>(z)) {
        return sub; // 本层级自带数据,无需变换
    }
    if (mapped > static_cast<int32_t>(z)) {
        if (!mergeEnabled.load(std::memory_order_relaxed)) {
            // 没开 tile-merge:coveringZoom 已把取瓦层级抬到数据层级,不会有
            // 比数据更粗的瓦片走到这里;真走到了也不做变换(原版行为)。
            sub.dataZ = z;
            return sub;
        }
        // 数据更细:本瓦片包含 2^k × 2^k 张数据瓦片。没有"子格坐标"可言——
        // 偏移是每张**源**瓦片各自的,由数据源在合成时逐格给出。
        sub.dataZ = static_cast<uint8_t>(mapped);
        sub.steps = static_cast<int8_t>(static_cast<int32_t>(z) - mapped);
        return sub;
    }
    sub.dataZ = static_cast<uint8_t>(mapped);
    sub.steps = static_cast<int8_t>(static_cast<int32_t>(z) - mapped);
    // 本瓦片在祖先瓦片内的子格坐标。
    sub.offX = x - ((x >> sub.steps) << sub.steps);
    sub.offY = y - ((y >> sub.steps) << sub.steps);
    return sub;
}

} // namespace extras
} // namespace mln

namespace mln {
namespace extras {

int32_t TileSchemeExtension::levelId(uint8_t z) const noexcept {
    if (static_cast<int32_t>(z) > maxLevel) {
        return z;
    }
    const int32_t id = levelIds[z].load(std::memory_order_relaxed);
    return id == identity ? z : id;
}

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
