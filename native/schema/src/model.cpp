#include "qstate/schema/model.h"

namespace qstate::schema {

const char* ledgerKindName(LedgerKind kind) {
    return kind == LedgerKind::Spectrum ? "spectrum" : "universe";
}

} // namespace qstate::schema
