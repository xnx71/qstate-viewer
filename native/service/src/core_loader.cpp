#include "qstate/service/core_loader.h"

#include "qstate/cpp/source.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/error.h"
#include "qstate/schema/extract.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace qstate::service {

namespace fs = std::filesystem;
using nlohmann::json;
using rpc::Code;
using support::GitError;
using support::GitMirrorStore;

void to_json(json& j, const CoreInfo& v) {
    j = {{"repoUrl", v.repoUrl}, {"ref", v.ref}, {"kind", v.kind}, {"parseMs", v.parseMs}, {"fileCount", v.fileCount}};
    if (!v.sha.empty()) j["sha"] = v.sha;
    if (!v.version.empty()) j["version"] = v.version;
    if (v.epoch) j["epoch"] = *v.epoch;
}

void to_json(json& j, const Diagnostic& v) {
    const char* severity = v.severity == Diagnostic::Severity::Error     ? "error"
                           : v.severity == Diagnostic::Severity::Warning ? "warning"
                                                                         : "note";
    j = {{"severity", severity}, {"message", v.message}};
    if (!v.file.empty()) j["file"] = v.file;
    if (v.line > 0) j["line"] = v.line;
    if (v.contract) j["contract"] = *v.contract;
}

namespace {

constexpr const char* kRootFile = "src/contract_core/contract_def.h";
constexpr const char* kVersionHeader = "src/public_settings.h";

// SourceProvider decorator: once cancelled every read fails, which makes a running extraction wind down quickly.
class CancellableSource final : public cpp::SourceProvider {
public:
    CancellableSource(const cpp::SourceProvider& inner, const std::function<bool()>& cancelled)
        : inner_(inner), cancelled_(cancelled) {}
    std::optional<std::string> read(std::string_view path) const override {
        if (isCancelled()) return std::nullopt;
        return inner_.read(path);
    }
    bool exists(std::string_view path) const override { return !isCancelled() && inner_.exists(path); }

private:
    bool isCancelled() const { return cancelled_ && cancelled_(); }
    const cpp::SourceProvider& inner_;
    const std::function<bool()>& cancelled_;
};

Diagnostic makeDiag(Diagnostic::Severity severity, std::string message) {
    Diagnostic d;
    d.severity = severity;
    d.message = std::move(message);
    return d;
}

void throwIfCancelled(const CoreCall& call) {
    if (call.cancelled && call.cancelled()) throw rpc::Cancelled();
}

[[noreturn]] void rethrow(const GitError& e) {
    switch (e.kind()) {
        case GitError::Kind::Cancelled: throw rpc::Cancelled();
        case GitError::Kind::BadInput: throw rpc::Error(Code::InvalidParams, e.what());
        case GitError::Kind::Unavailable:
        case GitError::Kind::Failed: break;
    }
    throw rpc::Error(Code::IoError, e.what());
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::vector<std::pair<std::string, std::string>> parseDefines(const std::vector<std::string>& defines) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& d : defines) {
        if (d.empty()) continue;
        const auto eq = d.find('=');
        if (eq == std::string::npos) {
            out.emplace_back(d, "1");
        } else {
            out.emplace_back(d.substr(0, eq), d.substr(eq + 1));
        }
    }
    return out;
}

json versionJson(const std::string& ref, support::RefKind kind, const std::string& sha, const support::PublicSettings& facts,
                 const std::string& date, const std::string& subject = {}) {
    json j = {{"ref", ref}, {"kind", support::refKindName(kind)}, {"sha", sha}};
    if (facts.version) j["version"] = *facts.version;
    if (facts.epoch) j["epoch"] = *facts.epoch;
    if (!date.empty()) j["date"] = date;
    if (!subject.empty()) j["subject"] = subject;
    return j;
}

} // namespace

CoreLoader::CoreLoader(std::string cacheDir, support::GitOptions git)
    : git_(std::move(git)),
      mirrors_(std::make_unique<GitMirrorStore>((fs::path(cacheDir) / "repos").string(), git_)),
      exportDir_((fs::path(cacheDir) / "core").string()) {}

bool CoreLoader::gitAvailable() const {
    std::lock_guard lock(probeMutex_);
    if (!available_) available_ = support::GitRepo::isAvailable(git_);
    return *available_;
}

void CoreLoader::requireGit() const {
    if (!gitAvailable()) {
        throw rpc::Error(Code::IoError, "git is not available: the core sources are fetched with the git executable (install git and make sure it is on the PATH)");
    }
}

void CoreLoader::requireValidUrl(const std::string& repoUrl) const {
    if (const std::string why = GitMirrorStore::checkUrl(repoUrl); !why.empty()) throw rpc::Error(Code::InvalidParams, why);
}

GitMirrorStore::SyncResult CoreLoader::syncMirror(const std::string& repoUrl, GitMirrorStore::Mode mode, const CoreCall& call) {
    auto progress = [&](const support::MirrorProgress& p) {
        if (call.progress) call.progress(p.phase, p.message, p.percent);
    };
    try {
        return mirrors_->sync(repoUrl, mode, progress, call.cancelled);
    } catch (const GitError& e) {
        rethrow(e);
    }
}

