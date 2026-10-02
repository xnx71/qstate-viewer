// JSON conversions producing / consuming exactly the shapes of ui/src/rpc/contract.ts.
// Usable through nlohmann ADL: `nlohmann::json j = nodeInfo;`, `req = j.get<qstate::decode::TableRequest>();`.
#pragma once

#include <nlohmann/json.hpp>

#include "qstate/decode/values.h"

namespace qstate::decode {

void to_json(nlohmann::json& j, const LeafValue& v);
void to_json(nlohmann::json& j, const ContainerStats& v);
void to_json(nlohmann::json& j, const NodeInfo& v);
void to_json(nlohmann::json& j, const ChildrenPage& v);
void to_json(nlohmann::json& j, const NodeLocation& v);
void to_json(nlohmann::json& j, const NodeReveal& v);
void to_json(nlohmann::json& j, const SearchResult& v);
void to_json(nlohmann::json& j, const TableColumn& v);
void to_json(nlohmann::json& j, const TableInfo& v);
void to_json(nlohmann::json& j, const TableRow& v);
void to_json(nlohmann::json& j, const TablePage& v);

// Params: { column, op, value? } / { column, desc? } / TableQuery (contract field of TableQuery is ignored here).
void from_json(const nlohmann::json& j, FilterSpec& v);
void from_json(const nlohmann::json& j, SortSpec& v);
void from_json(const nlohmann::json& j, TableRequest& v);

} // namespace qstate::decode
