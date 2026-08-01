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

private:
    /// 把数据源当前的合成结果交给 GeometryTile;渐进式数据源每有一片到达就调一次。
    void flushDataSource();

    /// extras 提供的瓦片数据源(需要额外取瓦/变换时非空);见 extras/hooks.hpp。
    std::unique_ptr<extras::TileDataSource> dataSource;
};

} // namespace mln
