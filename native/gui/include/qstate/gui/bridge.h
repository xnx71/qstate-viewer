// Pure helpers of the webview bridge (no webview dependency, so they are unit-testable).
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace qstate::gui {

// Name of the JS function bound by the host: window.__qstate_invoke(method, params) -> Promise.
inline constexpr const char* kInvokeBinding = "__qstate_invoke";
// Name of the JS function the UI defines: window.__qstate_emit(name, payload).
inline constexpr const char* kEmitFunction = "__qstate_emit";

struct InvokeCall {
    std::string method;
    nlohmann::json params;
};

// `args` is the JSON array text the webview passes to a bound function: ["<method>", {params}].
// Returns false (with `error` set) for anything else.
bool parseInvokeArgs(const std::string& args, InvokeCall& call, std::string& error);

struct BridgeReply {
    int status; // 0 = resolve, 1 = reject (the webview contract)
    std::string json;
};

// {"result": r} -> resolve(r) ; {"error": e} -> reject(e)  (JSON text, exactly the shapes of contract.ts).
BridgeReply makeReply(const nlohmann::json& response);

// A JSON string literal (also a valid JavaScript string literal, U+2028 / U+2029 escaped).
std::string jsStringLiteral(const std::string& text);

struct QueuedEvent {
    std::string name;
    std::string payloadJson; // serialized payload
};

// JavaScript that delivers the events to window.__qstate_emit (no-op while the UI has not defined it).
// Payloads travel as JSON.parse("<json string literal>"), which is injection-proof by construction.
std::string makeEmitScript(const std::vector<QueuedEvent>& events);

} // namespace qstate::gui
