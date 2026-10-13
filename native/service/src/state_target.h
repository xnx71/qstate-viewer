// Internal: the `contract: StateTarget` parameter of the state.* / table.* methods and the decode -> rpc error mapping
// shared by the state and ledger method groups.
#pragma once

#include "qstate/decode/decoder.h"
#include "qstate/rpc/error.h"
#include "qstate/rpc/params.h"
#include "qstate/support/file_reader.h"
#include "workspace.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace qstate::service {

inline std::optional<schema::LedgerKind> parseLedgerKind(const std::string& s) {
    if (s == "spectrum") return schema::LedgerKind::Spectrum;
    if (s == "universe") return schema::LedgerKind::Universe;
    return std::nullopt;
}

// params.contract: a contract index (number) or "spectrum" / "universe"; anything else is invalid_params.
inline StateTarget requireTarget(const nlohmann::json& params) {
    if (!params.is_object()) throw rpc::Error(rpc::Code::InvalidParams, "params must be an object");
    auto it = params.find("contract");
    if (it == params.end() || it->is_null()) throw rpc::Error(rpc::Code::InvalidParams, "params.contract is required");
    if (it->is_string()) {
        if (auto k = parseLedgerKind(it->get<std::string>())) return StateTarget::ofLedger(*k);
        throw rpc::Error(rpc::Code::InvalidParams, "params.contract must be a contract index, \"spectrum\" or \"universe\"");
    }
    return StateTarget::ofContract(rpc::requireParam<std::uint32_t>(params, "contract"));
}

// Maps the exceptions of decode / support to rpc errors.
template <class F>
auto guardedDecode(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const rpc::Error&) {
        throw;
    } catch (const rpc::Cancelled&) {
        throw;
    } catch (const decode::NotFoundError& e) {
        throw rpc::Error(rpc::Code::NotFound, e.what());
    } catch (const decode::InvalidArgumentError& e) {
        throw rpc::Error(rpc::Code::InvalidParams, e.what());
    } catch (const decode::CancelledError&) {
        throw rpc::Error(rpc::Code::Internal, "cancelled");
    } catch (const support::FileError& e) {
        throw rpc::Error(rpc::Code::IoError, e.what());
    }
}

} // namespace qstate::service
