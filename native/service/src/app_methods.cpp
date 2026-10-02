// Method group "app": app.info
#include "module.h"
#include "qstate/support/dir_scan.h"

#ifndef QSTATE_VERSION_STRING
#define QSTATE_VERSION_STRING "0.0.0"
#endif

namespace qstate::service {

nlohmann::json makeAppInfo(const ServiceConfig& config, bool gitAvailable) {
    return {
        {"name", config.appName},
        {"version", config.version.empty() ? std::string(QSTATE_VERSION_STRING) : config.version},
        {"platform", support::platformName()},
        {"transport", "webview"},
        {"homeDir", support::homeDir()},
        {"cwd", support::currentDir()},
        {"pathSeparator", std::string(1, support::pathSeparator())},
        {"gitAvailable", gitAvailable},
        {"defaultRepoUrl", kDefaultRepoUrl},
    };
}

void registerAppMethods(ModuleContext& ctx) {
    auto config = ctx.config;
    auto core = ctx.state->core;
    ctx.add("app.info", [config, core](const nlohmann::json&, rpc::CallContext&) {
        return makeAppInfo(*config, core->gitAvailable());
    });
}

} // namespace qstate::service
