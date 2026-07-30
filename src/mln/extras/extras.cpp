#include <mln/extras/extras.hpp>

#include <mln/extras/tile_zoom.hpp>

#include <mln/util/logging.hpp>

#include <atomic>
#include <mutex>
#include <vector>

namespace mln {
namespace extras {

namespace {

/// 在渲染路径上被读取,因此用一个零初始化的裸原子:没有扩展生效时,
/// `mapCoveringZoom()` 只有一次 relaxed 读取加一次分支,既无惰性初始化
/// 保护变量,也无锁。
std::atomic<TileScheme*> activeScheme{nullptr};

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
        created->extensions.push_back(std::make_unique<TileZoomExtension>());
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
}

} // namespace

void Registry::add(std::unique_ptr<Extension> extension) {
    if (!extension) {
        return;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    const std::string key = extension->key();
    for (auto& existing : s.extensions) {
        if (existing->key() == key) {
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
        const std::string key = extension->key();
        const JSValue* value = nullptr;
        if (extra) {
            const auto member = extra->FindMember(key.c_str());
            if (member != extra->MemberEnd()) {
                value = &member->value;
            }
        }

        if (value && extension->configure(*value)) {
            Log::Info(Event::ParseStyle, "style extra:已启用 " + key);
        } else {
            // 字段不存在,或格式有误(已在 configure 内告警):回到原版引擎行为,
            // 而不是沿用上一份 style 残留的映射。
            extension->reset();
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
