#include <mln/actor/actor.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/extras/hooks.hpp>
#include <mln/platform/settings.hpp>
#include <mln/storage/file_source_manager.hpp>
#include <mln/storage/file_source_request.hpp>
#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/stopwatch.hpp>
#include <mln/util/thread.hpp>

#include <cassert>
#include <functional>
#include <map>
#include <string>

namespace mln {

class MainResourceLoaderThread {
public:
    MainResourceLoaderThread(std::shared_ptr<FileSource> assetFileSource_,
                             std::shared_ptr<FileSource> databaseFileSource_,
                             std::shared_ptr<FileSource> localFileSource_,
                             std::shared_ptr<FileSource> onlineFileSource_,
                             std::shared_ptr<FileSource> mbtilesFileSource_,
                             std::shared_ptr<FileSource> pmtilesFileSource_)
        : assetFileSource(std::move(assetFileSource_)),
          databaseFileSource(std::move(databaseFileSource_)),
          localFileSource(std::move(localFileSource_)),
          onlineFileSource(std::move(onlineFileSource_)),
          mbtilesFileSource(std::move(mbtilesFileSource_)),
          pmtilesFileSource(std::move(pmtilesFileSource_)) {}

    void request(AsyncRequest* req, const Resource& resource, const ActorRef<FileSourceRequest>& ref) {
        auto callback = [ref](const Response& res) {
            ref.invoke(&FileSourceRequest::setResponse, res);
        };

        auto requestFromNetwork = [=, this](const Resource& res,
                                            std::unique_ptr<AsyncRequest> parent) -> std::unique_ptr<AsyncRequest> {
            if (!onlineFileSource || !onlineFileSource->canRequest(resource)) {
                return parent;
            }

            // hook:同 URL 在途合并。稀疏层级方案下多张瓦片共享同一条数据 URL
            // (子分兄弟 4~256 张、tile-grid 相邻行的度数 leg 重叠),同帧并发的
            // 冷取各自打网 —— 磁盘缓存只能拦"先后",拦不住"同时"(实测压测窗口
            // 271 条请求里 197 条是这种并发重复,最差同一 URL ×18)。合并后同 URL
            // 只发一条网络请求,响应零拷贝分发、落库一次;取消语义保持原版
            // (最后一个订阅者取消即撤单)。仅限**冷取瓦片**:带 prior 的重验证
            // 语义按响应逐请求解释(304 对应各自的 prior),不合并;dataRange
            // (分段请求)同理。无扩展生效时门控恒假,下方原版路径逐位保留。
            if (extras::tileSchemeActive() && res.kind == Resource::Kind::Tile && !res.priorData && !res.priorEtag &&
                !res.priorModified && !res.dataRange) {
                return subscribeSharedFetch(res, req, callback, std::move(parent));
            }

            // Keep parent request alive while chained request is being processed.
            std::shared_ptr<AsyncRequest> parentKeepAlive = std::move(parent);

            MBGL_TIMING_START(watch);
            return onlineFileSource->request(res, [=, ptr = parentKeepAlive, this](const Response& response) {
                if (databaseFileSource) {
                    databaseFileSource->forward(res, response, nullptr);
                }
                if (res.kind == Resource::Kind::Tile) {
                    // onlineResponse.data will be null if data not modified
                    MBGL_TIMING_FINISH(watch,
                                       " Action: " << "Requesting,"
                                                   << " URL: " << res.url.c_str() << " Size: "
                                                   << (response.data != nullptr ? response.data->size() : 0) << "B,"
                                                   << " Time")
                }
                callback(response);
            });
        };

        // Initial tasksSize is used to check whether any of
        // the sources were able to request a resource.
        const std::size_t tasksSize = tasks.size();

        // Waterfall resource request processing and return early once resource was requested.
        if (assetFileSource && assetFileSource->canRequest(resource)) {
            // Asset request
            tasks[req] = assetFileSource->request(resource, callback);
        } else if (mbtilesFileSource && mbtilesFileSource->canRequest(resource)) {
            // Local file request
            tasks[req] = mbtilesFileSource->request(resource, callback);
        } else if (pmtilesFileSource && pmtilesFileSource->canRequest(resource)) {
            // Local file request
            tasks[req] = pmtilesFileSource->request(resource, callback);
        } else if (localFileSource && localFileSource->canRequest(resource)) {
            // Local file request
            tasks[req] = localFileSource->request(resource, callback);
        } else if (databaseFileSource && databaseFileSource->canRequest(resource)) {
            // Try cache only request if needed.
            if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                tasks[req] = databaseFileSource->request(resource, callback);
            } else {
                // Cache request with fallback to network with cache control
                tasks[req] = databaseFileSource->request(resource, [=, this](const Response& response) {
                    Resource res = resource;

                    // Resource is in the cache
                    if (!response.noContent) {
                        if (response.isUsable()) {
                            callback(response);
                            // Set the priority of existing resource to low if it's expired but usable.
                            res.setPriority(Resource::Priority::Low);
                        } else {
                            // Set prior data only if it was not returned to
                            // the requester. Once we get 304 response from
                            // the network, we will forward response to the
                            // requester.
                            res.priorData = response.data;
                        }

                        // Copy response fields for cache control request
                        res.priorModified = response.modified;
                        res.priorExpires = response.expires;
                        res.priorEtag = response.etag;
                    }

                    tasks[req] = requestFromNetwork(res, std::move(tasks[req]));
                });
            }
        } else if (auto networkReq = requestFromNetwork(resource, nullptr)) {
            // Get from the online file source
            tasks[req] = std::move(networkReq);
        }

