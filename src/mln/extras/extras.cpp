#include <mln/extras/extras.hpp>

#include <mln/extras/projection.hpp>
#include <mln/extras/offset.hpp>
#include <mln/extras/subdivided_tile_data.hpp>
#include <mln/extras/tile_grid.hpp>
#include <mln/extras/tile_scheme.hpp>
#include <mln/extras/world_bounds.hpp>

#include <mln/util/logging.hpp>
#include <mln/util/projection.hpp>

#include <algorithm>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace mln {
namespace extras {

namespace {

/// 在渲染路径上被读取,因此用一个零初始化的裸原子:没有扩展生效时,
/// `mapCoveringZoom()` 只有一次 relaxed 读取加一次分支,既无惰性初始化
/// 保护变量,也无锁。
std::atomic<TileScheme*> activeScheme{nullptr};
std::atomic<DatumShift*> activeShift{nullptr};
std::atomic<PlanarProjection*> activeProjection{nullptr};
std::atomic<WorldExtent*> activeExtent{nullptr};
std::atomic<TileGridMapper*> activeGridMapper{nullptr};

struct State {
    std::mutex mutex;
    std::vector<std::unique_ptr<Extension>> extensions;
    /// 被 `add()` 替换下来的扩展。这里保留而不销毁,是为了让已经发布给其他
    /// 线程的 hook 指针不可能悬空;注册只在启动时发生几次,不会持续增长。
    std::vector<std::unique_ptr<Extension>> retired;
};

State& state() {
    // 故意泄漏:进程退出、静态对象析构期间仍可能查询本注册表。
    static State* instance = [] {
        auto* created = new State();
        created->extensions.push_back(std::make_unique<TileSchemeExtension>());
        created->extensions.push_back(std::make_unique<OffsetExtension>());
        created->extensions.push_back(std::make_unique<ProjectionExtension>());
        created->extensions.push_back(std::make_unique<WorldBoundsExtension>());
        created->extensions.push_back(std::make_unique<TileGridExtension>());
        return created;
    }();
    return *instance;
}

/// 重新发布各 hook 指针。调用时必须已持有 `state().mutex`。
void publishHooks(State& s) {
    TileScheme* scheme = nullptr;
    for (const auto& extension : s.extensions) {
        if (auto* candidate = extension->tileScheme()) {
            scheme = candidate;
            break;
        }
    }
    activeScheme.store(scheme, std::memory_order_release);

    DatumShift* shift = nullptr;
    for (const auto& extension : s.extensions) {
        if (auto* candidate = extension->datumShift()) {
            shift = candidate;
            break;
        }
    }
    activeShift.store(shift, std::memory_order_release);

    PlanarProjection* projection = nullptr;
    for (const auto& extension : s.extensions) {
        if (auto* candidate = extension->planarProjection()) {
            projection = candidate;
            break;
        }
    }
    activeProjection.store(projection, std::memory_order_release);

    WorldExtent* extent = nullptr;
    for (const auto& extension : s.extensions) {
        if (auto* candidate = extension->worldExtent()) {
            extent = candidate;
            break;
        }
    }
    activeExtent.store(extent, std::memory_order_release);

    TileGridMapper* gridMapper = nullptr;
    for (const auto& extension : s.extensions) {
        if (auto* candidate = extension->tileGridMapper()) {
            gridMapper = candidate;
            break;
        }
    }
    activeGridMapper.store(gridMapper, std::memory_order_release);
}

} // namespace

void Registry::add(std::unique_ptr<Extension> extension) {
    if (!extension) {
        return;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    const auto keys = extension->keys();
    for (auto& existing : s.extensions) {
        if (existing->keys() == keys) {
            s.retired.push_back(std::move(existing));
            existing = std::move(extension);
            publishHooks(s);
            return;
        }
    }
    s.extensions.push_back(std::move(extension));
    publishHooks(s);
}

void Registry::configure(const JSValue* extra) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    if (extra && !extra->IsObject()) {
        Log::Warning(Event::ParseStyle, "style 的 \"extra\" 字段必须是对象,已忽略");
        extra = nullptr;
    }

    for (const auto& extension : s.extensions) {
        // 先收集本扩展在这份 style 里出现的键;一个都没有就复位。
        std::vector<std::pair<std::string, const JSValue*>> present;
        if (extra) {
            for (const auto& key : extension->keys()) {
                const auto member = extra->FindMember(key.c_str());
                if (member != extra->MemberEnd()) {
                    present.emplace_back(key, &member->value);
                }
            }
        }

        // 无论如何先复位,避免沿用上一份 style 的残留配置。
        extension->reset();
        for (const auto& [key, value] : present) {
            if (extension->configure(key, *value)) {
                Log::Info(Event::ParseStyle, "style extra:已启用 " + key);
            } else {
                // 任一项格式有误就整体退回原版,而不是半生效。
                extension->reset();
                break;
            }
        }
    }

    publishHooks(s);
}

int32_t mapCoveringZoom(double zoom, int32_t level) noexcept {
    if (auto* scheme = activeScheme.load(std::memory_order_acquire)) {
        return scheme->coveringZoom(zoom, level);
    }
    return level;
}

int32_t tileLevelId(uint8_t z) noexcept {
    if (auto* scheme = activeScheme.load(std::memory_order_acquire)) {
        return scheme->levelId(z);
    }
    return z;
}

void translateTileCoords(uint8_t z, int64_t& x, int64_t& y) noexcept {
    if (auto* scheme = activeScheme.load(std::memory_order_acquire)) {
        scheme->translate(z, x, y);
    }
}

const DatumShift* datumShift() noexcept {
    return activeShift.load(std::memory_order_acquire);
}

const PlanarProjection* planarProjection() noexcept {
    return activeProjection.load(std::memory_order_acquire);
}

bool worldWrapDisabled() noexcept {
    // 扩展只有在配置了合法范围时才发布 WorldExtent,指针非空即世界有界。
    return activeExtent.load(std::memory_order_acquire) != nullptr;
}

bool tileGridActive() noexcept {
    return activeGridMapper.load(std::memory_order_acquire) != nullptr;
}

uint32_t tileGridLeg0Row(uint8_t z, uint32_t y) noexcept {
    // sidecar 组装其余 leg 的 URL 时走旁路:传入的 y 已是度数行,不能再映射。
    if (tileGridResourceBypassed()) {
        return y;
    }
    if (auto* mapper = activeGridMapper.load(std::memory_order_acquire)) {
        uint32_t r0 = y;
        uint32_t r1 = y;
        mapper->sourceRows(z, y, r0, r1);
        return r0;
    }
    return y;
}

void tileGridSourceRows(uint8_t z, uint32_t y, uint32_t& r0, uint32_t& r1) noexcept {
    r0 = y;
    r1 = y;
    if (auto* mapper = activeGridMapper.load(std::memory_order_acquire)) {
        mapper->sourceRows(z, y, r0, r1);
    }
}

std::unique_ptr<GeometryTileData> adaptTileData(
    std::unique_ptr<GeometryTileData> data, uint8_t z, uint32_t x, uint32_t y, const std::string& sourceID) {
    if (const auto sub = tileSubdivision(z, x, y); sub.steps > 0) {
        data = std::make_unique<SubdividedGeometryTileData>(
            std::move(data), static_cast<uint8_t>(sub.steps), sub.offX, sub.offY, sharedParentIndex(sourceID, sub.dataZ, x >> sub.steps, y >> sub.steps));
    }
    return data;
}

bool renderZoomGateDisabled() noexcept {
    // 只有 style 显式声明 layer-zoom-gate:"tile" 才放开(per-level 样式表专用,
    // 见 hooks.hpp 注释);常规 zoom 门控的样式保持原版行为。
    if (auto* scheme = activeScheme.load(std::memory_order_acquire)) {
        return scheme->layerZoomGateByTile();
    }
    return false;
}

WorldBounds worldBounds() noexcept {
    auto* extent = activeExtent.load(std::memory_order_acquire);
    if (!extent) {
        return {};
    }
    double w = 0.0;
    double s = 0.0;
    double e = 0.0;
    double n = 0.0;
    extent->bounds(w, s, e, n);
    // 走 Projection 而不是自己算:偏移与投影两个 hook 会自动串上,声明的经纬度
    // 因此就是"肉眼看到的地图边界"。scale=1 时 worldSize 即 tileSize,除掉即归一化。
    constexpr double unitScale = 1.0;
    const double worldSize = Projection::worldSize(unitScale);
    const auto northWest = Projection::project(LatLng{n, w}, unitScale);
    const auto southEast = Projection::project(LatLng{s, e}, unitScale);
    WorldBounds out;
    out.x0 = std::min(northWest.x, southEast.x) / worldSize;
    out.x1 = std::max(northWest.x, southEast.x) / worldSize;
    out.y0 = std::min(northWest.y, southEast.y) / worldSize;
    out.y1 = std::max(northWest.y, southEast.y) / worldSize;
    // 反常配置(投影把两角压成同一点)会让后续除法产生 inf,退回整块方块。
    if (!(out.x1 > out.x0) || !(out.y1 > out.y0)) {
        return {};
    }
    return out;
}

namespace {

/// 共享祖先几何索引的进程级注册表。只持 weak_ptr:索引的生命周期完全由持有
/// shared_ptr 的子瓦片决定,兄弟全部释放后自动回收,注册表本身不构成缓存。
std::mutex parentIndexMutex;
std::map<std::tuple<std::string, uint8_t, uint32_t, uint32_t>, std::weak_ptr<ParentGeometryIndex>>
    parentIndexRegistry;

} // namespace

std::shared_ptr<ParentGeometryIndex> sharedParentIndex(const std::string& sourceID,
                                                       uint8_t dataZ,
                                                       uint32_t ancestorX,
                                                       uint32_t ancestorY) {
    std::lock_guard<std::mutex> lock(parentIndexMutex);
    // 顺手清掉失效条目;活跃祖先同时只有个位数,线性扫描可忽略。
    for (auto it = parentIndexRegistry.begin(); it != parentIndexRegistry.end();) {
        it = it->second.expired() ? parentIndexRegistry.erase(it) : std::next(it);
    }
    auto& entry = parentIndexRegistry[std::make_tuple(sourceID, dataZ, ancestorX, ancestorY)];
    if (auto existing = entry.lock()) {
        return existing;
    }
    auto created = std::make_shared<ParentGeometryIndex>();
    entry = created;
    return created;
}

TileSubdivision tileSubdivision(uint8_t z, uint32_t x, uint32_t y) noexcept {
    if (auto* scheme = activeScheme.load(std::memory_order_acquire)) {
        return scheme->subdivision(z, x, y);
    }
    TileSubdivision sub;
    sub.dataZ = z;
    return sub;
}

} // namespace extras
} // namespace mln
