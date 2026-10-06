// qstate-memory-bridge: the REAL native service (all RPC methods of ui/src/rpc/contract.ts) behind a line protocol on
// stdin / stdout. A measurement tool, not part of the product (CMake option QSTATE_BUILD_MEMORY_BRIDGE, OFF by default):
// ui/scripts/memory.mjs --backend=bridge attaches it to headless Chrome so that the page sees real payloads and the
// renderer's heap can be profiled with CDP (the webview itself offers no heap API). See docs/MEMORY.md.
//
//   request  (one line):  {"id": <n>, "method": "<rpc method>", "params": {...}}
//   response (one line):  R <id> <status> <json>          status 0 = result, 1 = RpcError (the bridge contract of docs/HOST.md)
//   event    (one line):  E <name> <json>
//   method "__stats":     {"rssKb": .., "hwmKb": ..}      resident / peak resident size of this process (Linux)
//   method "__trim":      Service::trimMemory(), then the same numbers
//
// The environment variables of the app apply (QSTATE_CONFIG_DIR, QSTATE_CACHE_DIR); QSTATE_DECODE_CACHE_MB overrides the
// decode cache size and QSTATE_NO_TUNE=1 skips the allocator tuning of the host (to measure what it buys).
#include "qstate/gui/bridge.h"
#include "qstate/gui/memory.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/service/service.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace {

std::mutex outMutex;

void emitLine(const std::string& line) {
    std::lock_guard lock(outMutex);
    std::cout << line << '\n' << std::flush;
}

std::string procStatus() {
    std::ifstream f("/proc/self/status");
    std::string line;
    long rss = 0;
    long hwm = 0;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) rss = std::atol(line.c_str() + 6);
        if (line.rfind("VmHWM:", 0) == 0) hwm = std::atol(line.c_str() + 6);
    }
    return "{\"rssKb\":" + std::to_string(rss) + ",\"hwmKb\":" + std::to_string(hwm) + "}";
}

} // namespace

int main() {
    using namespace qstate;
    const char* noTune = std::getenv("QSTATE_NO_TUNE");
    if (noTune == nullptr || *noTune == '\0' || *noTune == '0') gui::tuneAllocator();
    rpc::Dispatcher dispatcher(4);
    rpc::EventBus events;
    service::ServiceConfig config;
    if (const char* dir = std::getenv("QSTATE_CONFIG_DIR")) config.settingsPath = std::string(dir) + "/settings.json";
    if (const char* dir = std::getenv("QSTATE_CACHE_DIR")) config.cacheDir = dir;
    if (const char* mb = std::getenv("QSTATE_DECODE_CACHE_MB")) config.decodeCacheBytes = static_cast<std::size_t>(std::atol(mb)) << 20;
    service::Service service(config);
    service.registerAll(dispatcher, events);
    auto subscription = events.subscribeScoped([](const std::string& name, const nlohmann::json& payload) {
        emitLine("E " + name + " " + payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        auto request = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (request.is_discarded() || !request.is_object() || !request.contains("id") || !request.contains("method")) continue;
        const long id = request["id"].get<long>();
        const std::string method = request["method"].get<std::string>();
        if (method == "__stats" || method == "__trim") {
            if (method == "__trim") service.trimMemory(true);
            emitLine("R " + std::to_string(id) + " 0 " + procStatus());
            continue;
        }
        dispatcher.dispatchAsync(method, request.value("params", nlohmann::json::object()), [id](nlohmann::json response) {
            gui::BridgeReply reply = gui::makeReply(response);
            emitLine("R " + std::to_string(id) + " " + std::to_string(reply.status) + " " + reply.json);
        });
    }
    dispatcher.shutdown();
    return 0;
}