        // If no new tasks were added, notify client that request cannot be processed.
        if (tasks.size() == tasksSize) {
            Response response;
            response.noContent = true;
            response.error = std::make_unique<Response::Error>(Response::Error::Reason::Other,
                                                               "Unsupported resource request.");
            callback(response);
        }
    }

    void cancel(AsyncRequest* req) {
        assert(req);
        tasks.erase(req);
    }

private:
    // ---- hook:同 URL 在途合并(启用条件与依据见 requestFromNetwork)----
    //
    // 本类是单线程 actor(request/cancel 都经 thread->actor() 进邮箱),无须加锁。
    // 管理面刻意裁到不可再裁的两条规则:
    //   响应:错误 → 分发、保留(底层重试语义照旧);成功 → 落库一次、分发、销毁;
    //   退订:订阅者清零 → 无响应的**保留跑完**(取消不撤单),响应过的销毁。
    //
    // "取消不撤单"的依据(2026-08-14 真机压测,30Hz 缩放振荡):按原版"清零即
    // 撤单"实测残余 37 条重复,微结构全是"取消后重建"的世代循环 —— 每代瓦片
    // 撤单导致响应不落库,下一代再取,代理侧上一代还在处理,世代间形成假并发
    // (同 URL 同秒 ×8)。保留跑完后:字节落库供后代命中,且孤儿在途体仍在表里,
    // 下一代直接挂上来续用同一条请求。代价是真正被放弃的取瓦会多下完(单张
    // ≤500KB、以视口为界),比"回访再整取一次"便宜。
    // "后继瓦片不发请求"绕不开"谁来叫醒它"——订阅表就是这件事的最小形态;
    // 表在首个成功响应后即销毁,不缓存响应字节:分发瞬间之后才来的同 URL 请求
    // 会先查数据库,而落库(forward)与该查询由本线程按序投进同一个数据库 actor
    // 邮箱,FIFO 保证读得到刚写的行 —— 不需要额外状态兜这个竞态。

    /// 一条在途的共享网络取瓦。
    struct SharedFetch {
        /// 订阅者:外层请求令牌 → 投递回调。回调即各请求自己的
        /// `ref.invoke(setResponse)`,经各自 FileSourceRequest 的 actor 转回
        /// 请求方线程 —— 分发路径与各自独立请求时逐位相同,只共享网络往返。
        std::map<AsyncRequest*, std::function<void(const Response&)>> subscribers;
        /// 底层网络请求,归共享体所有,不随任何单个订阅者取消(见类注释的
        /// "取消不撤单"依据)。
        std::unique_ptr<AsyncRequest> underlying;
        /// 是否已投递过响应(含错误)。区分两种"订阅者清零":还没等到响应的
        /// 纯在途 —— 保留跑完落库;已投递过错误还没成功的 —— 销毁,不为孤儿
        /// 烧重试(404 这类不可重试错误否则滞留到进程尾)。
        bool responded = false;
    };

    /// 订阅令牌:存进 tasks[req] 的轻量 AsyncRequest,析构即退订;顺带持有本
    /// 订阅者自己的父链(数据库查询请求),语义同原版的 parentKeepAlive。
    /// 析构只发生在本 actor 线程(cancel() 的 tasks.erase / 线程停机时 tasks
    /// 整体析构;sharedFetches 声明在 tasks 之前,届时仍然活着)。
    class SharedFetchSubscription final : public AsyncRequest {
    public:
        SharedFetchSubscription(MainResourceLoaderThread& thread_,
                                std::string url_,
                                AsyncRequest* req_,
                                std::unique_ptr<AsyncRequest> parent_)
            : thread(thread_),
              url(std::move(url_)),
              req(req_),
              parent(std::move(parent_)) {}
        ~SharedFetchSubscription() override { thread.unsubscribeSharedFetch(url, req); }

    private:
        MainResourceLoaderThread& thread;
        const std::string url;
        AsyncRequest* const req;
        const std::unique_ptr<AsyncRequest> parent;
    };

