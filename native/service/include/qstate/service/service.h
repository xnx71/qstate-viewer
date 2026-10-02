// The RPC methods of ui/src/rpc/contract.ts, implemented on top of the other modules (schema, decode, support).
// Method groups: app, settings, fs (fs.list, core.versions), workspace, state (schema.types, state.*, table.*).
// See docs/SERVICE.md and native/service/README.md.
#pragma once

#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/support/watcher.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qstate::service {

// Subset of WorkspaceRequest that can be given on the command line (AppInfo.startup).
struct StartupRequest {
    std::optional<std::string> coreDir;
    std::optional<std::string> coreRef;
    std::optional<std::string> stateDir;
    std::optional<std::int64_t> epoch;
};

struct ServiceState; // settings store, workspace manager, decode cache (internal)

struct ServiceConfig {
    std::string appName = "qstate-viewer";
    // "" = the version of the CMake project.
    std::string version;
    // AppInfo.transport: "webview" | "http" | "mock" (supplied by the host).
    std::string transport = "http";
    StartupRequest startup;
    // Size of the decode cache shared by all contracts of the open workspace.
    std::size_t decodeCacheBytes = std::size_t(256) << 20;
    // Settings file ("" = <config dir>/qstate-viewer/settings.json).
    std::string settingsPath;
    // Where git exports of core tags are kept ("" = <cache dir>/qstate-viewer/core).
    std::string cacheDir;
    // Polling / settle times of the file watcher (state files and core sources).
    support::WatcherOptions watcher;
    // false: the workspace is not watched (one-shot tools like qstate-cli): no watcher threads, no events.
    bool watchFiles = true;
    // false: workspace.open does not touch the settings file (recentWorkspaces).
    bool recordRecentWorkspaces = true;
    // Quiet time after the last change of a watched core source file before the schema is re-extracted.
    std::chrono::milliseconds sourceDebounce{500};
};

// Thread safety: registerAll() must be called before the dispatcher is used. The registered handlers do not
// reference the Service object (they share its state: configuration, settings, the current workspace), so the
// Service may be destroyed afterwards (events stop at that point, see ~Service). Handlers may run concurrently from several threads. registerAll() may be
// called for several dispatcher / bus pairs: they share one workspace and every bus receives the events.
class Service {
public:
    explicit Service(ServiceConfig config = {});
    // Stops delivering events to the buses registered through registerAll (the handlers themselves keep working
    // and keep sharing the workspace). Destroy the Service before the EventBus it was registered with, or keep it
    // alive as long as the bus.
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    const ServiceConfig& config() const noexcept { return *config_; }

    // Registers every method of contract.ts on `dispatcher`: the implemented ones, and a "not implemented yet"
    // stub for the rest. `events` is where services publish "workspace.updated" / "contracts.changed".
    // The dispatcher and the bus must outlive the registered handlers.
    void registerAll(rpc::Dispatcher& dispatcher, rpc::EventBus& events);

    // The value of the `app.info` result.
    nlohmann::json appInfo() const;

    // Names of all methods of contract.ts (RpcMethods), in contract order.
    static const std::vector<std::string>& contractMethods();

private:
    std::shared_ptr<const ServiceConfig> config_;
    std::shared_ptr<ServiceState> state_;
    std::vector<rpc::EventBus*> buses_;
};

} // namespace qstate::service
