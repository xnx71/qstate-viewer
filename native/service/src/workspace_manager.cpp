#include "workspace_manager.h"

#include "qstate/decode/support_glue.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/error.h"
#include "qstate/support/dir_scan.h"

#include <algorithm>
#include <set>

namespace qstate::service {

using rpc::Code;
using Clock = std::chrono::steady_clock;

WorkspaceManager::WorkspaceManager(std::shared_ptr<const ServiceConfig> config)
    : config_(std::move(config)),
      cache_(std::make_shared<decode::DecodeCache>(config_->decodeCacheBytes)),
      identity_(std::make_shared<decode::SupportIdentityCodec>()) {
    worker_ = std::thread([this] { workerMain(); });
}

WorkspaceManager::~WorkspaceManager() {
    {
        std::lock_guard lock(queueMutex_);
        stopping_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::shared_ptr<Workspace> last;
    {
        std::lock_guard lock(mutex_);
        buses_.clear(); // nothing is emitted from here on
        if (activeCancel_) activeCancel_->store(true);
        last = std::move(current_);
    }
    if (last) last->stop();
}

void WorkspaceManager::addBus(rpc::EventBus* bus) {
    std::lock_guard lock(mutex_);
    if (std::find(buses_.begin(), buses_.end(), bus) == buses_.end()) buses_.push_back(bus);
}

void WorkspaceManager::removeBus(rpc::EventBus* bus) {
    std::lock_guard lock(mutex_);
    buses_.erase(std::remove(buses_.begin(), buses_.end(), bus), buses_.end());
}

std::shared_ptr<Workspace> WorkspaceManager::current() const {
    std::lock_guard lock(mutex_);
    return current_;
}

std::shared_ptr<Workspace> WorkspaceManager::require() const {
    auto ws = current();
    if (!ws) throw rpc::Error(Code::NoWorkspace, "no workspace is open (call workspace.open first)");
    return ws;
}

WorkspaceManager::Attempt WorkspaceManager::begin() {
    std::lock_guard lock(mutex_);
    if (activeCancel_) activeCancel_->store(true);
    activeCancel_ = std::make_shared<std::atomic<bool>>(false);
    return Attempt{++latestTicket_, activeCancel_};
}

void WorkspaceManager::install(const std::shared_ptr<Workspace>& ws, const Attempt& attempt) {
    std::shared_ptr<Workspace> old;
    {
        std::lock_guard lock(mutex_);
        if (attempt.ticket != latestTicket_ || attempt.cancel->load()) throw rpc::Cancelled();
        old = std::move(current_);
        current_ = ws;
    }
    if (old) old->stop();
}

void WorkspaceManager::close() {
    std::shared_ptr<Workspace> old;
    {
        std::lock_guard lock(mutex_);
        if (activeCancel_) activeCancel_->store(true);
        ++latestTicket_;
        old = std::move(current_);
        current_.reset();
    }
    if (old) old->stop();
    std::lock_guard lock(queueMutex_);
    queue_.clear();
    sourceDue_.reset();
}

void WorkspaceManager::emitIfCurrent(std::uint64_t workspaceId, const char* name, const nlohmann::json& payload) {
    std::lock_guard lock(mutex_);
    if (!current_ || current_->id() != workspaceId) return;
    for (rpc::EventBus* bus : buses_) bus->emit(name, payload);
}

void WorkspaceManager::startWatching(const std::shared_ptr<Workspace>& ws) {
    ws->startWatching([this](std::uint64_t id, const support::WatchEvent& e) { onWatchEvent(id, e); });
    // Changes between the directory scan and the start of the watcher: compare the files once.
    QueuedEvent q;
    q.workspaceId = ws->id();
    q.reconcile = true;
    {
        std::lock_guard lock(queueMutex_);
        queue_.push_back(std::move(q));
    }
    queueCv_.notify_one();
}

void WorkspaceManager::onWatchEvent(std::uint64_t workspaceId, const support::WatchEvent& event) {
    {
        std::lock_guard lock(queueMutex_);
        if (event.tag == "src") {
            sourceDue_ = Clock::now() + config_->sourceDebounce;
            sourceWorkspace_ = workspaceId;
        } else {
            QueuedEvent q;
            q.workspaceId = workspaceId;
            q.event = event;
            queue_.push_back(std::move(q));
        }
    }
    queueCv_.notify_one();
}

std::shared_ptr<Workspace> WorkspaceManager::buildAndInstall(const support::WorkspaceRequest& requestIn, const Attempt& attempt,
                                                            const std::atomic<bool>* callerCancel,
                                                            const std::shared_ptr<Workspace>& previous, bool reuseCore,
                                                            bool lenientCore) {
    auto cancelled = [&]() {
        return (callerCancel != nullptr && callerCancel->load(std::memory_order_relaxed)) ||
               attempt.cancel->load(std::memory_order_relaxed);
    };
    support::WorkspaceRequest request = requestIn;
    if (request.coreDir.empty()) throw rpc::Error(Code::InvalidParams, "params.coreDir must not be empty");
    if (request.stateDir.empty()) throw rpc::Error(Code::InvalidParams, "params.stateDir must not be empty");
    request.coreDir = support::normalizePath(request.coreDir);
    request.stateDir = support::normalizePath(request.stateDir);

    support::StateDirScan scan = support::scanStateDir(request.stateDir);
    if (!scan.readable) {
        throw rpc::Error(Code::IoError, scan.error.empty() ? "cannot read state directory '" + request.stateDir + "'" : scan.error);
    }
    std::optional<std::uint32_t> ext;
    std::optional<int> epochForCore;
    if (request.epoch) {
        if (*request.epoch < 0) throw rpc::Error(Code::InvalidParams, "params.epoch must not be negative");
        ext = static_cast<std::uint32_t>(*request.epoch) % 1000u;
        epochForCore = *request.epoch;
    } else if (auto newest = scan.newestEpoch()) {
        ext = *newest;
        epochForCore = static_cast<int>(*newest);
    }

    std::shared_ptr<const CoreBundle> bundle;
    if (reuseCore && previous) {
        const auto& old = previous->core();
        if (old && old->epochForCore == epochForCore && old->loaded.request.coreDir == request.coreDir &&
            old->loaded.request.coreRef == request.coreRef.value_or("") && old->loaded.request.defines == request.defines) {
            bundle = old;
        }
    }
    if (!bundle) {
        CoreRequest cr;
        cr.coreDir = request.coreDir;
        cr.coreRef = request.coreRef.value_or("");
        cr.epoch = epochForCore;
        cr.defines = request.defines;
        cr.cacheDir = config_->cacheDir;
        LoadedCore loaded;
        if (lenientCore && previous) {
            loaded = loadCoreLenient(cr, cancelled);
            if (loaded.schema->contracts.empty()) {
                // The headers are broken right now (half-saved edit): keep the last good schema, show the errors.
                LoadedCore keep = previous->core()->loaded;
                std::vector<Diagnostic> diags;
                for (const Diagnostic& d : loaded.diagnostics) {
                    if (d.severity == Diagnostic::Severity::Error) diags.push_back(d);
                }
                if (diags.empty()) {
                    Diagnostic d;
                    d.severity = Diagnostic::Severity::Error;
                    d.message = "re-extraction of the core sources failed; showing the previous schema";
                    diags.push_back(d);
                }
                for (const Diagnostic& d : keep.diagnostics) {
                    if (d.severity != Diagnostic::Severity::Error) diags.push_back(d);
                }
                keep.diagnostics = std::move(diags);
                // The files that were read last time stay watched (so fixing the file triggers the next attempt),
                // plus the ones this attempt read.
                for (const std::string& f : loaded.files) {
                    if (std::find(keep.files.begin(), keep.files.end(), f) == keep.files.end()) keep.files.push_back(f);
                }
                loaded = std::move(keep);
            }
        } else {
            loaded = loadCore(cr, cancelled);
        }
        bundle = makeCoreBundle(std::move(loaded));
    }
    if (cancelled()) throw rpc::Cancelled();

    std::map<std::uint32_t, std::uint64_t> generations;
    if (previous) generations = previous->generations();
    WorkspaceDeps deps;
    deps.cache = cache_;
    deps.identity = identity_;
    deps.watcher = config_->watcher;
    auto ws = Workspace::create(nextId_.fetch_add(1), std::move(request), std::move(bundle), std::move(scan), ext, std::move(deps),
                                generations);
    ++created_;
    install(ws, attempt);
    if (config_->watchFiles) startWatching(ws);
    return ws;
}

std::shared_ptr<Workspace> WorkspaceManager::open(const support::WorkspaceRequest& request, const std::atomic<bool>* callerCancel) {
    Attempt attempt = begin();
    return buildAndInstall(request, attempt, callerCancel, current(), /*reuseCore=*/false, /*lenientCore=*/false);
}

std::shared_ptr<Workspace> WorkspaceManager::reload(const std::atomic<bool>* callerCancel) {
    std::shared_ptr<Workspace> prev = require();
    Attempt attempt = begin();
    return buildAndInstall(prev->request(), attempt, callerCancel, prev, /*reuseCore=*/false, /*lenientCore=*/false);
}

void WorkspaceManager::rescan(const std::shared_ptr<Workspace>& cur, bool reextract) {
    Attempt attempt = begin();
    try {
        auto ws = buildAndInstall(cur->request(), attempt, nullptr, cur, /*reuseCore=*/!reextract, /*lenientCore=*/true);
        emitIfCurrent(ws->id(), "workspace.updated", ws->toJson());
    } catch (const rpc::Cancelled&) {
        // superseded by an open / close
    } catch (const std::exception&) {
        // The directory vanished or similar: keep the current workspace; the next event retries.
    }
}

void WorkspaceManager::processEvents(std::vector<QueuedEvent>& events, std::uint64_t sourceWorkspace) {
    std::shared_ptr<Workspace> cur = current();
    if (!cur) return;
    bool needRescan = false;
    std::set<std::uint32_t> modified;
    bool reconcile = false;
    for (const QueuedEvent& q : events) {
        if (q.workspaceId != cur->id()) continue;
        if (q.reconcile) {
            reconcile = true;
            continue;
        }
        const support::ParsedFileName parsed = support::parseStateFileName(q.event.path.substr(q.event.path.find_last_of("/\\") + 1));
        const bool shownContractFile = parsed.kind == support::FileNameKind::ContractState && cur->epochExt() &&
                                       parsed.ext == cur->epochExt();
        if (shownContractFile && q.event.kind == support::WatchEventKind::Modified && !q.event.isDirectory) {
            modified.insert(parsed.contractIndex);
        } else {
            needRescan = true;
        }
    }
    const bool reextract = sourceWorkspace == cur->id();
    if (reextract) {
        rescan(cur, /*reextract=*/true);
        return;
    }
    if (needRescan) {
        rescan(cur, /*reextract=*/false);
        return;
    }
    if (reconcile) {
        for (std::uint32_t i : cur->indicesWithFiles()) modified.insert(i);
    }
    if (modified.empty()) return;
    Workspace::RefreshOutcome outcome = cur->refreshFiles(std::vector<std::uint32_t>(modified.begin(), modified.end()));
    if (outcome.missing) {
        rescan(cur, /*reextract=*/false);
        return;
    }
    if (outcome.changed.empty()) return;
    nlohmann::json contracts = nlohmann::json::array();
    for (std::uint32_t i : outcome.changed) contracts.push_back(cur->contractJson(i));
    emitIfCurrent(cur->id(), "contracts.changed", {{"workspaceId", cur->id()}, {"contracts", std::move(contracts)}});
}

void WorkspaceManager::workerMain() {
    std::unique_lock lock(queueMutex_);
    while (!stopping_) {
        if (queue_.empty() && !sourceDue_) {
            queueCv_.wait(lock);
            continue;
        }
        if (queue_.empty() && sourceDue_ && Clock::now() < *sourceDue_) {
            queueCv_.wait_until(lock, *sourceDue_);
            continue;
        }
        std::vector<QueuedEvent> batch(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
        queue_.clear();
        std::uint64_t sourceWorkspace = 0;
        if (sourceDue_ && Clock::now() >= *sourceDue_) {
            sourceWorkspace = sourceWorkspace_;
            sourceDue_.reset();
        }
        lock.unlock();
        try {
            processEvents(batch, sourceWorkspace);
        } catch (...) {
            // a failing refresh must never kill the worker
        }
        lock.lock();
    }
}

} // namespace qstate::service
