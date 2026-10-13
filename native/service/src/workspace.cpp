#include "workspace.h"

#include "qstate/decode/support_glue.h"
#include "qstate/rpc/error.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace qstate::service {

using rpc::Code;
using schema::LedgerKind;

namespace {

std::string stateFileName(std::uint32_t index, std::uint32_t ext) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "contract%04u.%03u", index, ext);
    return buf;
}

std::string ledgerFileName(LedgerKind kind, std::uint32_t ext) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s.%03u", schema::ledgerKindName(kind), ext);
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
    const support::FileNameKind kind = support::parseStateFileName(name).kind;
    return kind == support::FileNameKind::ContractState || kind == support::FileNameKind::Spectrum ||
           kind == support::FileNameKind::Universe;
}

support::OtherFileKind otherKindOf(LedgerKind kind) {
    return kind == LedgerKind::Spectrum ? support::OtherFileKind::Spectrum : support::OtherFileKind::Universe;
}

std::size_t ledgerIndex(LedgerKind kind) { return kind == LedgerKind::Spectrum ? 0 : 1; }

// Cache scope of a ledger decoder (contract decoders use their index, which is below 1024).
std::uint32_t ledgerScope(LedgerKind kind) { return (1u << 20) + static_cast<std::uint32_t>(ledgerIndex(kind)); }

} // namespace

std::string StateTarget::text() const {
    if (ledger) return schema::ledgerKindName(*ledger);
    return "contract " + std::to_string(contract.value_or(0));
}

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
                                             std::shared_ptr<const CoreBundle> core, support::StateDirScan scan, Scope scope,
                                             std::optional<std::uint32_t> epochExt, std::optional<StateTarget> fileTarget,
                                             WorkspaceDeps deps, const Generations& previousGenerations) {
    std::shared_ptr<Workspace> ws(new Workspace());
    ws->id_ = id;
    ws->request_ = std::move(request);
    ws->core_ = std::move(core);
    ws->scan_ = std::move(scan);
    ws->scope_ = scope;
    ws->epochExt_ = epochExt;
    ws->fileTarget_ = fileTarget;
    ws->deps_ = std::move(deps);

    auto openReader = [](Entry& e) {
        std::string error;
        e.reader = support::FileReader::tryOpen(e.file->path, &error);
        if (e.reader) {
            // The reader's view of the size wins (the file may have changed since the scan).
            e.file->size = e.reader->size();
            e.file->mtimeMs = e.reader->mtimeNs() / 1000000;
        }
    };

    // contracts
    const bool singleLedger = scope == Scope::File && fileTarget && fileTarget->isLedger();
    std::map<std::uint32_t, Entry> byIndex;
    if (!singleLedger) {
        for (const schema::ContractSchema& c : ws->schema().contracts) {
            if (scope == Scope::File && fileTarget && fileTarget->contract != c.index) continue;
            Entry e;
            e.target = StateTarget::ofContract(c.index);
            e.contract = &c;
            byIndex.emplace(c.index, std::move(e));
        }
        if (epochExt) {
            if (const support::EpochFileSet* set = ws->scan_.find(*epochExt)) {
                for (const support::StateFileEntry& f : set->contracts) {
                    Entry& e = byIndex[f.index];
                    e.target = StateTarget::ofContract(f.index);
                    e.file = f;
                    openReader(e);
                }
            }
        }
    }
    ws->entries_.reserve(byIndex.size());
    for (auto& [index, e] : byIndex) {
        auto prev = previousGenerations.contracts.find(index);
        e.generation = prev == previousGenerations.contracts.end() ? 1 : prev->second + 1;
        ws->entries_.push_back(std::move(e));
    }

    // ledgers: both kinds for a directory, the one opened for a single ledger file, none for a single contract file
    for (LedgerKind kind : {LedgerKind::Spectrum, LedgerKind::Universe}) {
        if (scope == Scope::File && (!fileTarget || fileTarget->ledger != kind)) continue;
        Entry e;
        e.target = StateTarget::ofLedger(kind);
        e.ledger = ws->schema().ledger(kind);
        if (epochExt) {
            for (const support::OtherFileEntry& o : ws->scan_.othersOfEpoch(*epochExt)) {
                if (o.kind != otherKindOf(kind) || !o.ext) continue;
                support::StateFileEntry f;
                f.ext = *o.ext;
                f.name = o.name;
                f.path = o.path;
                f.size = o.size;
                f.mtimeMs = o.mtimeMs;
                e.file = f;
                openReader(e);
                break;
            }
        }
        e.generation = previousGenerations.ledgers[ledgerIndex(kind)] + 1;
        ws->ledgers_.push_back(std::move(e));
    }
    return ws;
}

