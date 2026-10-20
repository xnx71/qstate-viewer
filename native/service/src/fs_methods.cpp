// Method group "fs": fs.list, fs.write
#include "module.h"
#include "qstate/rpc/params.h"
#include "qstate/support/dir_scan.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace qstate::service {

namespace {

using nlohmann::json;
using rpc::Code;

json entryJson(const support::FsEntry& e) {
    json j = {{"name", e.name}, {"path", e.path}, {"kind", e.isDir ? "dir" : "file"}};
    if (e.size) j["size"] = *e.size;
    if (e.mtimeMs) j["mtimeMs"] = *e.mtimeMs;
    if (e.state) j["state"] = {{"index", e.state->first}, {"epoch", e.state->second}};
    if (e.ledger) {
        j["ledger"] = {{"kind", e.ledger->first == support::FileNameKind::Spectrum ? "spectrum" : "universe"},
                       {"epoch", e.ledger->second}};
    }
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
        {"hints", {{"stateEpochs", r.listing.hints.stateEpochs}}},
    };
}

// Exports are written in chunks to "<path>.part"; `finish` renames it to the path (replacing a file of that name), so
// an interrupted or cancelled export never leaves a truncated file under the chosen name. `abort` removes the part file.
json fsWrite(const json& params) {
    namespace fs = std::filesystem;
    const std::string path = rpc::requireParam<std::string>(params, "path");
    const std::string data = rpc::optionalParam<std::string>(params, "data").value_or("");
    const bool append = rpc::optionalParam<bool>(params, "append").value_or(false);
    const bool finish = rpc::optionalParam<bool>(params, "finish").value_or(false);
    const bool abort = rpc::optionalParam<bool>(params, "abort").value_or(false);
    const fs::path target(path);
    if (!target.is_absolute() || !target.has_filename()) throw rpc::Error(Code::InvalidParams, "'" + path + "' is not an absolute file path");
    const fs::path part(path + ".part");
    std::error_code ec;
    if (abort) {
        fs::remove(part, ec);
        return {{"path", path}, {"bytes", 0}};
    }
    if (fs::is_directory(target, ec)) throw rpc::Error(Code::IoError, "'" + path + "' is a directory");
    {
        std::ofstream out(part, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        if (!out) throw rpc::Error(Code::IoError, "cannot write '" + part.string() + "'");
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) throw rpc::Error(Code::IoError, "cannot write '" + part.string() + "' (disk full?)");
    }
    const auto bytes = fs::file_size(part, ec);
    if (finish) {
        fs::rename(part, target, ec);
        if (ec) throw rpc::Error(Code::IoError, "cannot rename '" + part.string() + "' to '" + path + "': " + ec.message());
    }
    return {{"path", path}, {"bytes", ec ? 0 : bytes}};
}

} // namespace

void registerFsMethods(ModuleContext& ctx) {
    ctx.add("fs.list", [](const json& params, rpc::CallContext&) { return fsList(params); });
    ctx.add("fs.write", [](const json& params, rpc::CallContext&) { return fsWrite(params); });
}

} // namespace qstate::service