    std::unique_ptr<AsyncRequest> subscribeSharedFetch(const Resource& res,
                                                       AsyncRequest* req,
                                                       std::function<void(const Response&)> callback,
                                                       std::unique_ptr<AsyncRequest> parent) {
        auto& share = sharedFetches[res.url];
        share.subscribers[req] = std::move(callback);
        if (!share.underlying) {
            // 首订阅者发出真正的网络请求。冷取瓦片恒为 Regular 优先级(Low 只在
            // 过期重验证路径出现,而那条路带 prior、不参与合并),无优先级合并问题。
            share.underlying = onlineFileSource->request(res, [this, url = res.url, res](const Response& response) {
                onSharedFetchResponse(url, res, response);
            });
        }
        return std::make_unique<SharedFetchSubscription>(*this, res.url, req, std::move(parent));
    }

    void onSharedFetchResponse(const std::string& url, const Resource& res, const Response& response) {
        auto it = sharedFetches.find(url);
        if (it == sharedFetches.end()) {
            return; // 全部订阅者已取消、共享体已销毁,响应回调仍在邮箱里排队的尾巴
        }
        it->second.responded = true;
        if (databaseFileSource) {
            // 落库一次(原版是每个请求各落一次同样的字节)。错误响应也照原版
            // 转发:是否缓存(如 404)由数据库层的既有策略决定,不在这层拦。
            databaseFileSource->forward(res, response, nullptr);
        }
        if (response.error) {
            // 错误:已无人订阅就销毁(不为孤儿烧重试);否则分发后保留共享体
            // —— 底层请求的重试(带退避)还在跑,成功后会再次投递,与原版
            // "单请求内错误后重试恢复"语义一致。
            if (it->second.subscribers.empty()) {
                sharedFetches.erase(it);
                return;
            }
            for (auto& [subscriber, deliver] : it->second.subscribers) {
                deliver(response);
            }
            return;
        }
        // 成功:先摘出再分发(回调可能发起新请求,避免迭代期间表被改写),
        // 随共享体销毁,不缓存任何响应字节。
        auto node = sharedFetches.extract(it);
        for (auto& [subscriber, deliver] : node.mapped().subscribers) {
            deliver(response);
        }
    }

    void unsubscribeSharedFetch(const std::string& url, AsyncRequest* req) {
        auto it = sharedFetches.find(url);
        if (it == sharedFetches.end()) {
            return; // 成功响应已收摊,令牌才析构 —— 常规路径
        }
        it->second.subscribers.erase(req);
        // 订阅者清零不撤单:纯在途的保留跑完落库(见类注释依据);已投递过
        // (错误)响应的销毁。
        if (it->second.subscribers.empty() && it->second.responded) {
            sharedFetches.erase(it);
        }
    }

    /// 键为 URL:合并条件已限定 Kind::Tile 且无 prior / dataRange,同 URL 即同
    /// 资源语义。声明在 tasks 之前(析构按声明逆序):tasks 里的订阅令牌析构时
    /// 要回调 unsubscribeSharedFetch 触碰本表。
    std::map<std::string, SharedFetch> sharedFetches;

    const std::shared_ptr<FileSource> assetFileSource;
    const std::shared_ptr<FileSource> databaseFileSource;
    const std::shared_ptr<FileSource> localFileSource;
    const std::shared_ptr<FileSource> onlineFileSource;
    const std::shared_ptr<FileSource> mbtilesFileSource;
    const std::shared_ptr<FileSource> pmtilesFileSource;
    std::map<AsyncRequest*, std::unique_ptr<AsyncRequest>> tasks;
};

class MainResourceLoader::Impl {
public:
    Impl(const ResourceOptions& resourceOptions_,
         const ClientOptions& clientOptions_,
         std::shared_ptr<FileSource> assetFileSource_,
         std::shared_ptr<FileSource> databaseFileSource_,
         std::shared_ptr<FileSource> localFileSource_,
         std::shared_ptr<FileSource> onlineFileSource_,
         std::shared_ptr<FileSource> mbtilesFileSource_,
         std::shared_ptr<FileSource> pmtilesFileSource_)
        : assetFileSource(std::move(assetFileSource_)),
          databaseFileSource(std::move(databaseFileSource_)),
          localFileSource(std::move(localFileSource_)),
          onlineFileSource(std::move(onlineFileSource_)),
          mbtilesFileSource(std::move(mbtilesFileSource_)),
          pmtilesFileSource(std::move(pmtilesFileSource_)),
          supportsCacheOnlyRequests_(bool(databaseFileSource)),
          thread(std::make_unique<util::Thread<MainResourceLoaderThread>>(
              util::makeThreadPrioritySetter(platform::EXPERIMENTAL_THREAD_PRIORITY_WORKER),
              "ResourceLoaderThread",
              assetFileSource,
              databaseFileSource,
              localFileSource,
              onlineFileSource,
              mbtilesFileSource,
              pmtilesFileSource)),
          resourceOptions(resourceOptions_.clone()),
          clientOptions(clientOptions_.clone()) {}

