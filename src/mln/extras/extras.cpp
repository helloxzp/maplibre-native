#include <mln/extras/extras.hpp>

#include <mln/extras/bd09mc.hpp>
#include <mln/extras/offset.hpp>
#include <mln/extras/tile_scheme.hpp>
#include <mln/extras/world_bounds.hpp>

#include <mln/util/logging.hpp>
#include <mln/util/projection.hpp>

#include <algorithm>

#include <atomic>
#include <mutex>
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
        created->extensions.push_back(std::make_unique<BD09MCExtension>());
        created->extensions.push_back(std::make_unique<WorldBoundsExtension>());
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

int32_t mapCoveringZoom(int32_t level) noexcept {
    if (auto* scheme = activeScheme.load(std::memory_order_acquire)) {
        return scheme->coveringZoom(level);
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
