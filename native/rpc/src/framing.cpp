#include "qstate/rpc/framing.h"

namespace qstate::rpc {

Request parseRequest(const std::string& text) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        throw Error(Code::InvalidParams, "request is not valid JSON");
    }
    if (!j.is_object()) {
        throw Error(Code::InvalidParams, "request must be a JSON object");
    }
    auto method = j.find("method");
    if (method == j.end() || !method->is_string() || method->get_ref<const std::string&>().empty()) {
        throw Error(Code::InvalidParams, "request.method must be a non-empty string");
    }
    Request request;
    request.method = method->get<std::string>();
    auto params = j.find("params");
    if (params != j.end() && !params->is_null()) {
        request.params = std::move(*params);
    }
    return request;
}

nlohmann::json makeResult(nlohmann::json result) {
    nlohmann::json response = nlohmann::json::object();
    response["result"] = std::move(result);
    return response;
}

nlohmann::json makeError(Code code, const std::string& message, nlohmann::json data) {
    return makeError(Error(code, message, std::move(data)));
}

nlohmann::json makeError(const Error& error) {
    nlohmann::json response = nlohmann::json::object();
    response["error"] = error.toJson();
    return response;
}

std::string serialize(const nlohmann::json& response) {
    try {
        return response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception& e) { // out of memory, essentially
        return std::string(R"({"error":{"code":"internal","message":"cannot serialize response"}})");
    }
}

std::string handleRequest(const Dispatcher& dispatcher, const std::string& requestText) {
    try {
        Request request = parseRequest(requestText);
        return serialize(dispatcher.dispatch(request.method, request.params));
    } catch (const Error& e) {
        return serialize(makeError(e));
    } catch (const std::exception& e) {
        return serialize(makeError(Code::Internal, e.what()));
    } catch (...) {
        return serialize(makeError(Code::Internal, "unknown failure"));
    }
}

bool isError(const nlohmann::json& response) {
    return response.is_object() && response.contains("error");
}

} // namespace qstate::rpc
