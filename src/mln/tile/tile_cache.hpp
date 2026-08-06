#pragma once

#include <mln/actor/scheduler.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/tile/tile.hpp>

#include <list>
#include <memory>
#include <map>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace mln {

/// 瓦片对象缓存。
///
/// 键**无视 wrap**(按 canonical 归一化存取,命中时把请求方的 wrap 盖回瓦片 id):
/// wrap 只描述"画在哪个世界拷贝",内容完全相同。按含 wrap 的键存取的话,低缩放
/// 环绕平移每跨一次日界线,相机经度就回卷一次(TilePyramid::handleWrapJump),
/// 离屏时存进来的 wrap 与转一圈后再请求的 wrap 永远对不齐 —— pop 全部落空,
/// 每一圈都整列重建(负子分合成瓦一次几百毫秒,屏幕一列列闪白;真机踩过)。
class TileCache {
public:
    TileCache(const TaggedScheduler& threadPool_, size_t size_ = 0)
        : threadPool(threadPool_),
          size(size_) {}
    ~TileCache();

    /// Change the maximum size of the cache.
    void setSize(size_t);

    /// Get the maximum size
    size_t getMaxSize() const { return size; }

    /// Add a new tile with the given ID.
    /// If a tile with the same ID is already present, it will be retained and the new one will be discarded.
    void add(const OverscaledTileID& key, std::unique_ptr<Tile>&& tile);

    std::unique_ptr<Tile> pop(const OverscaledTileID& key);
    Tile* get(const OverscaledTileID& key);
    bool has(const OverscaledTileID& key);
    void clear();

    /// Set aside a tile to be destroyed later, without blocking
    void deferredRelease(std::unique_ptr<Tile>&&);

    /// Schedule any accumulated deferred tiles to be destroyed
    void deferPendingReleases();

private:
    std::map<OverscaledTileID, std::unique_ptr<Tile>> tiles;
    std::list<OverscaledTileID> orderedKeys;
    TaggedScheduler threadPool;
    std::vector<std::unique_ptr<Tile>> pendingReleases;
    size_t deferredDeletionsPending{0};
    std::mutex deferredSignalLock;
    std::condition_variable deferredSignal;
    size_t size;
};

} // namespace mln
