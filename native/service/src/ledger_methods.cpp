// Method group "ledger": ledger.summary, ledger.lookup (spectrum.EEE / universe.EEE of the shown epoch).
#include "module.h"
#include "qstate/decode/decoder.h"
#include "qstate/decode/json.h"
#include "qstate/rpc/params.h"
#include "state_target.h"
#include "workspace_manager.h"

namespace qstate::service {

namespace {

using nlohmann::json;
using rpc::Code;

struct LedgerContext {
    std::shared_ptr<Workspace> ws;
    Workspace::View view;
    decode::Query query;
};

LedgerContext ledgerContext(const WorkspaceManager& manager, const json& params, rpc::CallContext& call) {
    LedgerContext c;
    c.ws = manager.require();
    const std::string kind = rpc::requireParam<std::string>(params, "kind");
    const std::optional<schema::LedgerKind> k = parseLedgerKind(kind);
    if (!k) throw rpc::Error(Code::InvalidParams, "params.kind must be \"spectrum\" or \"universe\"");
    c.view = c.ws->view(StateTarget::ofLedger(*k));
    c.query.generation = c.view.generation;
    c.query.cancel = call.cancelFlag();
    return c;
}

} // namespace

void registerLedgerMethods(ModuleContext& ctx) {
    auto manager = ctx.state->workspaces;

    ctx.add("ledger.summary", [manager](const json& params, rpc::CallContext& call) -> json {
        LedgerContext c = ledgerContext(*manager, params, call);
        return guardedDecode([&]() -> json { return c.view.decoder->ledgerSummary(c.query); });
    });

    ctx.add("ledger.lookup", [manager](const json& params, rpc::CallContext& call) -> json {
        LedgerContext c = ledgerContext(*manager, params, call);
        const std::string identity = rpc::requireParam<std::string>(params, "identity");
        return guardedDecode([&]() -> json { return c.view.decoder->ledgerLookup(identity, c.query); });
    });
}

} // namespace qstate::service
