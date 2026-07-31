#include <mln/tile/vector_mvt_tile.hpp>

#include <mln/extras/hooks.hpp>

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
    : VectorTile(id_, std::move(sourceID_), parameters_, tileset_, observer_) {
    // hook:tile-grid 生效时,一张墨卡托瓦片的纬度带覆盖多个度数行,leg0 走本类
    // 自身的 TileLoader(URL 已在 Resource::tile 里换成北缘度数行),其余行由
    // sidecar 请求;全部到位后在 setData 合成。
    if (!tileset_.tiles.empty()) {
        dataSource = extras::makeTileDataSource(parameters_.fileSource,
                                                tileset_.tiles.at(0),
                                                parameters_.pixelRatio,
                                                static_cast<uint8_t>(tileset_.scheme),
                                                sourceID,
                                                id.canonical.z,
                                                id.canonical.x,
                                                id.canonical.y,
                                                [this] {
                                                    if (!obsolete && dataSource && dataSource->ready()) {
                                                        GeometryTile::setData(dataSource->build());
                                                    }
                                                });
    }
}

VectorMVTTile::~VectorMVTTile() {
    // 先撤 extras 数据源(取消其未完成的请求),再撤 loader。
    dataSource.reset();
    // this needs to be explicitly deleted in the most-derived destructor
    loader.reset();

    // Don't rely on `~TileLoader` to close, it's not safe to call there.
    // We're still calling a virtual method from a destructor, so any overrides will not be called.
    GeometryTile::cancel();
}

void VectorMVTTile::setData(const std::shared_ptr<const std::string>& data_) {
    if (obsolete) {
        return;
    }
    if (dataSource) {
        // extras 接管:字节交给它,所需数据齐备后合成。
        dataSource->setPrimaryData(data_);
        if (dataSource->ready()) {
            GeometryTile::setData(dataSource->build());
        }
        return;
    }
    std::unique_ptr<GeometryTileData> data;
    if (data_) {
        data = std::make_unique<VectorMVTTileData>(data_);
        // hook:稀疏层级复用(子分 + 兄弟共享索引),无扩展生效时原样返回。
        data = extras::adaptTileData(std::move(data), id.canonical.z, id.canonical.x, id.canonical.y, sourceID);
    }
    GeometryTile::setData(std::move(data));
}

} // namespace mln
