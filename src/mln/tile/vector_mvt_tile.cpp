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
    : VectorTile(id_, std::move(sourceID_), parameters_, tileset_, observer_),
      mergedTile(extras::tileSubdivision(id_.canonical.z, id_.canonical.x, id_.canonical.y).steps < 0) {
    // hook:两种情况下一张瓦片需要不止一次取瓦,都由 extras 的数据源接管——本类
    // 自身的 TileLoader 只负责其中"主"的那一次(URL 已在 Resource::tile 里换好),
    // 其余由数据源自取,全部到位后在 setData 合成:
    //   - tile-grid:一张墨卡托瓦片的纬度带覆盖多个度数行,主的是覆盖北缘那一行;
    //   - 负子分:本瓦片比数据层级更粗,包含 2^k × 2^k 张数据瓦片,主的是西北角那格。
    // 两者会复合:负子分的每个子格内部再各自做度数行拆分。
    if (!tileset_.tiles.empty()) {
        dataSource = extras::makeTileDataSource(parameters_.fileSource,
                                                tileset_.tiles.at(0),
                                                parameters_.pixelRatio,
                                                static_cast<uint8_t>(tileset_.scheme),
                                                sourceID,
                                                id.canonical.z,
                                                id.canonical.x,
                                                id.canonical.y,
                                                [this] { flushDataSource(); });
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

void VectorMVTTile::flushDataSource() {
    if (obsolete || !dataSource) {
        return;
    }
    // 齐备了就出图;声明了渐进式的(见 TileDataSource::progressive)则每有一片到达
    // 就用当前已有的部分先出图。重复调用不会让解析次数翻倍:GeometryTileWorker
    // 忙时只替换待解析的数据并标记 NeedsParse,连续到达塌成"当前这次 + 最后一次"。
    // build() 全空时返回 nullptr——那不代表"空瓦片",只是还没有任何一片到,所以
    // 不能就这么交上去,否则瓦片会被当成已加载的空瓦片。
    if (!dataSource->ready() && !dataSource->progressive()) {
        return;
    }
    if (auto data = dataSource->build()) {
        GeometryTile::setData(std::move(data));
    } else if (dataSource->ready()) {
        GeometryTile::setData(nullptr); // 确实是空瓦片
    }
}

bool VectorMVTTile::holdForFade() const {
    if (mergedTile) {
        return false; // 见头文件注释
    }
    return GeometryTile::holdForFade();
}

void VectorMVTTile::setData(const std::shared_ptr<const std::string>& data_) {
    if (obsolete) {
        return;
    }
    if (dataSource) {
        // extras 接管:字节交给它,所需数据到位后合成。
        dataSource->setPrimaryData(data_);
        flushDataSource();
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
