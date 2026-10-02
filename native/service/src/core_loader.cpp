#include "qstate/service/core_loader.h"

#include "qstate/cpp/source.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/error.h"
#include "qstate/schema/extract.h"
#include "qstate/support/dir_scan.h"
#include "qstate/support/git.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace qstate::service {

namespace fs = std::filesystem;
using rpc::Code;

void to_json(nlohmann::json& j, const CoreInfo& v) {
    j = {{"dir", v.dir}, {"sourceDir", v.sourceDir}, {"ref", v.ref}, {"parseMs", v.parseMs}, {"fileCount", v.fileCount}};
    if (!v.sha.empty()) j["sha"] = v.sha;
    if (!v.version.empty()) j["version"] = v.version;
    if (v.epoch) j["epoch"] = *v.epoch;
}

void to_json(nlohmann::json& j, const Diagnostic& v) {
    const char* severity = v.severity == Diagnostic::Severity::Error     ? "error"
                           : v.severity == Diagnostic::Severity::Warning ? "warning"
                                                                         : "note";
    j = {{"severity", severity}, {"message", v.message}};
    if (!v.file.empty()) j["file"] = v.file;
    if (v.line > 0) j["line"] = v.line;
    if (v.contract) j["contract"] = *v.contract;
}

bool gitAvailable() {
    static std::once_flag once;
    static bool available = false;
    std::call_once(once, [] { available = support::GitRepo::isAvailable(); });
    return available;
}

std::string defaultCoreCacheDir() {
    auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return (v != nullptr) ? std::string(v) : std::string();
    };
    std::string base;
#if defined(_WIN32)
    base = env("LOCALAPPDATA");
    if (base.empty()) base = env("TEMP");
#elif defined(__APPLE__)
    const std::string home = support::homeDir();
    if (!home.empty()) base = home + "/Library/Caches";
#else
    base = env("XDG_CACHE_HOME");
    if (base.empty()) {
        const std::string home = support::homeDir();
        if (!home.empty()) base = home + "/.cache";
    }
#endif
    if (base.empty()) base = (fs::temp_directory_path() / "qstate-viewer-cache").string();
    return (fs::path(base) / "qstate-viewer" / "core").string();
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

void throwIfCancelled(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw rpc::Cancelled();
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

} // namespace

LoadedCore loadCoreLenient(const CoreRequest& request, const std::function<bool()>& cancelled) {
    LoadedCore out;
    out.request = request;
    if (request.coreDir.empty()) throw rpc::Error(Code::InvalidParams, "params.coreDir must not be empty");
    const std::string dir = support::normalizePath(request.coreDir);
    if (!support::isDirectory(dir)) {
        throw rpc::Error(Code::IoError, "core directory '" + dir + "' does not exist or is not a directory");
    }
    out.info.dir = dir;
    out.info.sourceDir = dir;

    const bool wantsRef = !request.coreRef.empty();
    std::string ref;
    std::unique_ptr<support::GitRepo> repo;
    if (wantsRef) {
        const bool autoRef = request.coreRef == "auto";
        const bool git = gitAvailable() && support::pathExists(dir + "/.git");
        if (git) {
            repo = std::make_unique<support::GitRepo>(dir);
            if (!repo->isRepo()) repo.reset();
        }
        if (!repo) {
            if (!autoRef) {
                throw rpc::Error(Code::InvalidParams,
                                 gitAvailable() ? "core directory '" + dir + "' is not a git repository: cannot read ref '" +
                                                      request.coreRef + "'"
                                                : "git is not available: cannot read ref '" + request.coreRef + "'");
            }
            out.diagnostics.push_back(makeDiag(Diagnostic::Severity::Warning,
                                               gitAvailable() ? "coreRef auto: the core directory is not a git repository; using the sources as they are on disk"
                                                              : "coreRef auto: git is not available; using the sources as they are on disk"));
        } else if (autoRef) {
            if (!request.epoch) {
                out.diagnostics.push_back(makeDiag(Diagnostic::Severity::Warning,
                                                   "coreRef auto: the epoch of the state files is unknown; using the working tree"));
            } else if (auto picked = repo->autoPickRef(*request.epoch)) {
                ref = *picked;
            } else {
                out.diagnostics.push_back(makeDiag(Diagnostic::Severity::Warning,
                                                   "coreRef auto: no tag with #define EPOCH " + std::to_string(*request.epoch) +
                                                       " found; using the working tree"));
            }
        } else {
            ref = request.coreRef;
        }
    }
    throwIfCancelled(cancelled);

    if (!ref.empty()) {
        const auto sha = repo->resolveCommit(ref);
        if (!sha) throw rpc::Error(Code::InvalidParams, "unknown git ref '" + ref + "' in '" + dir + "'");
        const std::string cacheDir = request.cacheDir.empty() ? defaultCoreCacheDir() : request.cacheDir;
        try {
            const support::GitExportResult exported = repo->exportTree(ref, cacheDir);
            out.info.sourceDir = exported.dir;
        } catch (const std::exception& e) {
            throw rpc::Error(Code::IoError, std::string("cannot export '") + ref + "': " + e.what());
        }
        out.info.ref = ref;
        out.info.sha = *sha;
        out.workingTree = false;
    } else {
        out.workingTree = true;
        if (!support::pathExists(dir + "/" + kRootFile)) {
            throw rpc::Error(Code::SchemaError,
                             std::string("'") + dir + "' is not a Qubic core repository: " + kRootFile + " not found");
        }
        if (repo || (gitAvailable() && support::pathExists(dir + "/.git"))) {
            support::GitRepo r(dir);
            if (r.isRepo()) {
                if (auto sha = r.resolveCommit("HEAD")) out.info.sha = *sha;
            }
        }
    }
    throwIfCancelled(cancelled);

    cpp::DiskSource disk{fs::path(out.info.sourceDir)};
    if (auto header = disk.read(kVersionHeader)) {
        const support::GitVersionInfo vi = support::parsePublicSettings(*header);
        if (vi.version) out.info.version = *vi.version;
        out.info.epoch = vi.epoch;
    }
    if (request.epoch && out.info.epoch && *request.epoch != *out.info.epoch) {
        out.diagnostics.push_back(makeDiag(
            Diagnostic::Severity::Warning,
            "the core sources are for epoch " + std::to_string(*out.info.epoch) + (out.info.version.empty() ? "" : " (version " + out.info.version + ")") +
                " but the state files are from epoch " + std::to_string(*request.epoch) +
                ": contract layouts may differ (use a core version with EPOCH " + std::to_string(*request.epoch) + ", e.g. coreRef \"auto\")"));
    }

    schema::ExtractOptions options;
    for (auto& d : parseDefines(request.defines)) options.defines.push_back(std::move(d));
    CancellableSource source(disk, cancelled);
    schema::ExtractResult extracted = schema::extractSchema(source, options);
    throwIfCancelled(cancelled);

    out.schema = extracted.schema;
    out.files = std::move(extracted.files);
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

LoadedCore loadCore(const CoreRequest& request, const std::function<bool()>& cancelled) {
    LoadedCore core = loadCoreLenient(request, cancelled);
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
