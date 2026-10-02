// Persistent application settings (the `Settings` shape of ui/src/rpc/contract.ts) stored as JSON in the platform
// configuration directory. Reads are tolerant (missing or corrupt files give defaults, bad fields are dropped),
// writes are atomic (temporary file + fsync + rename).
//
// Thread-safety: SettingsStore serialises all operations with an internal mutex; it may be shared between threads.
// Two processes writing the same file race benignly (last writer wins, the file is never torn).
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace qstate::support {

inline constexpr size_t kMaxRecentWorkspaces = 10;

// contract.ts WorkspaceRequest.
struct WorkspaceRequest {
    std::string coreDir;
    std::optional<std::string> coreRef;   // "" / nullopt = working tree, "auto" = pick by epoch
    std::string stateDir;
    std::optional<int> epoch;
    std::vector<std::string> defines;     // omitted from JSON when empty

    bool operator==(const WorkspaceRequest&) const = default;
};

// contract.ts Settings. `theme` is "dark" | "light" | "system".
struct Settings {
    std::string theme = "system";
    std::vector<WorkspaceRequest> recentWorkspaces;   // most recent first, at most kMaxRecentWorkspaces
    nlohmann::json ui = nlohmann::json::object();     // free-form UI preferences owned by the UI

    bool operator==(const Settings&) const = default;
};

// JSON conversion in the contract.ts shapes. from_json never throws: invalid or missing fields keep their defaults,
// invalid recent entries are skipped and the list is truncated to kMaxRecentWorkspaces.
void to_json(nlohmann::json& j, const WorkspaceRequest& request);
void from_json(const nlohmann::json& j, WorkspaceRequest& request);
void to_json(nlohmann::json& j, const Settings& settings);
void from_json(const nlohmann::json& j, Settings& settings);

bool isValidTheme(const std::string& theme);

// Inserts `request` at the front of `list`; an existing entry with the same coreDir and stateDir (trailing path
// separators ignored) is replaced; the list is truncated to `maxEntries`.
void addRecentWorkspace(std::vector<WorkspaceRequest>& list, const WorkspaceRequest& request,
                        size_t maxEntries = kMaxRecentWorkspaces);

class SettingsStore {
public:
    // `path` = the settings file; empty = defaultPath(). Nothing is read or created until first use.
    explicit SettingsStore(std::string path = {});

    // <config base>/qstate-viewer/settings.json (Linux: $XDG_CONFIG_HOME or ~/.config; Windows: %APPDATA%;
    // macOS: ~/Library/Application Support). Falls back to a relative "qstate-viewer-settings.json" when no home
    // directory can be determined.
    static std::string defaultPath();

    const std::string& path() const { return path_; }

    // Re-reads the file (discarding cached state) and returns the result. Missing file: defaults, no error.
    // Corrupt file: defaults, lastError() describes it, and a copy is kept as "<path>.corrupt".
    Settings load();
    // Cached settings (loaded on first call).
    Settings get();

    // Replaces the settings and writes the file. Returns false (and sets lastError()) when writing failed; the
    // in-memory settings are updated either way.
    bool set(const Settings& settings);

    // Applies a contract.ts `settings.update` patch: {"theme": ..., "ui": {...}}. Unknown keys and an invalid theme are
    // ignored; `ui` keys are merged one level deep (a null value removes the key). Returns the new settings.
    Settings applyPatch(const nlohmann::json& patch);

    // Records a successfully opened workspace (most recent first, deduplicated, max 10) and saves.
    Settings addRecentWorkspace(const WorkspaceRequest& request);
    Settings clearRecentWorkspaces();

    // Runs `mutate` on the settings under the lock, then saves.
    Settings modify(const std::function<void(Settings&)>& mutate);

    // Description of the last load / save problem ("" when the last operation succeeded).
    std::string lastError() const;

private:
    Settings loadLocked();
    bool saveLocked();
    void ensureLoadedLocked();

    std::string path_;
    mutable std::mutex mutex_;
    Settings cached_;
    bool loaded_ = false;
    std::string error_;
};

} // namespace qstate::support
