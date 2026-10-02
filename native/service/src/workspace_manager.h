// Internal: owns the current Workspace (atomic swap), serialises open / reload / close, runs the worker thread
// that turns watcher events (state files) into `contracts.changed` / `workspace.updated`.
//
// Thread-safety: every member function may be called from any thread. Handlers use current() / require() to get
// a shared_ptr snapshot and keep using it even if the workspace is replaced meanwhile.
#pragma once

#include "qstate/rpc/event_bus.h"
#include "qstate/service/core_loader.h"
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
    WorkspaceManager(std::shared_ptr<const ServiceConfig> config, std::shared_ptr<CoreLoader> core);
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
    // Builds (loading the core unless `reuseCore` fits the previous workspace) and installs a workspace.
    // `fromWatcher`: no network, and a state file that vanished is a workspace without that file, not an error.
    std::shared_ptr<Workspace> buildAndInstall(const support::WorkspaceRequest& request, const Attempt& attempt,
                                               const std::atomic<bool>* callerCancel,
                                               const std::shared_ptr<Workspace>& previous, bool reuseCore,
                                               bool fromWatcher);
    void emitProgress(const std::string& phase, const std::string& message, std::optional<int> percent);
    void install(const std::shared_ptr<Workspace>& ws, const Attempt& attempt);
    void emitIfCurrent(std::uint64_t workspaceId, const char* name, const nlohmann::json& payload);
    void onWatchEvent(std::uint64_t workspaceId, const support::WatchEvent& event);
    void startWatching(const std::shared_ptr<Workspace>& ws);

    void workerMain();
    void processEvents(std::vector<QueuedEvent>& events);
    void rescan(const std::shared_ptr<Workspace>& current);

    std::shared_ptr<const ServiceConfig> config_;
    std::shared_ptr<CoreLoader> core_;
    std::shared_ptr<decode::DecodeCache> cache_;
    std::shared_ptr<const decode::IdentityCodec> identity_;

    mutable std::recursive_mutex mutex_; // current_, ticket / cancel bookkeeping, event emission
    std::shared_ptr<Workspace> current_;
    std::uint64_t latestTicket_ = 0;
    std::shared_ptr<std::atomic<bool>> activeCancel_;
    std::vector<rpc::EventBus*> buses_;
    std::atomic<std::uint64_t> nextId_{1};

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueuedEvent> queue_;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace qstate::service
