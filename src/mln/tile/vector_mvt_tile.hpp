#pragma once

#include <mln/tile/geometry_tile.hpp>
#include <mln/tile/tile_loader.hpp>
#include <mln/tile/vector_tile.hpp>

#include <memory>

namespace mln {

namespace extras {
class TileGridDualFetch;
} // namespace extras

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
    /// tile-grid(度数网格取瓦)生效时的多行取瓦 sidecar;详见 extras/tile_grid.hpp。
    std::unique_ptr<extras::TileGridDualFetch> gridFetch;
};

} // namespace mln
