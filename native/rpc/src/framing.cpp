#include "qstate/rpc/framing.h"

namespace qstate::rpc {

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

} // namespace qstate::rpc