    std::unique_ptr<AsyncRequest> request(const Resource& resource, Callback callback) {
        auto req = std::make_unique<FileSourceRequest>(std::move(callback));

        req->onCancel([actorRef = thread->actor(), req = req.get()]() {
            actorRef.invoke(&MainResourceLoaderThread::cancel, req);
        });
        thread->actor().invoke(&MainResourceLoaderThread::request, req.get(), resource, req->actor());
        return req;
    }

    bool canRequest(const Resource& resource) const {
        return (assetFileSource && assetFileSource->canRequest(resource)) ||
               (localFileSource && localFileSource->canRequest(resource)) ||
               (databaseFileSource && databaseFileSource->canRequest(resource)) ||
               (onlineFileSource && onlineFileSource->canRequest(resource)) ||
               (mbtilesFileSource && mbtilesFileSource->canRequest(resource)) ||
               (pmtilesFileSource && pmtilesFileSource->canRequest(resource));
    }

    bool supportsCacheOnlyRequests() const { return supportsCacheOnlyRequests_; }

    void pause() { thread->pause(); }

    void resume() { thread->resume(); }

    void setResourceOptions(ResourceOptions options) {
        std::scoped_lock lock(resourceOptionsMutex);
        resourceOptions = options;
        assetFileSource->setResourceOptions(options.clone());
        databaseFileSource->setResourceOptions(options.clone());
        localFileSource->setResourceOptions(options.clone());
        onlineFileSource->setResourceOptions(options.clone());
        mbtilesFileSource->setResourceOptions(options.clone());
        pmtilesFileSource->setResourceOptions(options.clone());
    }

    ResourceOptions getResourceOptions() {
        std::scoped_lock lock(resourceOptionsMutex);
        return resourceOptions.clone();
    }

    void setClientOptions(ClientOptions options) {
        std::scoped_lock lock(clientOptionsMutex);
        clientOptions = options;
        assetFileSource->setClientOptions(options.clone());
        databaseFileSource->setClientOptions(options.clone());
        localFileSource->setClientOptions(options.clone());
        onlineFileSource->setClientOptions(options.clone());
        mbtilesFileSource->setClientOptions(options.clone());
        pmtilesFileSource->setClientOptions(options.clone());
    }

    ClientOptions getClientOptions() {
        std::scoped_lock lock(clientOptionsMutex);
        return clientOptions.clone();
    }

private:
    const std::shared_ptr<FileSource> assetFileSource;
    const std::shared_ptr<FileSource> databaseFileSource;
    const std::shared_ptr<FileSource> localFileSource;
    const std::shared_ptr<FileSource> onlineFileSource;
    const std::shared_ptr<FileSource> mbtilesFileSource;
    const std::shared_ptr<FileSource> pmtilesFileSource;
    const bool supportsCacheOnlyRequests_;
    const std::unique_ptr<util::Thread<MainResourceLoaderThread>> thread;
    mutable std::mutex resourceOptionsMutex;
    ResourceOptions resourceOptions;
    mutable std::mutex clientOptionsMutex;
    ClientOptions clientOptions;
};

MainResourceLoader::MainResourceLoader(const ResourceOptions& resourceOptions, const ClientOptions& clientOptions)
    : impl(std::make_unique<Impl>(
          resourceOptions.clone(),
          clientOptions.clone(),
          FileSourceManager::get()->getFileSource(FileSourceType::Asset, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Database, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::FileSystem, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Network, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Mbtiles, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Pmtiles, resourceOptions, clientOptions))) {}

MainResourceLoader::~MainResourceLoader() = default;

bool MainResourceLoader::supportsCacheOnlyRequests() const {
    return impl->supportsCacheOnlyRequests();
}

std::unique_ptr<AsyncRequest> MainResourceLoader::request(const Resource& resource, Callback callback) {
    return impl->request(resource, std::move(callback));
}

bool MainResourceLoader::canRequest(const Resource& resource) const {
    return impl->canRequest(resource);
}

void MainResourceLoader::pause() {
    impl->pause();
}

void MainResourceLoader::resume() {
    impl->resume();
}

void MainResourceLoader::setResourceOptions(ResourceOptions options) {
    impl->setResourceOptions(options.clone());
}

ResourceOptions MainResourceLoader::getResourceOptions() {
    return impl->getResourceOptions();
}

void MainResourceLoader::setClientOptions(ClientOptions options) {
    impl->setClientOptions(options.clone());
}

ClientOptions MainResourceLoader::getClientOptions() {
    return impl->getClientOptions();
}

} // namespace mln
