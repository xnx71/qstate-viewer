#include "workspace.h"

#include "qstate/decode/support_glue.h"
#include "qstate/rpc/error.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace qstate::service {

using rpc::Code;

namespace {

std::string stateFileName(std::uint32_t index, std::uint32_t ext) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "contract%04u.%03u", index, ext);
    return buf;
}

const char* otherKindName(support::OtherFileKind kind) {
    switch (kind) {
    case support::OtherFileKind::Spectrum: return "spectrum";
    case support::OtherFileKind::Universe: return "universe";
    case support::OtherFileKind::ContractExecFees: return "contract-exec-fees";
    case support::OtherFileKind::Archive: return "archive";
    case support::OtherFileKind::Unknown: break;
    }
    return "unknown";
}

bool watchedName(const std::string& name) {
    switch (support::parseStateFileName(name).kind) {
    case support::FileNameKind::ContractState:
    case support::FileNameKind::Spectrum:
    case support::FileNameKind::Universe:
    case support::FileNameKind::ContractExecFees:
    case support::FileNameKind::Archive:
        return true;
    default:
        return false;
    }
}

} // namespace

std::shared_ptr<const CoreBundle> makeCoreBundle(LoadedCore loaded) {
    auto bundle = std::make_shared<CoreBundle>();
    bundle->loaded = std::move(loaded);
    bundle->epochForCore = bundle->loaded.request.epoch;
    if (bundle->loaded.schema) bundle->index = std::make_shared<const decode::SchemaIndex>(bundle->loaded.schema);
    return bundle;
}

const schema::Schema& Workspace::emptySchema() {
    static const schema::Schema empty;
    return empty;
}

std::shared_ptr<Workspace> Workspace::create(std::uint64_t id, support::WorkspaceRequest request,
                                             std::shared_ptr<const CoreBundle> core, support::StateDirScan scan,
                                             std::optional<std::uint32_t> epochExt, WorkspaceDeps deps,
                                             const std::map<std::uint32_t, std::uint64_t>& previousGenerations) {
    std::shared_ptr<Workspace> ws(new Workspace());
    ws->id_ = id;
    ws->request_ = std::move(request);
    ws->core_ = std::move(core);
    ws->scan_ = std::move(scan);
    ws->epochExt_ = epochExt;
    ws->deps_ = std::move(deps);

    std::map<std::uint32_t, Entry> byIndex;
    for (const schema::ContractSchema& c : ws->schema().contracts) {
        Entry e;
        e.index = c.index;
        e.contract = &c;
        byIndex.emplace(c.index, std::move(e));
    }
    if (epochExt) {
        if (const support::EpochFileSet* set = ws->scan_.find(*epochExt)) {
            for (const support::StateFileEntry& f : set->contracts) {
                Entry& e = byIndex[f.index];
                e.index = f.index;
                e.file = f;
                std::string error;
                e.reader = support::FileReader::tryOpen(f.path, &error);
                if (e.reader) {
                    // The reader's view of the size wins (the file may have changed since the scan).
                    e.file->size = e.reader->size();
                    e.file->mtimeMs = e.reader->mtimeNs() / 1000000;
                }
            }
        }
    }
    ws->entries_.reserve(byIndex.size());
    for (auto& [index, e] : byIndex) {
        auto prev = previousGenerations.find(index);
        e.generation = prev == previousGenerations.end() ? 1 : prev->second + 1;
        ws->entries_.push_back(std::move(e));
    }
    return ws;
}

Workspace::~Workspace() {
    stop();
}

Workspace::Entry* Workspace::find(std::uint32_t index) {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), index,
                               [](const Entry& e, std::uint32_t i) { return e.index < i; });
    return (it != entries_.end() && it->index == index) ? &*it : nullptr;
}

const Workspace::Entry* Workspace::find(std::uint32_t index) const {
    return const_cast<Workspace*>(this)->find(index);
}

