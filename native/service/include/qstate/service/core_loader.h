// Core source resolution + schema extraction, shared by workspace.open and the command line tool.
//
// "Which sources define the schema": the working tree of the core checkout, a git ref exported into a cache
// directory, or (coreRef "auto") the newest tag whose `#define EPOCH` equals the epoch of the state files.
#pragma once

#include "qstate/schema/model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qstate::service {

// contract.ts CoreInfo.
struct CoreInfo {
    std::string dir;
    std::string sourceDir;
    std::string ref;  // "" = working tree
    std::string sha;
    std::string version;
    std::optional<int> epoch;
    double parseMs = 0;
    std::size_t fileCount = 0;
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
    std::string coreDir;
    // "" = working tree, "auto" = newest tag with EPOCH == `epoch`, anything else = git ref.
    std::string coreRef;
    // Epoch of the state files (used by "auto" and for the epoch-mismatch warning).
    std::optional<int> epoch;
    // "NAME" or "NAME=VALUE".
    std::vector<std::string> defines;
    // Export cache for git refs; "" = default (defaultCoreCacheDir()).
    std::string cacheDir;
};

struct LoadedCore {
    CoreInfo info;
    std::shared_ptr<const schema::Schema> schema;
    // Files read by the extraction, relative to info.sourceDir.
    std::vector<std::string> files;
    std::vector<Diagnostic> diagnostics;
    // True when info.sourceDir is the user's working tree (so its files are worth watching).
    bool workingTree = true;
    // The request that produced it (epoch / ref / defines), to decide whether it can be reused.
    CoreRequest request;
};

// <cache dir>/qstate-viewer/core (Linux: $XDG_CACHE_HOME or ~/.cache).
std::string defaultCoreCacheDir();

// Throws rpc::Error: invalid_params (empty dir, unknown ref, git unavailable for an explicit ref),
// io_error (directory missing, git export failed), schema_error (nothing could be extracted: the first error
// diagnostic is the message), rpc::Cancelled when `cancelled()` became true. Extraction problems that still leave
// some contracts are returned as diagnostics.
LoadedCore loadCore(const CoreRequest& request, const std::function<bool()>& cancelled = {});

// Same, but returns the result even when no contract could be extracted (LoadedCore::schema->contracts empty,
// diagnostics hold the errors).
LoadedCore loadCoreLenient(const CoreRequest& request, const std::function<bool()>& cancelled = {});

// Probe of `git --version`, cached for the process.
bool gitAvailable();

} // namespace qstate::service
