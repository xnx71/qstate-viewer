// Internal: one open workspace = core sources (schema) + state directory or single state file (files) + watcher.
//
// Thread-safety: a Workspace is shared between handler threads (via shared_ptr snapshots) and the manager's
// worker thread. The per-target mutable data (file info, reader, decoder, generation) is guarded by one mutex;
// everything else is immutable after construction.
#pragma once

#include "qstate/decode/cache.h"
#include "qstate/decode/decoder.h"
#include "qstate/decode/identity.h"
#include "qstate/decode/schema_index.h"
#include "qstate/service/core_loader.h"
#include "qstate/support/dir_scan.h"
#include "qstate/support/file_reader.h"
#include "qstate/support/settings.h"
#include "qstate/support/watcher.h"

#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace qstate::service {

// Extracted schema + its decoder index; shared by consecutive workspaces that differ only in the state files.
struct CoreBundle {
    LoadedCore loaded;
    std::shared_ptr<const decode::SchemaIndex> index;
    // Epoch the core was resolved for (the request of "auto").
    std::optional<int> epochForCore;
};

std::shared_ptr<const CoreBundle> makeCoreBundle(LoadedCore loaded);

struct WorkspaceDeps {
    std::shared_ptr<decode::DecodeCache> cache;
    std::shared_ptr<const decode::IdentityCodec> identity;
    support::WatcherOptions watcher;
};

// What the state.* / table.* methods address (contract.ts StateTarget): a contract index or a ledger of the epoch.
struct StateTarget {
    std::optional<std::uint32_t> contract;
    std::optional<schema::LedgerKind> ledger;

    static StateTarget ofContract(std::uint32_t index) { return {index, std::nullopt}; }
    static StateTarget ofLedger(schema::LedgerKind kind) { return {std::nullopt, kind}; }
    bool isLedger() const { return ledger.has_value(); }
    std::string text() const; // "contract 3" | "spectrum" | "universe"
    bool operator==(const StateTarget&) const = default;
    bool operator<(const StateTarget& o) const {
        if (isLedger() != o.isLedger()) return !isLedger();
        return isLedger() ? *ledger < *o.ledger : *contract < *o.contract;
    }
};

class Workspace {
public:
    struct View {
        std::shared_ptr<decode::StateDecoder> decoder;
        std::shared_ptr<support::FileReader> reader;
        std::uint64_t generation = 0;
    };

    struct RefreshOutcome {
        std::vector<std::uint32_t> changed;             // contracts whose file content changed
        std::vector<schema::LedgerKind> changedLedgers; // ledgers whose file content changed
        bool missing = false; // a watched file disappeared: the directory needs a rescan
    };

    // Generation counters carried from one workspace to the next (cache keys never repeat).
    struct Generations {
        std::map<std::uint32_t, std::uint64_t> contracts;
        std::array<std::uint64_t, 2> ledgers{}; // [LedgerKind]
    };

    // What request.statePath names: a directory of state files, or one state file (only that contract / ledger is shown).
    enum class Scope { Dir, File };

    // `scan` is the scan of the state directory (for Scope::File it holds at most that one file); `epochExt` the
    // shown epoch (extension EEE of the files); `fileTarget` the contract / ledger of a Scope::File workspace.
    static std::shared_ptr<Workspace> create(std::uint64_t id, support::WorkspaceRequest request,
                                             std::shared_ptr<const CoreBundle> core, support::StateDirScan scan, Scope scope,
                                             std::optional<std::uint32_t> epochExt, std::optional<StateTarget> fileTarget,
                                             WorkspaceDeps deps, const Generations& previousGenerations);
    ~Workspace();

    std::uint64_t id() const { return id_; }
    const support::WorkspaceRequest& request() const { return request_; }
    const std::shared_ptr<const CoreBundle>& core() const { return core_; }
    const schema::Schema& schema() const { return core_->loaded.schema ? *core_->loaded.schema : emptySchema(); }
    Scope scope() const { return scope_; }
    std::optional<std::uint32_t> epochExt() const { return epochExt_; }
    const support::StateDirScan& scan() const { return scan_; }

    nlohmann::json toJson() const;
    // ContractInfo of one contract; throws rpc::Error(NotFound) for an index that is neither in the schema nor a file.
    nlohmann::json contractJson(std::uint32_t index) const;
    // LedgerInfo of one ledger; throws rpc::Error(NotFound) when the workspace does not show it (single contract file).
    nlohmann::json ledgerJson(schema::LedgerKind kind) const;
    std::vector<std::uint32_t> contractIndices() const;
    Generations generations() const;

    // Decoder + reader of a target for state.* / table.* calls. Throws rpc::Error: not_found (no such contract /
    // ledger, no file, unknown contract), schema_error (layout unavailable), io_error (file cannot be opened).
    View view(const StateTarget& target) const;
    // Reader only (state.bytes, state.digest): works for every target that has a file.
    View fileView(const StateTarget& target) const;

    // Re-stats the files of these targets; bumps the generation of those that changed.
    RefreshOutcome refreshFiles(const std::vector<StateTarget>& targets);
    std::vector<StateTarget> targetsWithFiles() const;

    // Starts the watcher over the state files (the directory, or the one file). The core sources are an immutable
    // export and are not watched. `sink` runs on the watcher thread and must be quick.
    using EventSink = std::function<void(std::uint64_t workspaceId, const support::WatchEvent&)>;
    void startWatching(EventSink sink);
    // Stops the watcher and waits for it. Idempotent; never call from a watcher callback.
    void stop();

private:
    struct Entry {
        StateTarget target;
        const schema::ContractSchema* contract = nullptr; // contracts
        const schema::LedgerSchema* ledger = nullptr;     // ledgers (null when the schema has no ledgers at all)
        std::optional<support::StateFileEntry> file;      // name / path / size / mtime as of the last refresh
        std::shared_ptr<support::FileReader> reader;
        std::shared_ptr<decode::StateDecoder> decoder;
        std::uint64_t generation = 1;
    };

    Workspace() = default;
    static const schema::Schema& emptySchema();
    Entry* find(const StateTarget& target);
    const Entry* find(const StateTarget& target) const;
    nlohmann::json contractJsonLocked(const Entry& e) const;
    nlohmann::json ledgerJsonLocked(const Entry& e) const;
    std::pair<const char*, std::string> statusOf(const Entry& e) const;
    std::vector<Diagnostic> workspaceDiagnostics() const;
    View makeView(const StateTarget& target, bool needDecoder) const;
    std::string coreText() const;

    std::uint64_t id_ = 0;
    support::WorkspaceRequest request_;
    std::shared_ptr<const CoreBundle> core_;
    support::StateDirScan scan_;
    Scope scope_ = Scope::Dir;
    std::optional<std::uint32_t> epochExt_;
    std::optional<StateTarget> fileTarget_;
    WorkspaceDeps deps_;

    mutable std::mutex mutex_; // entries_ / ledgers_ contents (not the vector layout, which is fixed after create())
    mutable std::vector<Entry> entries_;  // contracts, ascending by index
    mutable std::vector<Entry> ledgers_;  // the shown ledgers (spectrum, universe), in LedgerKind order

    std::mutex watchMutex_;
    std::unique_ptr<support::DirWatcher> watcher_;
    bool stopped_ = false;
};

} // namespace qstate::service