json CoreLoader::sync(const std::string& repoUrlIn, bool offline, const CoreCall& call) {
    const std::string repoUrl = trim(repoUrlIn);
    requireValidUrl(repoUrl);
    requireGit();
    throwIfCancelled(call);
    if (offline) {
        if (!mirrors_->exists(repoUrl)) {
            throw rpc::Error(Code::IoError, "there is no local copy of '" + repoUrl + "' yet: it is downloaded the first time it is used (needs the network)");
        }
    } else {
        try {
            syncMirror(repoUrl, GitMirrorStore::Mode::CloneOrFetch, call);
        } catch (const rpc::Error&) {
            if (!mirrors_->exists(repoUrl)) throw;  // nothing to fall back on
            // A stale mirror is still useful (offline, repository gone, ...): list it as it is.
        }
    }
    try {
        const support::GitRepo repo = mirrors_->open(repoUrl, call.cancelled);
        const std::vector<support::GitRef> tags = repo.listTags();
        const std::vector<support::GitRef> branches = repo.listBranches();
        std::vector<std::string> shas;
        for (const auto& r : tags) shas.push_back(r.sha);
        for (const auto& r : branches) shas.push_back(r.sha);
        const auto facts = mirrors_->publicSettings(repoUrl, shas, call.cancelled);
        json out = {{"repoUrl", repoUrl}, {"tags", json::array()}, {"branches", json::array()}};
        for (const auto& r : tags) out["tags"].push_back(versionJson(r.name, r.kind, r.sha, facts.at(r.sha), r.date));
        for (const auto& r : branches) out["branches"].push_back(versionJson(r.name, r.kind, r.sha, facts.at(r.sha), r.date));
        if (const auto def = repo.defaultBranch()) out["defaultBranch"] = *def;
        out["fetchedAt"] = mirrors_->fetchedAt(repoUrl).value_or("");
        return out;
    } catch (const GitError& e) {
        rethrow(e);
    }
}

json CoreLoader::commits(const std::string& repoUrlIn, const std::string& refIn, std::size_t limit, std::size_t skip,
                         const std::string& search, const CoreCall& call) {
    const std::string repoUrl = trim(repoUrlIn);
    requireValidUrl(repoUrl);
    requireGit();
    throwIfCancelled(call);
    if (!mirrors_->exists(repoUrl)) {
        throw rpc::Error(Code::IoError, "there is no local copy of '" + repoUrl + "': call core.sync first");
    }
    try {
        const support::GitRepo repo = mirrors_->open(repoUrl, call.cancelled);
        const auto resolved = repo.resolve(trim(refIn));
        if (!resolved) throw rpc::Error(Code::InvalidParams, "unknown ref '" + refIn + "' in '" + repoUrl + "'");
        support::GitCommitQuery query;
        query.sha = resolved->sha;
        query.limit = limit;
        query.skip = skip;
        query.search = search;
        const support::GitCommitPage page = repo.log(query);
        std::vector<std::string> shas;
        for (const auto& c : page.commits) shas.push_back(c.sha);
        const auto facts = mirrors_->publicSettings(repoUrl, shas, call.cancelled);
        json out = {{"commits", json::array()}};
        if (page.total) out["total"] = *page.total;
        for (const auto& c : page.commits) {
            out["commits"].push_back(versionJson(c.sha, support::RefKind::Commit, c.sha, facts.at(c.sha), c.date, c.subject));
        }
        return out;
    } catch (const GitError& e) {
        rethrow(e);
    }
}

