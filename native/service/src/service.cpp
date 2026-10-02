#include "qstate/service/service.h"

#include "module.h"
#include "workspace_manager.h"

#include <algorithm>

namespace qstate::service {

namespace {

using ModuleFn = void (*)(ModuleContext&);

// The method groups, registered in this order (later groups replace same-named methods of earlier ones).
// To add a group: define `void registerXxxMethods(ModuleContext&)` in src/xxx_methods.cpp, declare it in
// module.h and append it here. See native/service/README.md.
constexpr ModuleFn kModules[] = {
    registerAppMethods,
    registerSettingsMethods,
    registerFsMethods,
    registerWorkspaceMethods,
    registerStateMethods,
};

} // namespace

const std::vector<std::string>& Service::contractMethods() {
    // Keep in sync with `RpcMethods` in ui/src/rpc/contract.ts.
    static const std::vector<std::string> methods = {
        "app.info",
        "settings.get",
        "settings.update",
        "fs.list",
        "core.versions",
        "workspace.open",
        "workspace.get",
        "workspace.reload",
        "workspace.close",
        "schema.types",
        "state.node",
        "state.children",
        "state.bytes",
        "state.locate",
        "state.reveal",
        "state.search",
        "state.digest",
        "table.describe",
        "table.rows",
    };
    return methods;
}

Service::Service(ServiceConfig config) : config_(std::make_shared<const ServiceConfig>(std::move(config))) {
    state_ = std::make_shared<ServiceState>();
    state_->config = config_;
    state_->settings = std::make_shared<support::SettingsStore>(config_->settingsPath);
    state_->workspaces = std::make_shared<WorkspaceManager>(config_);
}

Service::~Service() {
    for (rpc::EventBus* bus : buses_) state_->workspaces->removeBus(bus);
}

void Service::registerAll(rpc::Dispatcher& dispatcher, rpc::EventBus& events) {
    state_->workspaces->addBus(&events);
    if (std::find(buses_.begin(), buses_.end(), &events) == buses_.end()) buses_.push_back(&events);
    ModuleContext ctx{dispatcher, events, config_, state_};
    for (ModuleFn module : kModules) {
        module(ctx);
    }
    // Every contract method not provided by a module answers "not implemented yet" (so the UI gets a proper
    // error instead of unknown_method). Modules never have to remove their stub.
    for (const std::string& method : contractMethods()) {
        if (!dispatcher.has(method)) {
            dispatcher.registerMethod(method, [method](const nlohmann::json&, rpc::CallContext&) -> nlohmann::json {
                throw rpc::Error(rpc::Code::NotFound, "not implemented yet: " + method);
            });
        }
    }
}

} // namespace qstate::service