std::pair<const char*, std::string> Workspace::statusOf(const Entry& e) const {
    const std::string epochText = epochExt_ ? std::to_string(*epochExt_) : std::string("?");
    const LoadedCore& core = core_->loaded;
    const std::string coreText = "core sources " + (core.info.ref.empty() ? std::string("(working tree)") : "'" + core.info.ref + "'") +
                                 (core.info.version.empty() ? "" : " version " + core.info.version) +
                                 (core.info.epoch ? ", epoch " + std::to_string(*core.info.epoch) : "");
    if (e.contract == nullptr) {
        return {"unknown-contract", "state file " + e.file->name + " exists but the " + coreText + " define no contract with index " +
                                        std::to_string(e.index) + " (core older than the state files?)"};
    }
    const schema::ContractSchema& c = *e.contract;
    if (!e.file) {
        if (epochExt_ && c.constructionEpoch > 0 && *epochExt_ < c.constructionEpoch) {
            return {"missing-file", "not yet constructed in epoch " + epochText + " (construction epoch " +
                                        std::to_string(c.constructionEpoch) + ")"};
        }
        return {"missing-file", "no file " + stateFileName(e.index, epochExt_.value_or(0)) + " in " + scan_.dir};
    }
    if (c.stateType == schema::kNoType) {
        return {"schema-error", c.error.empty() ? "the layout of " + c.stateTypeName + " could not be computed" : c.error};
    }
    if (e.file->size == c.expectedSize) return {"ok", std::string()};
    std::string message = "file " + e.file->name + " is " + std::to_string(e.file->size) + " bytes but sizeof(" + c.stateTypeName +
                          ") is " + std::to_string(c.expectedSize) + " bytes in the " + coreText + ".";
    if (core.info.epoch && epochExt_ && static_cast<std::uint32_t>(*core.info.epoch) % 1000u != *epochExt_) {
        message += " The state files are from epoch " + epochText + " but the core sources are for epoch " +
                   std::to_string(*core.info.epoch) + ": the struct layout probably changed in between; choose a core version with EPOCH " +
                   epochText + " (coreRef \"auto\").";
    } else if (e.file->size < c.expectedSize) {
        message += " The file is smaller: it may still be written, or the layout shrank compared with the version that wrote it.";
    } else {
        message += " The file is larger: the layout differs from the version that wrote it, or the file has a stale tail.";
    }
    return {"size-mismatch", message};
}

nlohmann::json Workspace::contractJsonLocked(const Entry& e) const {
    nlohmann::json j = {{"index", e.index}, {"generation", e.generation}};
    j["name"] = e.contract ? e.contract->name : std::string();
    if (e.contract) {
        const schema::ContractSchema& c = *e.contract;
        if (!c.structName.empty()) j["structName"] = c.structName;
        if (!c.stateTypeName.empty()) j["stateTypeName"] = c.stateTypeName;
        if (c.stateType != schema::kNoType) j["stateTypeId"] = c.stateType;
        if (!c.headerFile.empty()) j["headerFile"] = c.headerFile;
        j["constructionEpoch"] = c.constructionEpoch;
        if (c.destructionEpoch != 0) j["destructionEpoch"] = c.destructionEpoch;
        if (c.expectedSize != 0) j["expectedSize"] = c.expectedSize;
    }
    if (e.file) {
        j["file"] = {{"name", e.file->name}, {"path", e.file->path}, {"size", e.file->size}, {"mtimeMs", e.file->mtimeMs}};
    }
    auto [status, message] = statusOf(e);
    j["status"] = status;
    if (!message.empty()) j["statusMessage"] = message;
    return j;
}

nlohmann::json Workspace::contractJson(std::uint32_t index) const {
    std::lock_guard lock(mutex_);
    const Entry* e = find(index);
    if (e == nullptr) throw rpc::Error(Code::NotFound, "no contract with index " + std::to_string(index));
    return contractJsonLocked(*e);
}

std::vector<std::uint32_t> Workspace::contractIndices() const {
    std::vector<std::uint32_t> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.index);
    return out;
}

