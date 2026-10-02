// Method group "fs": fs.list, core.versions
#include "module.h"
#include "qstate/cpp/source.h"
#include "qstate/rpc/params.h"
#include "qstate/service/core_loader.h"
#include "qstate/support/dir_scan.h"
#include "qstate/support/git.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <optional>

namespace qstate::service {

namespace {

using nlohmann::json;
using rpc::Code;

json entryJson(const support::FsEntry& e) {
    json j = {{"name", e.name}, {"path", e.path}, {"kind", e.isDir ? "dir" : "file"}};
    if (e.size) j["size"] = *e.size;
    if (e.mtimeMs) j["mtimeMs"] = *e.mtimeMs;
    return j;
}

json fsList(const json& params) {
    const std::string path = rpc::requireParam<std::string>(params, "path");
    const bool showHidden = rpc::optionalParam<bool>(params, "showHidden").value_or(false);
    std::string target = path;
    if (target.empty()) {
        target = support::homeDir();
        if (target.empty()) throw rpc::Error(Code::IoError, "the home directory is unknown");
    }
    support::FsListResult r = support::listDirectory(target, showHidden);
    if (!r.ok) throw rpc::Error(Code::IoError, r.error.empty() ? "cannot list '" + target + "'" : r.error);
    json entries = json::array();
    for (const support::FsEntry& e : r.listing.entries) entries.push_back(entryJson(e));
    return {
        {"path", r.listing.path},
        {"parent", r.listing.parent ? json(*r.listing.parent) : json(nullptr)},
        {"entries", std::move(entries)},
        {"hints",
         {{"isCoreRepo", r.listing.hints.isCoreRepo},
          {"isGitRepo", r.listing.hints.isGitRepo},
          {"stateEpochs", r.listing.hints.stateEpochs}}},
    };
}

// Version / epoch of a commit never change: memoize by sha (core.versions is called whenever the user opens the
// version picker, and each tag costs a git process).
struct TagFacts {
    std::optional<std::string> version;
    std::optional<int> epoch;
};

class TagFactsCache {
public:
    std::optional<TagFacts> find(const std::string& sha) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(sha);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    void put(const std::string& sha, TagFacts facts) {
        std::lock_guard lock(mutex_);
        map_[sha] = std::move(facts);
    }

private:
    std::mutex mutex_;
    std::map<std::string, TagFacts> map_;
};

TagFactsCache& tagFactsCache() {
    static TagFactsCache cache;
    return cache;
}

json versionJson(const std::string& ref, const char* kind, const std::string& sha, const std::optional<std::string>& version,
                 const std::optional<int>& epoch, const std::string& date) {
    json j = {{"ref", ref}, {"kind", kind}};
    if (!sha.empty()) j["sha"] = sha;
    if (version) j["version"] = *version;
    if (epoch) j["epoch"] = *epoch;
    if (!date.empty()) j["date"] = date;
    return j;
}

json coreVersions(const json& params) {
    const std::string coreDir = rpc::requireParam<std::string>(params, "coreDir");
    const std::int64_t limit = rpc::optionalParam<std::int64_t>(params, "limit").value_or(30);
    if (limit < 1 || limit > 1000) throw rpc::Error(Code::InvalidParams, "params.limit must be between 1 and 1000");
    if (coreDir.empty()) throw rpc::Error(Code::InvalidParams, "params.coreDir must not be empty");
    const std::string dir = support::normalizePath(coreDir);
    if (!support::isDirectory(dir)) throw rpc::Error(Code::IoError, "'" + dir + "' is not a directory");

    support::GitRepo repo(dir);
    const bool git = gitAvailable() && support::pathExists(dir + "/.git") && repo.isRepo();

    json worktree = {{"ref", ""}, {"kind", "worktree"}};
    if (git) {
        if (auto info = repo.readVersionInfo("")) {
            worktree = versionJson("", "worktree", info->sha, info->version, info->epoch, info->date);
        }
    } else if (auto text = cpp::DiskSource(dir).read("src/public_settings.h")) {
        const support::GitVersionInfo info = support::parsePublicSettings(*text);
        worktree = versionJson("", "worktree", "", info.version, info.epoch, "");
    }

    json refs = json::array();
    if (git) {
        const std::vector<support::GitTag> tags = repo.listTags(static_cast<std::size_t>(limit));
        std::vector<json> out(tags.size());
        // One `git show` per uncached tag: run a few at a time.
        std::atomic<std::size_t> next{0};
        auto work = [&] {
            for (std::size_t i = next.fetch_add(1); i < tags.size(); i = next.fetch_add(1)) {
                const support::GitTag& tag = tags[i];
                TagFacts facts;
                if (auto cached = tagFactsCache().find(tag.commitSha)) {
                    facts = *cached;
                } else {
                    if (auto text = repo.showFile(tag.commitSha, "src/public_settings.h")) {
                        const support::GitVersionInfo info = support::parsePublicSettings(*text);
                        facts.version = info.version;
                        facts.epoch = info.epoch;
                    }
                    tagFactsCache().put(tag.commitSha, facts);
                }
                out[i] = versionJson(tag.name, "tag", tag.commitSha, facts.version, facts.epoch, tag.date);
            }
        };
        const std::size_t threads = std::min<std::size_t>(8, tags.size());
        std::vector<std::future<void>> pool;
        for (std::size_t t = 1; t < threads; ++t) pool.push_back(std::async(std::launch::async, work));
        work();
        for (auto& f : pool) f.get();
        for (json& j : out) refs.push_back(std::move(j));
    }
    return {{"worktree", std::move(worktree)}, {"refs", std::move(refs)}};
}

} // namespace

void registerFsMethods(ModuleContext& ctx) {
    ctx.add("fs.list", [](const json& params, rpc::CallContext&) { return fsList(params); });
    ctx.add("core.versions", [](const json& params, rpc::CallContext&) { return coreVersions(params); });
}

} // namespace qstate::service
