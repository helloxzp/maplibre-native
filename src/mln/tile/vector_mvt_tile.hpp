#pragma once

#include <mln/extras/hooks.hpp>
#include <mln/tile/geometry_tile.hpp>
#include <mln/tile/tile_loader.hpp>
#include <mln/tile/vector_tile.hpp>

#include <memory>

namespace mln {

class Tileset;
class TileParameters;

class VectorMVTTile final : public VectorTile {
public:
    VectorMVTTile(const OverscaledTileID&,
                  std::string sourceID,
                  const TileParameters&,
                  const Tileset&,
                  TileObserver* observer = nullptr);

    ~VectorMVTTile() override;

    void setData(const std::shared_ptr<const std::string>& data) override;

    /// 负子分合成瓦不做淡出滞留:离开视口立即进 TileCache(缓存按 canonical
    /// 存取,wrap 无关)。快速环绕平移跨过日界线时,新 wrap 的请求会赶在旧对象
    /// 走完 ~0.6s 的 holdForFade 之前到达 —— 旧对象还占着活表进不了缓存,
    /// 只能整瓦重建(合成一次几百毫秒,整列闪白)。这些层级的标注本就寥寥,
    /// 淡出保护不值这个代价。
    bool holdForFade() const override;

private:
    /// 把数据源当前的合成结果交给 GeometryTile;渐进式数据源每有一片到达就调一次。
    void flushDataSource();

    /// extras 提供的瓦片数据源(需要额外取瓦/变换时非空);见 extras/hooks.hpp。
    std::unique_ptr<extras::TileDataSource> dataSource;
};

} // namespace mln