Workspace::~Workspace() {
    stop();
}

Workspace::Entry* Workspace::find(const StateTarget& target) {
    if (target.ledger) {
        for (Entry& e : ledgers_)
            if (e.target.ledger == target.ledger) return &e;
        return nullptr;
    }
    if (!target.contract) return nullptr;
    auto it = std::lower_bound(entries_.begin(), entries_.end(), *target.contract,
                               [](const Entry& e, std::uint32_t i) { return *e.target.contract < i; });
    return (it != entries_.end() && it->target.contract == target.contract) ? &*it : nullptr;
}

const Workspace::Entry* Workspace::find(const StateTarget& target) const {
    return const_cast<Workspace*>(this)->find(target);
}

std::string Workspace::coreText() const {
    const LoadedCore& core = core_->loaded;
    return "core sources '" + core.info.ref + "'" + (core.info.version.empty() ? "" : " version " + core.info.version) +
           (core.info.epoch ? ", epoch " + std::to_string(*core.info.epoch) : "");
}

std::pair<const char*, std::string> Workspace::statusOf(const Entry& e) const {
    const std::string epochText = epochExt_ ? std::to_string(*epochExt_) : std::string("?");
    const LoadedCore& core = core_->loaded;
    auto epochHint = [&](std::string& message, std::uint64_t fileSize, std::uint64_t expected) {
        if (core.info.epoch && epochExt_ && static_cast<std::uint32_t>(*core.info.epoch) % 1000u != *epochExt_) {
            message += " The state files are from epoch " + epochText + " but the core sources are for epoch " +
                       std::to_string(*core.info.epoch) + ": the struct layout probably changed in between; choose a core version with EPOCH " +
                       epochText + " (ref \"auto\").";
        } else if (fileSize < expected) {
            message += " The file is smaller: it may still be written, or the layout shrank compared with the version that wrote it.";
        } else {
            message += " The file is larger: the layout differs from the version that wrote it, or the file has a stale tail.";
        }
    };

    if (e.target.ledger) {
        const LedgerKind kind = *e.target.ledger;
        if (!e.file) return {"missing-file", "no file " + ledgerFileName(kind, epochExt_.value_or(0)) + " in " + scan_.dir};
        if (e.ledger == nullptr || e.ledger->rootType == schema::kNoType) {
            return {"schema-error", e.ledger && !e.ledger->error.empty()
                                        ? e.ledger->error
                                        : "the layout of the " + std::string(schema::ledgerKindName(kind)) + " records is unknown in the " + coreText()};
        }
        if (e.file->size == e.ledger->expectedSize) return {"ok", std::string()};
        std::string message = "file " + e.file->name + " is " + std::to_string(e.file->size) + " bytes but " +
                              std::to_string(e.ledger->capacity) + " * sizeof(" + e.ledger->recordTypeName + ") is " +
                              std::to_string(e.ledger->expectedSize) + " bytes in the " + coreText() + ".";
        epochHint(message, e.file->size, e.ledger->expectedSize);
        return {"size-mismatch", message};
    }

    const std::uint32_t index = e.target.contract.value_or(0);
    if (e.contract == nullptr) {
        return {"unknown-contract", "state file " + e.file->name + " exists but the " + coreText() + " define no contract with index " +
                                        std::to_string(index) + " (core older than the state files?)"};
    }
    const schema::ContractSchema& c = *e.contract;
    if (!e.file) {
        if (epochExt_ && c.constructionEpoch > 0 && *epochExt_ < c.constructionEpoch) {
            return {"missing-file", "not yet constructed in epoch " + epochText + " (construction epoch " +
                                        std::to_string(c.constructionEpoch) + ")"};
        }
        return {"missing-file", "no file " + stateFileName(index, epochExt_.value_or(0)) + " in " + scan_.dir};
    }
    if (c.stateType == schema::kNoType) {
        return {"schema-error", c.error.empty() ? "the layout of " + c.stateTypeName + " could not be computed" : c.error};
    }
    if (e.file->size == c.expectedSize) return {"ok", std::string()};
    std::string message = "file " + e.file->name + " is " + std::to_string(e.file->size) + " bytes but sizeof(" + c.stateTypeName +
                          ") is " + std::to_string(c.expectedSize) + " bytes in the " + coreText() + ".";
    epochHint(message, e.file->size, c.expectedSize);
    return {"size-mismatch", message};
}

