#include "qstate/gui/bridge.h"

namespace qstate::gui {

bool parseInvokeArgs(const std::string& args, InvokeCall& call, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(args, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_array() || j.empty() || !j[0].is_string() || j[0].get_ref<const std::string&>().empty()) {
        error = "__qstate_invoke expects (method: string, params?: object)";
        return false;
    }
    if (j.size() > 2) {
        error = "__qstate_invoke takes at most two arguments";
        return false;
    }
    call.method = j[0].get<std::string>();
    call.params = (j.size() == 2 && !j[1].is_null()) ? std::move(j[1]) : nlohmann::json::object();
    return true;
}

BridgeReply makeReply(const nlohmann::json& response) {
    auto dump = [](const nlohmann::json& value) {
        return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    };
    if (response.is_object()) {
        auto error = response.find("error");
        if (error != response.end()) {
            return {1, dump(*error)};
        }
        auto result = response.find("result");
        if (result != response.end()) {
            return {0, dump(*result)};
        }
    }
    return {1, R"({"code":"internal","message":"malformed response"})"};
}

std::string jsStringLiteral(const std::string& text) {
    std::string literal = nlohmann::json(text).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    // JSON allows U+2028 / U+2029 unescaped; JavaScript before ES2019 does not. Escape them for any engine.
    std::string out;
    out.reserve(literal.size());
    for (std::size_t i = 0; i < literal.size(); ++i) {
        if (static_cast<unsigned char>(literal[i]) == 0xE2 && i + 2 < literal.size() &&
            static_cast<unsigned char>(literal[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(literal[i + 2]) == 0xA8 || static_cast<unsigned char>(literal[i + 2]) == 0xA9)) {
            out += static_cast<unsigned char>(literal[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out += literal[i];
        }
    }
    return out;
}

std::string makeEmitScript(const std::vector<QueuedEvent>& events) {
    std::string script = "(function(){var f=window.";
    script += kEmitFunction;
    script += ";if(typeof f!=='function')return;var e=[";
    bool first = true;
    for (const auto& event : events) {
        if (!first) {
            script += ',';
        }
        first = false;
        script += '[';
        script += jsStringLiteral(event.name);
        script += ',';
        script += jsStringLiteral(event.payloadJson);
        script += ']';
    }
    script += "];for(var i=0;i<e.length;i++){try{f(e[i][0],JSON.parse(e[i][1]));}catch(x){console.error(x);}}})();";
    return script;
}

} // namespace qstate::gui
