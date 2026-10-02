// Method group "workspace": workspace.open / get / reload / close
#include "module.h"
#include "qstate/rpc/params.h"
#include "workspace_manager.h"

namespace qstate::service {

namespace {

using nlohmann::json;

support::WorkspaceRequest parseRequest(const json& params) {
    if (!params.is_object()) throw rpc::Error(rpc::Code::InvalidParams, "params must be an object");
    support::WorkspaceRequest r;
    const json core = rpc::requireParam<json>(params, "core");
    if (!core.is_object()) throw rpc::Error(rpc::Code::InvalidParams, "params.core must be an object");
    r.core.repoUrl = rpc::requireParam<std::string>(core, "repoUrl");
    r.core.ref = rpc::requireParam<std::string>(core, "ref");
    r.statePath = rpc::requireParam<std::string>(params, "statePath");
    r.epoch = rpc::optionalParam<int>(params, "epoch");
    if (r.epoch && *r.epoch < 0) throw rpc::Error(rpc::Code::InvalidParams, "params.epoch must not be negative");
    if (auto it = params.find("defines"); it != params.end() && !it->is_null()) {
        if (!it->is_array()) throw rpc::Error(rpc::Code::InvalidParams, "params.defines must be an array of strings");
        for (const json& d : *it) {
            if (!d.is_string()) throw rpc::Error(rpc::Code::InvalidParams, "params.defines must be an array of strings");
            r.defines.push_back(d.get<std::string>());
        }
    }
    return r;
}

} // namespace

void registerWorkspaceMethods(ModuleContext& ctx) {
    auto manager = ctx.state->workspaces;
    auto settings = ctx.state->settings;

    ctx.add("workspace.open", [manager, settings](const json& params, rpc::CallContext& call) -> json {
        support::WorkspaceRequest request = parseRequest(params);
        auto ws = manager->open(request, call.cancelFlag());
        settings->addRecentWorkspace(ws->request());
        return ws->toJson();
    });
    ctx.add("workspace.get", [manager](const json&, rpc::CallContext&) -> json {
        auto ws = manager->current();
        return ws ? ws->toJson() : json(nullptr);
    });
    ctx.add("workspace.reload", [manager](const json&, rpc::CallContext& call) -> json {
        auto ws = manager->reload(call.cancelFlag());
        return ws->toJson();
    });
    ctx.add("workspace.close", [manager](const json&, rpc::CallContext&) -> json {
        manager->close();
        return nullptr;
    });
}

} // namespace qstate::service
