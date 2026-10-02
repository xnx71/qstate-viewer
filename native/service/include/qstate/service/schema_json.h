// Internal: schema::Type -> contract.ts TypeInfo.
#pragma once

#include "qstate/schema/model.h"

#include <nlohmann/json.hpp>

namespace qstate::service {

nlohmann::json typeInfoJson(const schema::Type& type);

} // namespace qstate::service
