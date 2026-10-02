// The RPC methods of ui/src/rpc/contract.ts, implemented on top of the other modules (schema, decode, support).
// Method groups: app, settings, fs (fs.list), core (core.sync, core.commits), workspace, state (schema.types,
// state.*, table.*). See docs/SERVICE.md and native/service/README.md.
#pragma once

#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/support/git.h"
#include "qstate/support/watcher.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qstate::service {

// Repository the core sources come from unless the user chooses another one (AppInfo.defaultRepoUrl).
inline constexpr const char* kDefaultRepoUrl = "https://github.com/qubic/core";

struct ServiceState; // settings store, workspace manager, core loader (internal)

struct ServiceConfig {
    std::string appName = "qstate-viewer";
    // "" = the version of the CMake project.
    std::string version;
    // Size of the decode cache shared by all contracts of the open workspace.
    std::size_t decodeCacheBytes = std::size_t(256) << 20;
    // Settings file ("" = <config dir>/qstate-viewer/settings.json).
    std::string settingsPath;
    // Application cache directory ("" = support::defaultCacheDir()): git mirrors in <cacheDir>/repos, exported core
    // sources in <cacheDir>/core.
    std::string cacheDir;
    // The git executable and its short-command timeout.
    support::GitOptions git;
    // Polling / settle times of the file watcher (state files).
    support::WatcherOptions watcher;
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
