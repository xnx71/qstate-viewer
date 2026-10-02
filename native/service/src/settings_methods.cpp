// Method group "settings": settings.get, settings.update
#include "module.h"
#include "qstate/rpc/params.h"

namespace qstate::service {

void registerSettingsMethods(ModuleContext& ctx) {
    auto store = ctx.state->settings;
    ctx.add("settings.get", [store](const nlohmann::json&, rpc::CallContext&) -> nlohmann::json { return store->get(); });
    ctx.add("settings.update", [store](const nlohmann::json& params, rpc::CallContext&) -> nlohmann::json {
        nlohmann::json patch = rpc::requireParam<nlohmann::json>(params, "patch");
        if (!patch.is_object()) throw rpc::Error(rpc::Code::InvalidParams, "params.patch must be an object");
        if (auto it = patch.find("theme"); it != patch.end() && !it->is_string()) {
            throw rpc::Error(rpc::Code::InvalidParams, "params.patch.theme must be a string");
        }
        if (auto it = patch.find("ui"); it != patch.end() && !it->is_object()) {
            throw rpc::Error(rpc::Code::InvalidParams, "params.patch.ui must be an object");
        }
        return store->applyPatch(patch);
    });
}

} // namespace qstate::service
