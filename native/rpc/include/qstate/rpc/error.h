// RPC error type. The codes mirror `RpcErrorCode` of ui/src/rpc/contract.ts.
#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace qstate::rpc {

enum class Code {
    InvalidParams,
    UnknownMethod,
    NotFound,
    NoWorkspace,
    IoError,
    SchemaError,
    Internal,
};

// The wire name of a code ("invalid_params", "unknown_method", ...).
std::string_view codeName(Code code) noexcept;

// Thrown by method handlers; the dispatcher converts it into `{"error": {code, message, data?}}`.
// Any other exception thrown by a handler becomes `Code::Internal`.
class Error : public std::runtime_error {
public:
    Error(Code code, const std::string& message, nlohmann::json data = nullptr)
        : std::runtime_error(message), code_(code), data_(std::move(data)) {}

    Code code() const noexcept { return code_; }
    // null when there is no extra data.
    const nlohmann::json& data() const noexcept { return data_; }

    // {"code": "...", "message": "...", "data": ...?}
    nlohmann::json toJson() const;

private:
    Code code_;
    nlohmann::json data_;
};

} // namespace qstate::rpc
