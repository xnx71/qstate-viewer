// Method group "fs": fs.list
#include "module.h"
#include "qstate/rpc/params.h"
#include "qstate/support/dir_scan.h"

namespace qstate::service {

namespace {

using nlohmann::json;
using rpc::Code;

json entryJson(const support::FsEntry& e) {
    json j = {{"name", e.name}, {"path", e.path}, {"kind", e.isDir ? "dir" : "file"}};
    if (e.size) j["size"] = *e.size;
    if (e.mtimeMs) j["mtimeMs"] = *e.mtimeMs;
    if (e.state) j["state"] = {{"index", e.state->first}, {"epoch", e.state->second}};
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

} // namespace

void registerFsMethods(ModuleContext& ctx) {
    ctx.add("fs.list", [](const json& params, rpc::CallContext&) { return fsList(params); });
}

} // namespace qstate::service
