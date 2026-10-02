// Response objects of the bridge: {"result": ...} | {"error": {"code": "...", "message": "...", "data": ...?}}
#pragma once

#include "qstate/rpc/error.h"

#include <nlohmann/json.hpp>

#include <string>

namespace qstate::rpc {

nlohmann::json makeResult(nlohmann::json result);
nlohmann::json makeError(Code code, const std::string& message, nlohmann::json data = nullptr);
nlohmann::json makeError(const Error& error);

} // namespace qstate::rpc
