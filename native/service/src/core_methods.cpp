// Method group "core": core.sync, core.commits (the git mirrors of core repositories)
#include "module.h"
#include "qstate/rpc/params.h"

namespace qstate::service {

namespace {

using nlohmann::json;
using rpc::Code;

// The call's cancellation and the `core.progress` events of this call.
CoreCall makeCall(rpc::CallContext& call, rpc::EventBus* events) {
    CoreCall c;
    c.cancelled = [&call] { return call.cancelled(); };
    c.progress = [events](const std::string& phase, const std::string& message, std::optional<int> percent) {
        json payload = {{"phase", phase}, {"message", message}};
        if (percent) payload["percent"] = *percent;
        events->emit("core.progress", payload);
    };
    return c;
}

} // namespace

void registerCoreMethods(ModuleContext& ctx) {
    auto core = ctx.state->core;
    rpc::EventBus* events = &ctx.events;

    ctx.add("core.sync", [core, events](const json& params, rpc::CallContext& call) -> json {
        const std::string repoUrl = rpc::requireParam<std::string>(params, "repoUrl");
        const bool offline = rpc::optionalParam<bool>(params, "offline").value_or(false);
        return core->sync(repoUrl, offline, makeCall(call, events));
    });
    ctx.add("core.commits", [core, events](const json& params, rpc::CallContext& call) -> json {
        const std::string repoUrl = rpc::requireParam<std::string>(params, "repoUrl");
        const std::string ref = rpc::requireParam<std::string>(params, "ref");
        const std::int64_t limit = rpc::optionalParam<std::int64_t>(params, "limit").value_or(50);
        const std::int64_t skip = rpc::optionalParam<std::int64_t>(params, "skip").value_or(0);
        const std::string search = rpc::optionalParam<std::string>(params, "search").value_or("");
        if (limit < 1 || limit > 1000) throw rpc::Error(Code::InvalidParams, "params.limit must be between 1 and 1000");
        if (skip < 0) throw rpc::Error(Code::InvalidParams, "params.skip must not be negative");
        return core->commits(repoUrl, ref, static_cast<std::size_t>(limit), static_cast<std::size_t>(skip), search, makeCall(call, events));
    });
}

} // namespace qstate::service
