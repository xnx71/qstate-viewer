// Local bare mirrors of git repositories: one directory per repository URL below a cache root, created by
// `git clone --bare` on first use and updated by `git fetch` later. Everything else (listing refs, reading files,
// exporting a tree) then works on the mirror without the network, see GitRepo.
//
// Layout: <reposDir>/<name>-<hash of the URL>.git  (bare repository) with two files of ours inside:
//   qstate-mirror.json  {"url": ..., "fetchedAt": ISO time of the last successful clone / fetch}
//   qstate-facts.json   memo of src/public_settings.h per commit sha (version, epoch); commits never change
// A mirror is only visible once its clone completed (it is built in a temporary directory and renamed).
//
// Thread-safety: all members may be called from any thread. Operations on the same mirror are serialised; a second
// sync of a mirror that is being synced waits for the first one.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "qstate/support/git.h"

namespace qstate::support {

// One step of clone / fetch progress, parsed from `git --progress` (stderr).
struct MirrorProgress {
    std::string phase;           // "clone" | "fetch"
    std::string message;         // e.g. "Receiving objects: 45% (7483/16627), 1.2 MiB | 2.0 MiB/s"
    std::optional<int> percent;  // overall 0..100 over counting, compressing, receiving and resolving
};
using MirrorProgressFn = std::function<void(const MirrorProgress&)>;

// One line of git's progress output ("remote: Compressing objects:  12% (58/480)"): label and percent.
struct GitProgressLine {
    std::string label;     // "Receiving objects"
    int percent = 0;       // 0..100 of this stage
    std::string text;      // the line without the "remote: " prefix
};
std::optional<GitProgressLine> parseGitProgressLine(const std::string& line);

// Overall percent of a stage: counting 0-5, compressing 5-20, receiving 20-80, resolving 80-100.
std::optional<int> overallPercent(const GitProgressLine& line);

class GitMirrorStore {
public:
    // `reposDir` is created on first clone.
    explicit GitMirrorStore(std::string reposDir, GitOptions options = {});

    // "" when `url` may be handed to git, else why not (empty, starts with '-', control characters, ...).
    static std::string checkUrl(const std::string& url);
    // Trimmed; an existing local directory becomes an absolute, normalized path (so "." and "/x/" equal "/x").
    static std::string normalizeUrl(const std::string& url);

    const std::string& reposDir() const { return reposDir_; }
    std::string mirrorDir(const std::string& url) const;
    bool exists(const std::string& url) const;
    // ISO time of the last successful clone / fetch; nullopt when there is no mirror.
    std::optional<std::string> fetchedAt(const std::string& url) const;

    struct SyncResult {
        std::string dir;
        bool cloned = false;
        bool fetched = false;
    };
    enum class Mode {
        CloneOrFetch,   // clone when missing, fetch otherwise
        CloneIfMissing  // clone when missing, otherwise leave the mirror alone
    };
    // Throws GitError: BadInput (url refused), Unavailable (no git), Failed (git's message), Cancelled (the child is
    // stopped and a partial clone directory removed; an interrupted fetch leaves the mirror as it was).
    // `cancel` is polled about every 100 ms. There is no timeout: a slow clone is a slow network.
    SyncResult sync(const std::string& url, Mode mode, const MirrorProgressFn& progress = {},
                    const std::function<bool()>& cancel = {});

    // Reader for the mirror; the mirror must exist.
    GitRepo open(const std::string& url, std::function<bool()> cancel = {}) const;

    // src/public_settings.h facts of `shas` through the on-disk memo; commits that are not memoized yet are read in
    // ONE git process and the memo is extended. The mirror must exist.
    std::map<std::string, PublicSettings> publicSettings(const std::string& url, const std::vector<std::string>& shas,
                                                         std::function<bool()> cancel = {});

private:
    std::shared_ptr<std::mutex> lockFor(const std::string& dir);
    GitOptions optionsWith(std::function<bool()> cancel) const;

    std::string reposDir_;
    GitOptions options_;
    std::mutex locksMutex_;
    std::map<std::string, std::shared_ptr<std::mutex>> locks_;
};

} // namespace qstate::support
