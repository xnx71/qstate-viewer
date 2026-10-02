// Where the schema comes from: the Qubic core sources of a git repository (GitHub by default).
//
// A repository URL is mirrored once into the cache directory (a bare clone, see support/git_mirror.h); a tag, a
// branch, a commit or "auto" (the newest tag whose `#define EPOCH` equals the epoch of the state files) is resolved on
// the mirror, exported into a content-addressed directory (<cache>/core/<sha>, immutable) and read with a DiskSource
// by `schema::extractSchema`. The same class serves `core.sync` / `core.commits`.
#pragma once

#include "qstate/schema/model.h"
#include "qstate/support/git.h"
#include "qstate/support/git_mirror.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace qstate::service {

// contract.ts CoreInfo (`sourceDir` is internal: where the exported tree lives).
struct CoreInfo {
    std::string repoUrl;
    std::string ref;            // the resolved tag / branch name, or the full sha (never "auto")
    std::string kind;           // "tag" | "branch" | "commit"
    std::string sha;
    std::string version;
    std::optional<int> epoch;
    double parseMs = 0;
    std::size_t fileCount = 0;
    std::string sourceDir;
};

// contract.ts Diagnostic.
struct Diagnostic {
    enum class Severity : std::uint8_t { Note, Warning, Error };
    Severity severity = Severity::Note;
    std::string message;
    std::string file;
    std::uint32_t line = 0;
    std::optional<std::uint32_t> contract;
};

void to_json(nlohmann::json& j, const CoreInfo& v);
void to_json(nlohmann::json& j, const Diagnostic& v);

struct CoreRequest {
    std::string repoUrl;
    // tag, branch, commit sha, or "auto".
    std::string ref;
    // Epoch of the state files (used by "auto" and for the epoch-mismatch warning).
    std::optional<int> epoch;
    // "NAME" or "NAME=VALUE".
    std::vector<std::string> defines;

    bool operator==(const CoreRequest&) const = default;
};

struct LoadedCore {
    CoreInfo info;
    std::shared_ptr<const schema::Schema> schema;
    std::vector<Diagnostic> diagnostics;
    // The request that produced it, to decide whether it can be reused.
    CoreRequest request;
};

// What a long running call hands to the loader.
struct CoreCall {
    // Polled between steps and while git runs; true makes the call throw rpc::Cancelled.
    std::function<bool()> cancelled;
    // phase "clone" | "fetch" | "export" | "parse" (contract.ts `core.progress`).
    std::function<void(const std::string& phase, const std::string& message, std::optional<int> percent)> progress;
    // false: never use the network (no clone, no fetch); a repository without a mirror is an io_error.
    bool allowNetwork = true;
};

class CoreLoader {
public:
    // `cacheDir` = the application cache directory: mirrors live in <cacheDir>/repos, exports in <cacheDir>/core.
    CoreLoader(std::string cacheDir, support::GitOptions git);

    // The `git` executable can be started (probed once).
    bool gitAvailable() const;

    // core.sync: clones or fetches (`offline`: reads the existing mirror only) and lists tags and branches with
    // version / epoch; shape of contract.ts CoreRepo. A failed fetch of an existing mirror is not an error (the
    // stale mirror is listed). Throws rpc::Error: invalid_params (bad URL), io_error (no git, network, no mirror
    // when offline), rpc::Cancelled.
    nlohmann::json sync(const std::string& repoUrl, bool offline, const CoreCall& call);

    // core.commits: commits of `ref` (tag, branch or sha) on the existing mirror, newest first.
    nlohmann::json commits(const std::string& repoUrl, const std::string& ref, std::size_t limit, std::size_t skip,
                           const std::string& search, const CoreCall& call);

    // Resolves the sources, exports them and extracts the schema. Throws rpc::Error: invalid_params (bad URL,
    // unknown ref), io_error (no git, clone / fetch / export failed), schema_error (nothing could be extracted),
    // rpc::Cancelled. Extraction problems that still leave some contracts are returned as diagnostics.
    LoadedCore load(const CoreRequest& request, const CoreCall& call);

    support::GitMirrorStore& mirrors() { return *mirrors_; }
    const std::string& exportDir() const { return exportDir_; }

private:
    // load() without the "no contract at all" check: the result may have an empty schema, the diagnostics hold why.
    LoadedCore loadLenient(const CoreRequest& request, const CoreCall& call);
    void requireGit() const;
    void requireValidUrl(const std::string& repoUrl) const;
    // Runs GitMirrorStore::sync with the call's progress / cancellation; maps GitError to rpc::Error.
    support::GitMirrorStore::SyncResult syncMirror(const std::string& repoUrl, support::GitMirrorStore::Mode mode,
                                                   const CoreCall& call);

    support::GitOptions git_;
    std::unique_ptr<support::GitMirrorStore> mirrors_;
    std::string exportDir_;
    mutable std::mutex probeMutex_;
    mutable std::optional<bool> available_;
};

} // namespace qstate::service