std::map<std::uint32_t, std::uint64_t> Workspace::generations() const {
    std::lock_guard lock(mutex_);
    std::map<std::uint32_t, std::uint64_t> out;
    for (const Entry& e : entries_) out[e.index] = e.generation;
    return out;
}

std::vector<std::uint32_t> Workspace::indicesWithFiles() const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint32_t> out;
    for (const Entry& e : entries_) {
        if (e.file) out.push_back(e.index);
    }
    return out;
}

std::vector<Diagnostic> Workspace::workspaceDiagnostics() const {
    std::vector<Diagnostic> out;
    auto warn = [&](std::string message) {
        Diagnostic d;
        d.severity = Diagnostic::Severity::Warning;
        d.message = std::move(message);
        out.push_back(std::move(d));
    };
    if (scan_.epochs.empty()) {
        warn("no contractNNNN.EEE state files found in " + scan_.dir);
    } else if (epochExt_) {
        const support::EpochFileSet* set = scan_.find(*epochExt_);
        if (set == nullptr) {
            warn("no state files of epoch " + std::to_string(*epochExt_) + " in " + scan_.dir);
        } else if (!set->contiguous) {
            std::string list;
            for (std::uint32_t i : set->missingIndices) list += (list.empty() ? "" : ", ") + std::to_string(i);
            warn("the state files of epoch " + std::to_string(*epochExt_) + " have gaps; missing contract indices: " + list);
        }
    }
    return out;
}

nlohmann::json Workspace::toJson() const {
    nlohmann::json j;
    j["id"] = id_;
    j["request"] = request_;
    j["core"] = core_->loaded.info;
    nlohmann::json state = {{"dir", scan_.dir}, {"epochsAvailable", scan_.epochNumbers()}, {"otherFiles", nlohmann::json::array()}};
    if (epochExt_) {
        state["epoch"] = *epochExt_;
        for (const support::OtherFileEntry& o : scan_.othersOfEpoch(*epochExt_)) {
            state["otherFiles"].push_back({{"name", o.name}, {"size", o.size}, {"kind", otherKindName(o.kind)}});
        }
    }
    j["state"] = std::move(state);
    nlohmann::json contracts = nlohmann::json::array();
    {
        std::lock_guard lock(mutex_);
        for (const Entry& e : entries_) contracts.push_back(contractJsonLocked(e));
    }
    j["contracts"] = std::move(contracts);
    nlohmann::json diagnostics = nlohmann::json::array();
    for (const Diagnostic& d : core_->loaded.diagnostics) diagnostics.push_back(d);
    for (const Diagnostic& d : workspaceDiagnostics()) diagnostics.push_back(d);
    // Per-contract layout errors that the extraction did not report as diagnostics already.
    for (const schema::ContractSchema& c : schema().contracts) {
        if (c.error.empty()) continue;
        Diagnostic d;
        d.severity = Diagnostic::Severity::Error;
        d.message = c.name + ": " + c.error;
        d.contract = c.index;
        if (!c.headerFile.empty()) d.file = c.headerFile;
        bool dup = false;
        for (const Diagnostic& other : core_->loaded.diagnostics) {
            if (other.message.find(c.error) != std::string::npos) dup = true;
        }
        if (!dup) diagnostics.push_back(d);
    }
    j["diagnostics"] = std::move(diagnostics);
    return j;
}

