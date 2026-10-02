#include "qstate/rpc/error.h"

namespace qstate::rpc {

std::string_view codeName(Code code) noexcept {
    switch (code) {
    case Code::InvalidParams: return "invalid_params";
    case Code::UnknownMethod: return "unknown_method";
    case Code::NotFound: return "not_found";
    case Code::NoWorkspace: return "no_workspace";
    case Code::IoError: return "io_error";
    case Code::SchemaError: return "schema_error";
    case Code::Internal: return "internal";
    }
    return "internal";
}

nlohmann::json Error::toJson() const {
    nlohmann::json j = {{"code", std::string(codeName(code_))}, {"message", what()}};
    if (!data_.is_null()) {
        j["data"] = data_;
    }
    return j;
}

} // namespace qstate::rpc