nlohmann::json Workspace::contractJsonLocked(const Entry& e) const {
    nlohmann::json j = {{"index", e.target.contract.value_or(0)}, {"generation", e.generation}};
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

nlohmann::json Workspace::ledgerJsonLocked(const Entry& e) const {
    const LedgerKind kind = *e.target.ledger;
    nlohmann::json j = {{"kind", schema::ledgerKindName(kind)}, {"generation", e.generation}};
    j["recordTypeName"] = e.ledger ? e.ledger->recordTypeName : std::string(kind == LedgerKind::Spectrum ? "EntityRecord" : "AssetRecord");
    if (e.ledger) {
        const schema::LedgerSchema& l = *e.ledger;
        if (l.recordType != schema::kNoType) j["recordTypeId"] = l.recordType;
        if (l.recordSize != 0) j["recordSize"] = l.recordSize;
        if (l.capacity != 0) j["capacity"] = l.capacity;
        if (l.expectedSize != 0) j["expectedSize"] = l.expectedSize;
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
    const Entry* e = find(StateTarget::ofContract(index));
    if (e == nullptr) throw rpc::Error(Code::NotFound, "no contract with index " + std::to_string(index));
    return contractJsonLocked(*e);
}

nlohmann::json Workspace::ledgerJson(LedgerKind kind) const {
    std::lock_guard lock(mutex_);
    const Entry* e = find(StateTarget::ofLedger(kind));
    if (e == nullptr) throw rpc::Error(Code::NotFound, std::string("the workspace does not show the ") + schema::ledgerKindName(kind));
    return ledgerJsonLocked(*e);
}

std::vector<std::uint32_t> Workspace::contractIndices() const {
    std::vector<std::uint32_t> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(*e.target.contract);
    return out;
}

Workspace::Generations Workspace::generations() const {
    std::lock_guard lock(mutex_);
    Generations out;
    for (const Entry& e : entries_) out.contracts[*e.target.contract] = e.generation;
    for (const Entry& e : ledgers_) out.ledgers[ledgerIndex(*e.target.ledger)] = e.generation;
    return out;
}

std::vector<StateTarget> Workspace::targetsWithFiles() const {
    std::lock_guard lock(mutex_);
    std::vector<StateTarget> out;
    for (const Entry& e : entries_)
        if (e.file) out.push_back(e.target);
    for (const Entry& e : ledgers_)
        if (e.file) out.push_back(e.target);
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
    if (scope_ == Scope::File) {
        bool present = false;
        if (fileTarget_ && fileTarget_->isLedger()) {
            for (const Entry& e : ledgers_) present = present || e.file.has_value();
        } else {
            present = scan_.find(epochExt_.value_or(0)) != nullptr;
        }
        if (!present) warn("the state file " + request_.statePath + " does not exist");
    } else if (scan_.epochs.empty()) {
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
    nlohmann::json state = {{"dir", scan_.dir},
                            {"scope", scope_ == Scope::File ? "file" : "dir"},
                            {"epochsAvailable", scope_ == Scope::File && epochExt_ ? std::vector<std::uint32_t>{*epochExt_} : scan_.epochNumbers()},
                            {"otherFiles", nlohmann::json::array()}};
    if (epochExt_) {
        state["epoch"] = *epochExt_;
        if (scope_ == Scope::Dir) {
            for (const support::OtherFileEntry& o : scan_.othersOfEpoch(*epochExt_)) {
                state["otherFiles"].push_back({{"name", o.name}, {"size", o.size}, {"kind", otherKindName(o.kind)}});
            }
        }
    }
    j["state"] = std::move(state);
    nlohmann::json contracts = nlohmann::json::array();
    nlohmann::json ledgers = nlohmann::json::array();
    {
        std::lock_guard lock(mutex_);
        for (const Entry& e : entries_) contracts.push_back(contractJsonLocked(e));
        for (const Entry& e : ledgers_) ledgers.push_back(ledgerJsonLocked(e));
    }
    j["contracts"] = std::move(contracts);
    j["ledgers"] = std::move(ledgers);
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

Workspace::View Workspace::makeView(const StateTarget& target, bool needDecoder) const {
    std::lock_guard lock(mutex_);
    Entry* e = const_cast<Workspace*>(this)->find(target);
    if (e == nullptr) {
        if (target.isLedger()) {
            throw rpc::Error(Code::NotFound, "the workspace does not show the " + target.text() +
                                                 (scope_ == Scope::File ? " (a single contract state file is open)" : ""));
        }
        throw rpc::Error(Code::NotFound, "no contract with index " + std::to_string(target.contract.value_or(0)));
    }
    if (!e->file) {
        throw rpc::Error(Code::NotFound, target.text() + " has no state file (" + statusOf(*e).second + ")");
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
    schema::TypeId rootType = schema::kNoType;
    std::uint32_t scopeId = 0;
    std::string rootLabel;
    if (target.isLedger()) {
        if (e->ledger == nullptr || e->ledger->rootType == schema::kNoType) throw rpc::Error(Code::SchemaError, statusOf(*e).second);
        rootType = e->ledger->rootType;
        scopeId = ledgerScope(*target.ledger);
        rootLabel = e->file->name;
    } else {
        if (e->contract == nullptr) throw rpc::Error(Code::NotFound, statusOf(*e).second);
        if (e->contract->stateType == schema::kNoType) throw rpc::Error(Code::SchemaError, statusOf(*e).second);
        rootType = e->contract->stateType;
        scopeId = *target.contract;
    }
    if (!e->decoder) {
        decode::DecoderConfig cfg;
        cfg.identity = deps_.identity;
        cfg.cache = deps_.cache;
        cfg.scope = scopeId;
        cfg.rootLabel = rootLabel;
        std::shared_ptr<const schema::Schema> schemaPtr = core_->loaded.schema;
        cfg.contractName = [schemaPtr](std::uint32_t i) -> std::string {
            const schema::ContractSchema* c = schemaPtr->contract(i);
            return c ? c->name : std::string();
        };
        auto source = std::make_shared<decode::FileReaderByteSource>(e->reader);
        e->decoder = std::make_shared<decode::StateDecoder>(core_->index, rootType, source, cfg);
    }
    v.decoder = e->decoder;
    return v;
}

Workspace::View Workspace::view(const StateTarget& target) const {
    return makeView(target, true);
}

Workspace::View Workspace::fileView(const StateTarget& target) const {
    return makeView(target, false);
}

Workspace::RefreshOutcome Workspace::refreshFiles(const std::vector<StateTarget>& targets) {
    RefreshOutcome out;
    for (const StateTarget& target : targets) {
        std::shared_ptr<support::FileReader> reader;
        std::string path;
        {
            std::lock_guard lock(mutex_);
            const Entry* e = find(target);
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
        Entry* e = find(target);
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
        if (target.ledger) out.changedLedgers.push_back(*target.ledger);
        else out.changed.push_back(*target.contract);
    }
    return out;
}

void Workspace::startWatching(EventSink sink) {
    std::lock_guard lock(watchMutex_);
    if (stopped_ || watcher_) return;
    const std::uint64_t id = id_;
    watcher_ = std::make_unique<support::DirWatcher>([id, sink = std::move(sink)](const support::WatchEvent& e) { sink(id, e); },
                                                     deps_.watcher);
    if (scope_ == Scope::File) {
        watcher_->addFile(request_.statePath, "state");
    } else {
        watcher_->addDirectory(scan_.dir, "state", watchedName);
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
