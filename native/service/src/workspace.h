// Internal: one open workspace = core sources (schema) + state directory (files) + watchers.
//
// Thread-safety: a Workspace is shared between handler threads (via shared_ptr snapshots) and the manager's
// worker thread. The per-contract mutable data (file info, reader, decoder, generation) is guarded by one mutex;
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

class Workspace {
public:
    struct View {
        std::shared_ptr<decode::StateDecoder> decoder;
        std::shared_ptr<support::FileReader> reader;
        std::uint64_t generation = 0;
    };

    struct RefreshOutcome {
        std::vector<std::uint32_t> changed;
        bool missing = false; // a watched file disappeared: the directory needs a rescan
    };

    // `scan` is the scan of request.stateDir; `epochExt` the shown epoch (extension EEE of the files).
    static std::shared_ptr<Workspace> create(std::uint64_t id, support::WorkspaceRequest request,
                                             std::shared_ptr<const CoreBundle> core, support::StateDirScan scan,
                                             std::optional<std::uint32_t> epochExt, WorkspaceDeps deps,
                                             const std::map<std::uint32_t, std::uint64_t>& previousGenerations);
    ~Workspace();

    std::uint64_t id() const { return id_; }
    const support::WorkspaceRequest& request() const { return request_; }
    const std::shared_ptr<const CoreBundle>& core() const { return core_; }
    const schema::Schema& schema() const { return core_->loaded.schema ? *core_->loaded.schema : emptySchema(); }
    std::optional<std::uint32_t> epochExt() const { return epochExt_; }
    const support::StateDirScan& scan() const { return scan_; }

    nlohmann::json toJson() const;
    // ContractInfo of one contract; throws rpc::Error(NotFound) for an index that is neither in the schema nor a file.
    nlohmann::json contractJson(std::uint32_t index) const;
    std::vector<std::uint32_t> contractIndices() const;
    std::map<std::uint32_t, std::uint64_t> generations() const;

    // Decoder + reader of a contract for state.* / table.* calls. Throws rpc::Error: not_found (no such contract,
    // no file, unknown contract), schema_error (layout unavailable), io_error (file cannot be opened).
    View view(std::uint32_t index) const;
    // Reader only (state.bytes, state.digest): works for every contract that has a file.
    View fileView(std::uint32_t index) const;

    // Re-stats the files of these contracts; bumps the generation of those that changed.
    RefreshOutcome refreshFiles(const std::vector<std::uint32_t>& indices);
    std::vector<std::uint32_t> indicesWithFiles() const;

    // Starts the directory watcher: contract files of the shown epoch ("state" tag) and, for working tree sources,
    // the core source files ("src" tag). `sink` runs on the watcher thread and must be quick.
    using EventSink = std::function<void(std::uint64_t workspaceId, const support::WatchEvent&)>;
    void startWatching(EventSink sink);
    // Stops the watcher and waits for it. Idempotent; never call from a watcher callback.
    void stop();

private:
    struct Entry {
        std::uint32_t index = 0;
        const schema::ContractSchema* contract = nullptr;
        std::optional<support::StateFileEntry> file; // name / path / size / mtime as of the last refresh
        std::shared_ptr<support::FileReader> reader;
        std::shared_ptr<decode::StateDecoder> decoder;
        std::uint64_t generation = 1;
    };

    Workspace() = default;
    static const schema::Schema& emptySchema();
    Entry* find(std::uint32_t index);
    const Entry* find(std::uint32_t index) const;
    nlohmann::json contractJsonLocked(const Entry& e) const;
    std::pair<const char*, std::string> statusOf(const Entry& e) const;
    std::vector<Diagnostic> workspaceDiagnostics() const;
    View makeView(std::uint32_t index, bool needDecoder) const;

    std::uint64_t id_ = 0;
    support::WorkspaceRequest request_;
    std::shared_ptr<const CoreBundle> core_;
    support::StateDirScan scan_;
    std::optional<std::uint32_t> epochExt_;
    WorkspaceDeps deps_;

    mutable std::mutex mutex_; // entries_ contents (not the vector layout, which is fixed after create())
    mutable std::vector<Entry> entries_;

    std::mutex watchMutex_;
    std::unique_ptr<support::DirWatcher> watcher_;
    bool stopped_ = false;
};

} // namespace qstate::service
