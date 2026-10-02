// Internal: owns the current Workspace (atomic swap), serialises open / reload / close, runs the worker thread
// that turns watcher events into `contracts.changed` / `workspace.updated`.
//
// Thread-safety: every member function may be called from any thread. Handlers use current() / require() to get
// a shared_ptr snapshot and keep using it even if the workspace is replaced meanwhile.
#pragma once

#include "qstate/rpc/event_bus.h"
#include "qstate/service/service.h"
#include "workspace.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace qstate::service {

class WorkspaceManager {
public:
    explicit WorkspaceManager(std::shared_ptr<const ServiceConfig> config);
    // Stops the worker thread and the watchers; emits nothing.
    ~WorkspaceManager();
    WorkspaceManager(const WorkspaceManager&) = delete;
    WorkspaceManager& operator=(const WorkspaceManager&) = delete;

    // Events go to every registered bus (a bus must outlive the manager's use of it: the dispatcher / bus owners
    // keep the handlers, and therefore the manager, alive only as long as they live themselves).
    void addBus(rpc::EventBus* bus);
    // After it returns the bus is not used any more (waits for an emission in flight).
    void removeBus(rpc::EventBus* bus);

    std::shared_ptr<Workspace> current() const;
    // Throws rpc::Error(NoWorkspace) when nothing is open.
    std::shared_ptr<Workspace> require() const;

    // Opens a workspace and makes it current. A newer open / reload / close cancels an older one that is still
    // running (the older call then throws rpc::Cancelled). `callerCancel` is the calling RPC's own flag.
    std::shared_ptr<Workspace> open(const support::WorkspaceRequest& request, const std::atomic<bool>* callerCancel);
    std::shared_ptr<Workspace> reload(const std::atomic<bool>* callerCancel);
    void close();

    // Number of Workspace objects built so far (open / reload / rescan / re-extraction); for tests and diagnostics.
    std::size_t workspaceCount() const { return created_.load(); }

private:
    struct Attempt {
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    struct QueuedEvent {
        std::uint64_t workspaceId = 0;
        support::WatchEvent event;
        bool reconcile = false;
    };

    Attempt begin();
    // Builds (loading the core unless `reuse` fits) and installs a workspace. `lenientCore`: a failed
    // re-extraction keeps the previous schema and reports the errors as diagnostics (watcher path).
    std::shared_ptr<Workspace> buildAndInstall(const support::WorkspaceRequest& request, const Attempt& attempt,
                                               const std::atomic<bool>* callerCancel,
                                               const std::shared_ptr<Workspace>& previous, bool reuseCore,
                                               bool lenientCore);
    void install(const std::shared_ptr<Workspace>& ws, const Attempt& attempt);
    void emitIfCurrent(std::uint64_t workspaceId, const char* name, const nlohmann::json& payload);
    void onWatchEvent(std::uint64_t workspaceId, const support::WatchEvent& event);
    void startWatching(const std::shared_ptr<Workspace>& ws);

    void workerMain();
    // `sourceWorkspace`: id of the workspace whose core sources changed (debounce elapsed), 0 = none.
    void processEvents(std::vector<QueuedEvent>& events, std::uint64_t sourceWorkspace);
    void rescan(const std::shared_ptr<Workspace>& current, bool reextract);

    std::shared_ptr<const ServiceConfig> config_;
    std::shared_ptr<decode::DecodeCache> cache_;
    std::shared_ptr<const decode::IdentityCodec> identity_;

    mutable std::recursive_mutex mutex_; // current_, ticket / cancel bookkeeping, event emission
    std::shared_ptr<Workspace> current_;
    std::uint64_t latestTicket_ = 0;
    std::shared_ptr<std::atomic<bool>> activeCancel_;
    std::vector<rpc::EventBus*> buses_;
    std::atomic<std::uint64_t> nextId_{1};
    std::atomic<std::size_t> created_{0};

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueuedEvent> queue_;
    std::optional<std::chrono::steady_clock::time_point> sourceDue_;
    std::uint64_t sourceWorkspace_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace qstate::service
