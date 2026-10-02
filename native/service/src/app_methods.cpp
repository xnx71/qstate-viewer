// Method group "app": app.info
#include "module.h"
#include "qstate/service/core_loader.h"
#include "qstate/service/service.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifndef QSTATE_VERSION_STRING
#define QSTATE_VERSION_STRING "0.0.0"
#endif

namespace qstate::service {

namespace {

const char* platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string homeDir() {
    for (const char* name : {"HOME", "USERPROFILE"}) {
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
            return value;
        }
    }
    return {};
}

std::string currentDir() {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    return ec ? std::string() : path.string();
}

} // namespace

nlohmann::json makeAppInfo(const ServiceConfig& config, const std::string& transport) {
    nlohmann::json startup = nlohmann::json::object();
    if (config.startup.coreDir) {
        startup["coreDir"] = *config.startup.coreDir;
    }
    if (config.startup.coreRef) {
        startup["coreRef"] = *config.startup.coreRef;
    }
    if (config.startup.stateDir) {
        startup["stateDir"] = *config.startup.stateDir;
    }
    if (config.startup.epoch) {
        startup["epoch"] = *config.startup.epoch;
    }
    return {
        {"name", config.appName},
        {"version", config.version.empty() ? std::string(QSTATE_VERSION_STRING) : config.version},
        {"platform", platformName()},
        {"transport", transport.empty() ? config.transport : transport},
        {"homeDir", homeDir()},
        {"cwd", currentDir()},
        {"pathSeparator", std::string(1, std::filesystem::path::preferred_separator)},
        {"gitAvailable", gitAvailable()},
        {"startup", startup},
    };
}

nlohmann::json Service::appInfo() const {
    return makeAppInfo(*config_, std::string());
}

void registerAppMethods(ModuleContext& ctx) {
    auto config = ctx.config;
    // The transport that delivered the call wins over the configured default (the desktop app can serve the
    // webview bridge and HTTP at the same time).
    ctx.add("app.info", [config](const nlohmann::json&, rpc::CallContext& call) {
        return makeAppInfo(*config, call.transport());
    });
}

} // namespace qstate::service
