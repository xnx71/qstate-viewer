#include "workspace_manager.h"

#include "qstate/decode/support_glue.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/error.h"
#include "qstate/support/dir_scan.h"

#include <algorithm>
#include <set>

namespace qstate::service {

using rpc::Code;

WorkspaceManager::WorkspaceManager(std::shared_ptr<const ServiceConfig> config, std::shared_ptr<CoreLoader> core)
    : config_(std::move(config)),
      core_(std::move(core)),
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
}

void WorkspaceManager::emitProgress(const std::string& phase, const std::string& message, std::optional<int> percent) {
    nlohmann::json payload = {{"phase", phase}, {"message", message}};
    if (percent) payload["percent"] = *percent;
    std::lock_guard lock(mutex_);
    for (rpc::EventBus* bus : buses_) bus->emit("core.progress", payload);
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
        QueuedEvent q;
        q.workspaceId = workspaceId;
        q.event = event;
        queue_.push_back(std::move(q));
    }
    queueCv_.notify_one();
}

namespace {

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// Keeps only the one state file of a single-file workspace (and no other files of the directory).
void restrictToFile(support::StateDirScan& scan, std::uint32_t index, std::uint32_t ext) {
    std::vector<support::StateFileEntry> keep;
    if (const support::EpochFileSet* set = scan.find(ext)) {
        for (const support::StateFileEntry& f : set->contracts) {
            if (f.index == index) keep.push_back(f);
        }
    }
    scan.epochs.clear();
    scan.others.clear();
    scan.snapshots.clear();
    if (!keep.empty()) {
        support::EpochFileSet set;
        set.epoch = ext;
        set.contracts = std::move(keep);
        scan.epochs.push_back(std::move(set));
    }
}

} // namespace

std::shared_ptr<Workspace> WorkspaceManager::buildAndInstall(const support::WorkspaceRequest& requestIn, const Attempt& attempt,
                                                            const std::atomic<bool>* callerCancel,
                                                            const std::shared_ptr<Workspace>& previous, bool reuseCore,
                                                            bool fromWatcher) {
    auto cancelled = [&]() {
        return (callerCancel != nullptr && callerCancel->load(std::memory_order_relaxed)) ||
               attempt.cancel->load(std::memory_order_relaxed);
    };
    support::WorkspaceRequest request = requestIn;
    request.core.repoUrl = trimmed(request.core.repoUrl);
    request.core.ref = trimmed(request.core.ref);
    if (request.core.repoUrl.empty()) throw rpc::Error(Code::InvalidParams, "params.core.repoUrl must not be empty");
    if (request.core.ref.empty()) throw rpc::Error(Code::InvalidParams, "params.core.ref must not be empty");
    if (request.statePath.empty()) throw rpc::Error(Code::InvalidParams, "params.statePath must not be empty");
    if (request.epoch && *request.epoch < 0) throw rpc::Error(Code::InvalidParams, "params.epoch must not be negative");
    request.statePath = support::normalizePath(request.statePath);

    // What the state path names: a directory with state files, or one state file.
    Workspace::Scope scope = Workspace::Scope::Dir;
    std::string stateDir = request.statePath;
    std::optional<std::uint32_t> fileIndex;
    std::optional<std::uint32_t> fileExt;
    if (!support::isDirectory(request.statePath)) {
        const std::string name = request.statePath.substr(request.statePath.find_last_of("/\\") + 1);
        const support::ParsedFileName parsed = support::parseStateFileName(name);
        const bool exists = support::pathExists(request.statePath);
        if (parsed.kind != support::FileNameKind::ContractState) {
            if (!exists) throw rpc::Error(Code::IoError, "the state path '" + request.statePath + "' does not exist");
            throw rpc::Error(Code::InvalidParams, "'" + request.statePath + "' is neither a directory nor a contract state file (contractNNNN.EEE)");
        }
        if (!exists && !fromWatcher) throw rpc::Error(Code::IoError, "the state file '" + request.statePath + "' does not exist");
        scope = Workspace::Scope::File;
        fileIndex = parsed.contractIndex;
        fileExt = parsed.ext;
        stateDir = support::parentPath(request.statePath).value_or(request.statePath);
    }

    support::StateDirScan scan = support::scanStateDir(stateDir);
    if (!scan.readable && !(fromWatcher && scope == Workspace::Scope::File)) {
        throw rpc::Error(Code::IoError, scan.error.empty() ? "cannot read state directory '" + stateDir + "'" : scan.error);
    }
    std::optional<std::uint32_t> ext;
    std::optional<int> epochForCore;
    if (scope == Workspace::Scope::File) {
        restrictToFile(scan, *fileIndex, *fileExt);
        ext = *fileExt;
        epochForCore = static_cast<int>(*fileExt);
    } else if (request.epoch) {
        ext = static_cast<std::uint32_t>(*request.epoch) % 1000u;
        epochForCore = *request.epoch;
    } else if (auto newest = scan.newestEpoch()) {
        ext = *newest;
        epochForCore = static_cast<int>(*newest);
    }

    CoreRequest coreRequest;
    coreRequest.repoUrl = request.core.repoUrl;
    coreRequest.ref = request.core.ref;
    coreRequest.epoch = epochForCore;
    coreRequest.defines = request.defines;
    std::shared_ptr<const CoreBundle> bundle;
    if (reuseCore && previous && previous->core() && previous->core()->loaded.request == coreRequest) bundle = previous->core();
    if (!bundle) {
        CoreCall call;
        call.cancelled = cancelled;
        call.allowNetwork = !fromWatcher;
        call.progress = [this](const std::string& phase, const std::string& message, std::optional<int> percent) {
            emitProgress(phase, message, percent);
        };
        bundle = makeCoreBundle(core_->load(coreRequest, call));
    }
    if (cancelled()) throw rpc::Cancelled();

    std::map<std::uint32_t, std::uint64_t> generations;
    if (previous) generations = previous->generations();
    WorkspaceDeps deps;
    deps.cache = cache_;
    deps.identity = identity_;
    deps.watcher = config_->watcher;
    auto ws = Workspace::create(nextId_.fetch_add(1), std::move(request), std::move(bundle), std::move(scan), scope, ext, fileIndex,
                                std::move(deps), generations);
    install(ws, attempt);
    startWatching(ws);
    return ws;
}

