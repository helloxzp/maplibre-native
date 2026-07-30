#pragma once

#include <mln/extras/hooks.hpp>
#include <mln/util/rapidjson.hpp>

#include <memory>
#include <string>
#include <vector>

/// style 根级 `extra` 字段驱动的可插拔扩展。
///
/// 目标是把定制逻辑集中在本目录内,上游文件只保留极少量 hook:
///   - `style/parser.cpp`        解析出 `extra` 后交给 Registry::configure()
///   - `util/tile_cover.cpp`     coveringZoomLevel() 里调用 mapCoveringZoom()
///   - `storage/resource.cpp`    Resource::tile() 里把 URL 改指数据层级祖先
///   - `tile/vector_mvt_tile.cpp` setData() 里对几何做子格仿射变换
///   - `util/projection.hpp`      经纬度 ↔ 墨卡托两个方向的基准面换算
///   - `map/transform_state.cpp`  相机侧三处内联墨卡托的同名换算
/// style 不含 `extra` 时,全部扩展保持复位,渲染行为与原版 MapLibre 完全一致。
namespace mln {
namespace extras {

/// 对应 style 根级 `extra` 对象中的一个键。
///
/// 扩展只注册一次,之后每次解析 style 都会重新配置:style 带该键时调用
/// `configure()`,不带时调用 `reset()`。
///
/// 要新增一类 hook,只需给 Extension 再加一个默认返回 nullptr 的访问器,
/// 只有实现它的扩展才需要覆写。
class Extension {
public:
    virtual ~Extension() = default;

    /// 本扩展在 `extra` 中占用的键名。一个扩展可以认领多个相关的键
    /// (例如层级方案同时管 "tile-levels" 与 "tile-size"),这样它们共用
    /// 同一份状态,不会出现两个扩展抢同一个 hook 的问题。
    virtual std::vector<std::string> keys() const = 0;

    /// 应用 `extra.<key>`。返回 false 会记录告警并把整个扩展复位,
    /// 使格式错误的 style 退化为原版行为而不是半生效。
    virtual bool configure(const std::string& key, const JSValue& value) = 0;

    /// 丢弃全部配置,回到原版行为。
    virtual void reset() = 0;

    /// 本扩展提供的稀疏层级方案;不提供则返回 nullptr。
    virtual TileScheme* tileScheme() noexcept { return nullptr; }

    /// 本扩展提供的基准面偏移;不提供则返回 nullptr。
    virtual DatumShift* datumShift() noexcept { return nullptr; }

    /// 本扩展提供的平面投影;不提供则返回 nullptr。
    virtual PlanarProjection* planarProjection() noexcept { return nullptr; }
};

/// 进程级扩展集合。
///
/// 依赖注入点:在首次解析 style 之前用 `add()` 传入一个键名相同的 Extension,
/// 即可替换内置实现——无需改动任何 hook 调用点就能换成另一套网格/映射实现。
class Registry {
public:
    /// 取得所有权;同名键的已有扩展会被替换。
    static void add(std::unique_ptr<Extension> extension);

    /// 传入 style 根级的 `extra` 值,style 没有该字段时传 nullptr。
    /// 未被该对象点名的扩展一律复位。
    static void configure(const JSValue* extra);

private:
    Registry() = delete;
};

} // namespace extras
} // namespace mln