Workspace::View Workspace::makeView(std::uint32_t index, bool needDecoder) const {
    std::lock_guard lock(mutex_);
    Entry* e = const_cast<Workspace*>(this)->find(index);
    if (e == nullptr) {
        throw rpc::Error(Code::NotFound, "no contract with index " + std::to_string(index));
    }
    if (!e->file) {
        throw rpc::Error(Code::NotFound, "contract " + std::to_string(index) + " has no state file (" + statusOf(*e).second + ")");
    }
    if (!e->reader) {
        // The file could not be opened when the workspace was created (or was created since): try again.
        std::string error;
        e->reader = support::FileReader::tryOpen(e->file->path, &error);
        if (!e->reader) throw rpc::Error(Code::IoError, "cannot open " + e->file->path + ": " + error);
        e->file->size = e->reader->size();
    }
    View v;
    v.reader = e->reader;
    v.generation = e->generation;
    if (!needDecoder) return v;
    if (e->contract == nullptr) {
        throw rpc::Error(Code::NotFound, statusOf(*e).second);
    }
    if (e->contract->stateType == schema::kNoType) {
        throw rpc::Error(Code::SchemaError, statusOf(*e).second);
    }
    if (!e->decoder) {
        decode::DecoderConfig cfg;
        cfg.identity = deps_.identity;
        cfg.cache = deps_.cache;
        cfg.scope = index;
        std::shared_ptr<const schema::Schema> schemaPtr = core_->loaded.schema;
        cfg.contractName = [schemaPtr](std::uint32_t i) -> std::string {
            const schema::ContractSchema* c = schemaPtr->contract(i);
            return c ? c->name : std::string();
        };
        auto source = std::make_shared<decode::FileReaderByteSource>(e->reader);
        e->decoder = std::make_shared<decode::StateDecoder>(core_->index, e->contract->stateType, source, cfg);
    }
    v.decoder = e->decoder;
    return v;
}

Workspace::View Workspace::view(std::uint32_t index) const {
    return makeView(index, true);
}

Workspace::View Workspace::fileView(std::uint32_t index) const {
    return makeView(index, false);
}

Workspace::RefreshOutcome Workspace::refreshFiles(const std::vector<std::uint32_t>& indices) {
    RefreshOutcome out;
    for (std::uint32_t index : indices) {
        std::shared_ptr<support::FileReader> reader;
        std::string path;
        {
            std::lock_guard lock(mutex_);
            const Entry* e = find(index);
            if (e == nullptr || !e->file) continue;
            reader = e->reader;
            path = e->file->path;
        }
        support::RefreshResult result = support::RefreshResult::Changed;
        std::shared_ptr<support::FileReader> opened;
        if (reader) {
            result = reader->refresh();
        } else {
            opened = support::FileReader::tryOpen(path);
            if (!opened) result = support::RefreshResult::Missing;
        }
        if (result == support::RefreshResult::Missing) {
            out.missing = true;
            continue;
        }
        if (result == support::RefreshResult::Unchanged) continue;
        std::lock_guard lock(mutex_);
        Entry* e = find(index);
        if (e == nullptr || !e->file) continue;
        if (opened) {
            e->reader = opened;
            e->decoder.reset();
            reader = opened;
        }
        const support::FileStamp stamp = reader->stamp();
        e->file->size = stamp.size;
        e->file->mtimeMs = stamp.mtimeNs / 1000000;
        ++e->generation;
        out.changed.push_back(index);
    }
    return out;
}

void Workspace::startWatching(EventSink sink) {
    std::lock_guard lock(watchMutex_);
    if (stopped_ || watcher_) return;
    const std::uint64_t id = id_;
    watcher_ = std::make_unique<support::DirWatcher>([id, sink = std::move(sink)](const support::WatchEvent& e) { sink(id, e); },
                                                     deps_.watcher);
    watcher_->addDirectory(scan_.dir, "state", watchedName);
    const LoadedCore& loaded = core_->loaded;
    if (loaded.workingTree) {
        std::vector<std::string> paths;
        paths.reserve(loaded.files.size());
        for (const std::string& rel : loaded.files) paths.push_back(loaded.info.sourceDir + "/" + rel);
        watcher_->addFiles(paths, "src");
    }
    watcher_->start();
}

void Workspace::stop() {
    std::unique_ptr<support::DirWatcher> watcher;
    {
        std::lock_guard lock(watchMutex_);
        stopped_ = true;
        watcher = std::move(watcher_);
    }
    if (watcher) watcher->stop();
}

} // namespace qstate::service