std::shared_ptr<Workspace> WorkspaceManager::open(const support::WorkspaceRequest& request, const std::atomic<bool>* callerCancel) {
    Attempt attempt = begin();
    return buildAndInstall(request, attempt, callerCancel, current(), /*reuseCore=*/false, /*fromWatcher=*/false);
}

std::shared_ptr<Workspace> WorkspaceManager::reload(const std::atomic<bool>* callerCancel) {
    std::shared_ptr<Workspace> prev = require();
    Attempt attempt = begin();
    return buildAndInstall(prev->request(), attempt, callerCancel, prev, /*reuseCore=*/false, /*fromWatcher=*/false);
}

void WorkspaceManager::rescan(const std::shared_ptr<Workspace>& cur) {
    Attempt attempt = begin();
    try {
        auto ws = buildAndInstall(cur->request(), attempt, nullptr, cur, /*reuseCore=*/true, /*fromWatcher=*/true);
        emitIfCurrent(ws->id(), "workspace.updated", ws->toJson());
    } catch (const rpc::Cancelled&) {
        // superseded by an open / close
    } catch (const std::exception&) {
        // The directory vanished or similar: keep the current workspace; the next event retries.
    }
}

void WorkspaceManager::processEvents(std::vector<QueuedEvent>& events) {
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
    if (needRescan) {
        rescan(cur);
        return;
    }
    if (reconcile) {
        for (std::uint32_t i : cur->indicesWithFiles()) modified.insert(i);
    }
    if (modified.empty()) return;
    Workspace::RefreshOutcome outcome = cur->refreshFiles(std::vector<std::uint32_t>(modified.begin(), modified.end()));
    if (outcome.missing) {
        rescan(cur);
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
        if (queue_.empty()) {
            queueCv_.wait(lock);
            continue;
        }
        std::vector<QueuedEvent> batch(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
        queue_.clear();
        lock.unlock();
        try {
            processEvents(batch);
        } catch (...) {
            // a failing refresh must never kill the worker
        }
        lock.lock();
    }
}

} // namespace qstate::service
