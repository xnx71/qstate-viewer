// Internal: how a group of RPC methods plugs into Service::registerAll (see native/service/README.md).
#pragma once

#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/service/core_loader.h"
#include "qstate/service/service.h"
#include "qstate/support/settings.h"

#include <memory>
#include <string>

namespace qstate::service {

class WorkspaceManager;

// State shared by all method groups (and by every dispatcher the Service is registered on).
struct ServiceState {
    std::shared_ptr<const ServiceConfig> config;
    std::shared_ptr<support::SettingsStore> settings;
    std::shared_ptr<CoreLoader> core;
    std::shared_ptr<WorkspaceManager> workspaces;
};

// Everything a method group needs. Handlers must capture copies of the shared_ptr members (never `this` of
// the Service or a reference to the context, which lives only during registration).
struct ModuleContext {
    rpc::Dispatcher& dispatcher;
    rpc::EventBus& events;
    std::shared_ptr<const ServiceConfig> config;
    std::shared_ptr<ServiceState> state;

    void add(const std::string& method, rpc::Dispatcher::Handler handler) {
        dispatcher.registerMethod(method, std::move(handler));
    }
};

nlohmann::json makeAppInfo(const ServiceConfig& config, bool gitAvailable);

// One function per method group, each defined in its own .cpp file (src/<group>_methods.cpp).
// Declare it here and append it to kModules in service.cpp.
void registerAppMethods(ModuleContext& ctx);
void registerSettingsMethods(ModuleContext& ctx);
void registerFsMethods(ModuleContext& ctx);        // fs.list
void registerCoreMethods(ModuleContext& ctx);      // core.sync, core.commits
void registerWorkspaceMethods(ModuleContext& ctx); // workspace.*
void registerStateMethods(ModuleContext& ctx);     // schema.types, state.*, table.*

} // namespace qstate::service