LoadedCore CoreLoader::loadLenient(const CoreRequest& request, const CoreCall& call) {
    LoadedCore out;
    out.request = request;
    const std::string repoUrl = trim(request.repoUrl);
    const std::string ref = trim(request.ref);
    requireValidUrl(repoUrl);
    if (ref.empty()) throw rpc::Error(Code::InvalidParams, "params.core.ref must not be empty");
    if (ref != "auto" && !support::isValidRefName(ref)) throw rpc::Error(Code::InvalidParams, "'" + ref + "' is not a valid tag, branch or commit name");
    requireGit();
    throwIfCancelled(call);

    // The mirror: cloned when missing; fetched at most once per call, and only when the ref cannot be found.
    bool synced = false;
    if (!mirrors_->exists(repoUrl)) {
        if (!call.allowNetwork) {
            throw rpc::Error(Code::IoError, "there is no local copy of '" + repoUrl + "' (it is downloaded when the workspace is opened)");
        }
        syncMirror(repoUrl, GitMirrorStore::Mode::CloneIfMissing, call);
        synced = true;
    }
    auto refetch = [&]() {
        if (synced || !call.allowNetwork) return false;
        synced = true;
        try {
            syncMirror(repoUrl, GitMirrorStore::Mode::CloneOrFetch, call);
        } catch (const rpc::Error&) {
            return false;  // offline: work with what the mirror has
        }
        return true;
    };

    std::optional<support::GitRepo::Resolved> resolved;
    try {
        const support::GitRepo repo = mirrors_->open(repoUrl, call.cancelled);
        auto pickAuto = [&]() -> std::optional<support::GitRepo::Resolved> {
            const std::vector<support::GitRef> tags = repo.listTags();
            std::vector<std::string> shas;
            for (const auto& t : tags) shas.push_back(t.sha);
            const auto facts = mirrors_->publicSettings(repoUrl, shas, call.cancelled);
            for (const auto& t : tags) {  // newest first
                if (facts.at(t.sha).epoch == request.epoch) return support::GitRepo::Resolved{t.name, support::RefKind::Tag, t.sha};
            }
            return std::nullopt;
        };
        if (ref == "auto") {
            if (!request.epoch) {
                out.diagnostics.push_back(makeDiag(Diagnostic::Severity::Warning,
                                                   "ref auto: the epoch of the state files is unknown; using the default branch"));
            } else {
                resolved = pickAuto();
                if (!resolved && refetch()) resolved = pickAuto();
            }
            if (!resolved) {
                resolved = repo.resolve("HEAD");
                if (!resolved) throw rpc::Error(Code::IoError, "the repository '" + repoUrl + "' has no branches");
                if (request.epoch) {
                    out.diagnostics.push_back(makeDiag(
                        Diagnostic::Severity::Warning, "ref auto: no tag with #define EPOCH " + std::to_string(*request.epoch) +
                                                           " found; using the head of branch '" + resolved->name + "'"));
                }
            }
        } else {
            resolved = repo.resolve(ref);
            if (!resolved && refetch()) resolved = repo.resolve(ref);
            if (!resolved) throw rpc::Error(Code::InvalidParams, "unknown ref '" + ref + "' in '" + repoUrl + "'");
        }
        throwIfCancelled(call);

        if (call.progress) call.progress("export", "Exporting " + resolved->name, std::nullopt);
        const support::GitExportResult exported = repo.exportTree(resolved->sha, exportDir_);
        out.info.sourceDir = exported.dir;
    } catch (const GitError& e) {
        rethrow(e);
    }
    out.info.repoUrl = repoUrl;
    out.info.ref = resolved->name;
    out.info.kind = support::refKindName(resolved->kind);
    out.info.sha = resolved->sha;
    throwIfCancelled(call);

    cpp::DiskSource disk{fs::path(out.info.sourceDir)};
    if (!disk.exists(kRootFile)) {
        throw rpc::Error(Code::SchemaError, std::string("'") + repoUrl + "' at " + resolved->name + " is not a Qubic core repository: " +
                                                kRootFile + " not found");
    }
    if (auto header = disk.read(kVersionHeader)) {
        const support::PublicSettings ps = support::parsePublicSettings(*header);
        if (ps.version) out.info.version = *ps.version;
        out.info.epoch = ps.epoch;
    }
    if (request.epoch && out.info.epoch && *request.epoch != *out.info.epoch) {
        out.diagnostics.push_back(makeDiag(
            Diagnostic::Severity::Warning,
            "the core sources are for epoch " + std::to_string(*out.info.epoch) + (out.info.version.empty() ? "" : " (version " + out.info.version + ")") +
                " but the state files are from epoch " + std::to_string(*request.epoch) +
                ": contract layouts may differ (use a core version with EPOCH " + std::to_string(*request.epoch) + ", e.g. ref \"auto\")"));
    }

    if (call.progress) call.progress("parse", "Reading the contract layouts", std::nullopt);
    schema::ExtractOptions options;
    for (auto& d : parseDefines(request.defines)) options.defines.push_back(std::move(d));
    CancellableSource source(disk, call.cancelled);
    schema::ExtractResult extracted = schema::extractSchema(source, options);
    throwIfCancelled(call);

    out.schema = extracted.schema;
    out.info.parseMs = extracted.stats.totalMs;
    out.info.fileCount = extracted.stats.fileCount;
    for (const cpp::Diag& d : extracted.schema->diags) {
        Diagnostic diag;
        diag.severity = d.severity == cpp::Diag::Severity::Error     ? Diagnostic::Severity::Error
                        : d.severity == cpp::Diag::Severity::Warning ? Diagnostic::Severity::Warning
                                                                     : Diagnostic::Severity::Note;
        diag.message = d.message;
        diag.file = d.file;
        diag.line = d.line;
        out.diagnostics.push_back(std::move(diag));
    }
    return out;
}

LoadedCore CoreLoader::load(const CoreRequest& request, const CoreCall& call) {
    LoadedCore core = loadLenient(request, call);
    if (core.schema->contracts.empty()) {
        std::string message = "no contracts could be extracted from the core sources";
        for (const Diagnostic& d : core.diagnostics) {
            if (d.severity == Diagnostic::Severity::Error) {
                message = d.message + (d.file.empty() ? "" : " (" + d.file + (d.line ? ":" + std::to_string(d.line) : "") + ")");
                break;
            }
        }
        throw rpc::Error(Code::SchemaError, message);
    }
    return core;
}

} // namespace qstate::service
