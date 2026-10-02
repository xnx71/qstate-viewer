// Safe helper around the `git` executable: argv only (no shell), captured output, bounded run time for the short
// queries, cancellable. GitRepo reads one repository (in this application always a bare mirror, see git_mirror.h);
// it never touches a work tree.
//
// Thread-safety: a GitRepo holds only immutable configuration and may be used from several threads. exportTree()
// is safe to call concurrently for the same cache directory (the export is built in a private temporary directory
// and renamed into place atomically).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace qstate::support {

// Failure of a git operation. `kind` tells the caller how to report it.
class GitError : public std::runtime_error {
public:
    enum class Kind {
        Failed,       // git ran and reported an error (message = its last "fatal:" / "error:" line)
        Cancelled,    // the cancel callback returned true; the child process is gone
        BadInput,     // a URL / ref / path that is refused before git is started
        Unavailable   // the git executable cannot be started
    };
    GitError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

struct GitOptions {
    std::string executable = "git";
    int timeoutMs = 60000;                 // per short git invocation (ls, log, grep); `git archive` gets 4x this
    std::function<bool()> cancel;          // polled while a git process runs
};

// Contents of `#define EPOCH` / VERSION_A/B/C in src/public_settings.h.
struct PublicSettings {
    std::optional<int> epoch;
    std::optional<std::string> version;    // "1.306.0"
};

// Parses the text of src/public_settings.h (no git involved).
PublicSettings parsePublicSettings(const std::string& headerText);

enum class RefKind { Tag, Branch, Commit };
const char* refKindName(RefKind kind);   // "tag" | "branch" | "commit"

struct GitRef {
    std::string name;   // "v1.306.0", "main"
    RefKind kind = RefKind::Tag;
    std::string sha;    // peeled commit
    std::string date;   // ISO 8601: creator date of a tag (tagger date for annotated tags), commit date of a branch
};

struct GitCommit {
    std::string sha;
    std::string date;      // ISO 8601 commit date
    std::string subject;   // first line of the message
};

struct GitCommitQuery {
    std::string sha;       // commit to start from (full sha)
    size_t limit = 50;
    size_t skip = 0;
    std::string search;    // case-insensitive substring of the subject, or a sha prefix; "" = all
};

struct GitCommitPage {
    std::vector<GitCommit> commits;    // newest first
    std::optional<size_t> total;       // commits matching the query (absent when the scan was cut short)
};

struct GitExportResult {
    std::string dir;       // directory holding the exported tree (inside the cache root)
    std::string sha;       // commit that was exported
    bool fromCache = false;
};

// Ref names that are safe to hand to git: no option-like names, no revision syntax (`^`, `~`, `:`, `@{`), no
// control characters. Branch and tag names that git itself accepts pass.
bool isValidRefName(const std::string& ref);
bool isHexSha(const std::string& s);   // 40 or 64 hex digits

class GitRepo {
public:
    explicit GitRepo(std::string dir, GitOptions options = {});

    const std::string& dir() const { return dir_; }

    // `git --version` runs successfully.
    static bool isAvailable(const GitOptions& options = {});

    struct Resolved {
        std::string name;   // tag / branch name, or the full sha for a commit
        RefKind kind = RefKind::Commit;
        std::string sha;    // full commit sha
    };
    // A tag, a branch, "HEAD" (the default branch) or a commit sha (full or abbreviated, >= 4 digits), in that order.
    // nullopt when unknown or the name is refused.
    std::optional<Resolved> resolve(const std::string& ref) const;

    // Tags newest first (creator date). Branches: the default branch first, then newest first.
    std::vector<GitRef> listTags() const;
    std::vector<GitRef> listBranches() const;
    // Branch HEAD points to (nullopt when HEAD is detached or dangling).
    std::optional<std::string> defaultBranch() const;

    // src/public_settings.h of many commits with ONE git process (git grep). Commits where the file is missing or
    // has neither define map to an empty PublicSettings.
    std::map<std::string, PublicSettings> publicSettings(const std::vector<std::string>& shas) const;

    // Content of `path` at `sha`. nullopt when the file or commit does not exist.
    std::optional<std::string> showFile(const std::string& sha, const std::string& path) const;

    GitCommitPage log(const GitCommitQuery& query) const;

    // Exports the tree of `ref` into `<cacheRoot>/<sha>[-<subpath hash>]/` (git archive streamed into TarExtractor)
    // and returns that directory; an earlier complete export of the same commit and subpaths is reused. Empty
    // `subpaths` = src/, lib/ and every file in the repository root. Subpaths that do not exist at that commit are
    // skipped. Throws GitError.
    GitExportResult exportTree(const std::string& sha, const std::string& cacheRoot,
                               const std::vector<std::string>& subpaths = {}) const;

private:
    struct RunResult {
        bool ok = false;
        int exitCode = -1;
        std::string out;
        std::string err;
    };
    // Throws GitError(Cancelled) when the cancel callback fired; every other failure is reported in the result.
    RunResult run(const std::vector<std::string>& args, size_t maxOutput = 64u << 20) const;
    std::vector<GitRef> listRefs(const char* pattern, RefKind kind) const;

    std::string dir_;
    GitOptions options_;
};

// The environment every git child gets: never prompt, never page, protocols limited to the safe set
// (file, git, http, https, ssh; `ext::` and friends are refused), english messages.
std::vector<std::pair<std::string, std::string>> gitEnvironment();

// The most useful line of a git process's stderr: the last "fatal:" / "error:" line, else the last non-empty one.
std::string gitErrorLine(const std::string& stderrText);

} // namespace qstate::support
