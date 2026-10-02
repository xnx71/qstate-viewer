// JSON request / response framing shared by the transports.
//
//   request : {"method": "<name>", "params": {...}}      (params optional, default {})
//   response: {"result": ...} | {"error": {"code": "...", "message": "...", "data": ...?}}
#pragma once

#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/error.h"

#include <nlohmann/json.hpp>

#include <string>

namespace qstate::rpc {

struct Request {
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

// Throws rpc::Error(InvalidParams) for malformed JSON or a malformed request object.
Request parseRequest(const std::string& text);

nlohmann::json makeResult(nlohmann::json result);
nlohmann::json makeError(Code code, const std::string& message, nlohmann::json data = nullptr);
nlohmann::json makeError(const Error& error);

// JSON text of a response. Never throws: invalid UTF-8 in strings is replaced by U+FFFD.
std::string serialize(const nlohmann::json& response);

// parse + dispatch + serialize, all on the calling thread. Never throws.
std::string handleRequest(const Dispatcher& dispatcher, const std::string& requestText);

// True when the response object carries an error.
bool isError(const nlohmann::json& response);

} // namespace qstate::rpc
