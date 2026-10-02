// Safe helper around the `git` executable: argv only (no shell), bounded run time, captured output. Used to read
// the Qubic core sources from a tag instead of the working tree.
//
// Thread-safety: a GitRepo holds only immutable configuration and may be used from several threads. exportTree()
// is safe to call concurrently for the same cache directory (the export is built in a private temporary directory
// and renamed into place atomically).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qstate::support {

struct GitOptions {
    std::string executable = "git";
    int timeoutMs = 60000;   // per git invocation; `git archive` of a big tree gets 4x this
};

struct GitTag {
    std::string name;       // "v1.306.0"
    std::string commitSha;  // peeled commit
    std::string date;       // creator date (tagger date for annotated tags, commit date otherwise), ISO 8601
};

// Contents of `#define EPOCH` / VERSION_A/B/C in src/public_settings.h, plus commit data.
struct GitVersionInfo {
    std::string ref;                       // as requested; "" = working tree
    std::string sha;                       // commit sha ("" when the working tree is not a git repository)
    std::string date;                      // ISO 8601 commit date of `sha` ("" when unknown)
    std::optional<int> epoch;
    std::optional<std::string> version;    // "1.306.0"
};

struct GitExportResult {
    std::string dir;       // directory holding the exported tree (inside the cache root)
    std::string sha;       // commit that was exported
    bool fromCache = false;
};

// Parses the text of src/public_settings.h (no git involved).
GitVersionInfo parsePublicSettings(const std::string& headerText);

class GitRepo {
public:
    explicit GitRepo(std::string dir, GitOptions options = {});

    const std::string& dir() const { return dir_; }

    // `git --version` runs successfully.
    static bool isAvailable(const GitOptions& options = {});
    // `dir` is the top level of a git work tree (not merely a sub directory of one).
    static bool isRepo(const std::string& dir, const GitOptions& options = {});
    bool isRepo() const { return isRepo(dir_, options_); }

    // Commit sha of a ref (tag, branch, sha, HEAD); nullopt when unknown or git fails. Refs starting with '-' are
    // rejected.
    std::optional<std::string> resolveCommit(const std::string& ref) const;
    // Tags, newest first by creator date; limit 0 = all.
    std::vector<GitTag> listTags(size_t limit = 0) const;
    // Content of `path` at `ref` (a ref name or sha). nullopt when the file or ref does not exist.
    std::optional<std::string> showFile(const std::string& ref, const std::string& path) const;
    // Version info of src/public_settings.h at `ref`, or of the working tree when ref is empty. nullopt when the
    // file cannot be read.
    std::optional<GitVersionInfo> readVersionInfo(const std::string& ref) const;

    // Exports the tree of `ref` into `<cacheRoot>/<sha>[-<subpath hash>]/` (git archive streamed into TarExtractor)
    // and returns that directory; an earlier complete export of the same commit and subpaths is reused. Empty
    // `subpaths` = src/, lib/ and every file in the repository root. Subpaths that do not exist at that commit are
    // skipped. Throws std::runtime_error on failure.
    GitExportResult exportTree(const std::string& ref, const std::string& cacheRoot,
                               const std::vector<std::string>& subpaths = {}) const;

    // Newest tag (by creator date) whose `#define EPOCH` equals `epoch`; nullopt when none.
    std::optional<std::string> autoPickRef(int epoch) const;

private:
    struct RunResult {
        bool ok = false;
        std::string out;
        std::string err;
    };
    RunResult run(const std::vector<std::string>& args, size_t maxOutput = 64u << 20) const;

    std::string dir_;
    GitOptions options_;
};

} // namespace qstate::support
