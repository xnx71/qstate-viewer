#include "session.h"

namespace qstate::cli {

using nlohmann::json;

const std::vector<std::string> kCommonValueOptions = {"core",  "ref",   "state", "epoch", "define", "contract", "node",
                                                      "limit", "offset", "sort",  "filter", "view",  "query",    "mode",
                                                      "port",  "ui-dir", "depth", "workers"};
const std::vector<std::string> kCommonFlags = {"json", "desc", "raw", "hide-empty", "help"};

namespace {

service::ServiceConfig makeConfig(bool watch, service::StartupRequest startup) {
    service::ServiceConfig config;
    config.appName = "qstate-cli";
    config.transport = "http";
    config.watchFiles = watch;
    config.recordRecentWorkspaces = watch; // only `serve` behaves like the app
    config.startup = std::move(startup);
    return config;
}

} // namespace

Session::Session(bool watch, service::StartupRequest startup) : service_(makeConfig(watch, std::move(startup))) {
    service_.registerAll(dispatcher_, events_);
}

json Session::call(const std::string& method, const json& params) {
    json response = dispatcher_.dispatch(method, params, "http");
    if (response.contains("error")) {
        throw RpcFailure(response["error"].value("code", "internal"), response["error"].value("message", std::string("failed")));
    }
    return response["result"];
}

json workspaceRequest(const Args& args) {
    json req = {{"coreDir", args.require("core")}, {"stateDir", args.require("state")}};
    if (auto ref = args.get("ref")) req["coreRef"] = *ref;
    if (auto epoch = args.integer("epoch")) req["epoch"] = *epoch;
    auto defines = args.all("define");
    if (!defines.empty()) req["defines"] = defines;
    return req;
}

json Session::openWorkspace(const Args& args) {
    return call("workspace.open", workspaceRequest(args));
}

service::StartupRequest startupFrom(const Args& args) {
    service::StartupRequest s;
    s.coreDir = args.get("core");
    s.coreRef = args.get("ref");
    s.stateDir = args.get("state");
    s.epoch = args.integer("epoch");
    return s;
}

} // namespace qstate::cli
