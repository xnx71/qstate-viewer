// Internal: an in-process service (dispatcher + service, no transport) used by the one-shot commands.
#pragma once

#include "args.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/service/service.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace qstate::cli {

struct RpcFailure : std::runtime_error {
    RpcFailure(std::string errorCode, const std::string& message) : std::runtime_error(message), code(std::move(errorCode)) {}
    std::string code;
};

// Options shared by the commands that open a workspace.
extern const std::vector<std::string> kCommonValueOptions;
extern const std::vector<std::string> kCommonFlags;

class Session {
public:
    // `watch` = keep watching files (serve); one-shot commands do not.
    explicit Session(bool watch = false, service::StartupRequest startup = {});

    // Result of the call; throws RpcFailure for an error response.
    nlohmann::json call(const std::string& method, const nlohmann::json& params = nlohmann::json::object());

    // workspace.open from --core / --ref / --state / --epoch / --define.
    nlohmann::json openWorkspace(const Args& args);

    rpc::Dispatcher& dispatcher() { return dispatcher_; }
    rpc::EventBus& events() { return events_; }

private:
    rpc::Dispatcher dispatcher_;
    rpc::EventBus events_;
    service::Service service_;
};

// workspace.open params from the common options (UsageError when --core / --state are missing).
nlohmann::json workspaceRequest(const Args& args);
service::StartupRequest startupFrom(const Args& args);

} // namespace qstate::cli
