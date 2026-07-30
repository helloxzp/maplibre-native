#include <mln/tile/vector_mvt_tile.hpp>

#include <mln/extras/hooks.hpp>
#include <mln/extras/subdivided_tile_data.hpp>

#include <mln/renderer/tile_parameters.hpp>
#include <mln/tile/tile_loader_impl.hpp>
#include <mln/tile/vector_mvt_tile_data.hpp>

#include <utility>

namespace mln {

VectorMVTTile::VectorMVTTile(const OverscaledTileID& id_,
                             std::string sourceID_,
                             const TileParameters& parameters_,
                             const Tileset& tileset_,
                             TileObserver* observer_)
    : VectorTile(id_, std::move(sourceID_), parameters_, tileset_, observer_) {}

VectorMVTTile::~VectorMVTTile() {
    // this needs to be explicitly deleted in the most-derived destructor
    loader.reset();

    // Don't rely on `~TileLoader` to close, it's not safe to call there.
    // We're still calling a virtual method from a destructor, so any overrides will not be called.
    GeometryTile::cancel();
}

void VectorMVTTile::setData(const std::shared_ptr<const std::string>& data_) {
    if (!obsolete) {
        std::unique_ptr<GeometryTileData> data;
        if (data_) {
            data = std::make_unique<VectorMVTTileData>(data_);
            // hook:本瓦片取的是祖先数据瓦片时,把几何缩放平移到本瓦片的子格
            // 范围内(层级复用,见 src/mln/extras)。
            if (const auto sub = extras::tileSubdivision(id.canonical.z, id.canonical.x, id.canonical.y);
                sub.steps > 0) {
                data = std::make_unique<extras::SubdividedGeometryTileData>(
                    std::move(data), sub.steps, sub.offX, sub.offY);
            }
        }
        GeometryTile::setData(std::move(data));
    }
}

} // namespace mln
