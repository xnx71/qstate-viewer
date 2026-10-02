#include "qstate/decode/json.h"

namespace qstate::decode {

using nlohmann::json;

void to_json(json& j, const LeafValue& v) {
    using K = LeafValue::Kind;
    j = json::object();
    j["k"] = kindName(v.k);
    switch (v.k) {
    case K::Int:
        j["v"] = v.v;
        j["unsigned"] = v.isUnsigned;
        j["bits"] = v.bits;
        j["hex"] = v.hex;
        if (v.text) j["text"] = *v.text;
        break;
    case K::Bool:
        j["v"] = v.boolean;
        j["raw"] = v.raw;
        break;
    case K::Char:
        j["v"] = v.v;
        j["code"] = v.raw;
        break;
    case K::Enum:
        j["v"] = v.v;
        if (v.name) j["name"] = *v.name;
        break;
    case K::Id:
        j["identity"] = v.identity;
        j["hex"] = v.hex;
        j["zero"] = v.zero;
        if (v.contractIndex) j["contract"] = {{"index", *v.contractIndex}, {"name", v.contractName}};
        if (v.text) j["text"] = *v.text;
        break;
    case K::U128:
        j["v"] = v.v;
        j["hex"] = v.hex;
        break;
    case K::Float: j["v"] = v.v; break;
    case K::DateTime:
        j["text"] = v.text.value_or("");
        j["raw"] = v.rawDecimal;
        j["valid"] = v.valid;
        break;
    case K::Bits:
        j["count"] = v.count;
        j["set"] = v.set;
        j["hex"] = v.hex;
        j["truncated"] = v.truncated;
        break;
    case K::Bytes:
        j["length"] = v.length;
        j["hex"] = v.hex;
        j["truncated"] = v.truncated;
        if (v.text) j["text"] = *v.text;
        break;
    case K::Ptr: j["hex"] = v.hex; break;
    case K::Unavailable: j["reason"] = v.v; break;
    case K::Composite: j["preview"] = v.v; break;
    }
}

void to_json(json& j, const ContainerStats& v) {
    j = json::object();
    j["capacity"] = v.capacity;
    if (v.population) j["population"] = *v.population;
    if (v.removed) j["removed"] = *v.removed;
    if (v.povs) j["povs"] = *v.povs;
    if (!v.warning.empty()) j["warning"] = v.warning;
}

void to_json(json& j, const NodeInfo& v) {
    j = json::object();
    j["id"] = v.id;
    j["label"] = v.label;
    j["typeId"] = v.typeId;
    j["typeName"] = v.typeName;
    j["kind"] = kindName(v.kind);
    j["offset"] = v.offset;
    j["size"] = v.size;
    if (v.bit) j["bit"] = {{"offset", v.bit->first}, {"width", v.bit->second}};
    if (v.value) j["value"] = *v.value;
    if (!v.preview.empty()) j["preview"] = v.preview;
    j["childCount"] = v.childCount;
    if (v.rawChildCount) j["rawChildCount"] = *v.rawChildCount;
    if (v.container) j["container"] = *v.container;
    j["tabular"] = v.tabular;
    j["inFile"] = v.inFile;
    if (v.zero) j["zero"] = *v.zero;
}

void to_json(json& j, const ChildrenPage& v) {
    j = json::object();
    j["total"] = v.total;
    j["offset"] = v.offset;
    j["items"] = v.items;
}

void to_json(json& j, const NodeLocation& v) {
    j = json::object();
    j["id"] = v.id;
    j["path"] = json::array();
    for (const PathStep& s : v.path) j["path"].push_back({{"id", s.id}, {"label", s.label}});
    j["offset"] = v.offset;
    j["size"] = v.size;
    j["typeName"] = v.typeName;
}

void to_json(json& j, const NodeReveal& v) {
    j = json::object();
    j["id"] = v.id;
    j["path"] = json::array();
    for (const RevealStep& s : v.path)
        j["path"].push_back({{"id", s.id},
                             {"label", s.label},
                             {"index", s.index},
                             {"view", s.raw ? "raw" : "logical"},
                             {"childTotal", s.childTotal}});
    j["offset"] = v.offset;
    j["size"] = v.size;
    j["typeName"] = v.typeName;
    if (!v.blocked.empty()) j["blocked"] = v.blocked;
}

void to_json(json& j, const SearchResult& v) {
    j = json::object();
    json pattern = {{"mode", v.mode}, {"hex", v.patternHex}};
    if (!v.note.empty()) pattern["note"] = v.note;
    j["pattern"] = pattern;
    j["matches"] = json::array();
    for (const SearchMatch& m : v.matches)
        j["matches"].push_back({{"offset", m.offset}, {"length", m.length}, {"location", m.location}});
    j["truncated"] = v.truncated;
    j["elapsedMs"] = v.elapsedMs;
}

void to_json(json& j, const TableColumn& v) {
    j = {{"id", v.id},         {"label", v.label},       {"typeName", v.typeName}, {"kind", v.kind},
         {"group", v.group},   {"sortable", v.sortable}, {"filterable", v.filterable}};
}

void to_json(json& j, const TableInfo& v) {
    j = json::object();
    j["id"] = v.id;
    j["views"] = json::array();
    for (const TableView& w : v.views) j["views"].push_back({{"id", w.id}, {"label", w.label}});
    j["view"] = v.view;
    j["columns"] = v.columns;
    j["totalRows"] = v.totalRows;
    if (v.container) j["container"] = *v.container;
    if (v.elementTypeId) j["elementTypeId"] = *v.elementTypeId;
}

void to_json(json& j, const TableRow& v) {
    j = {{"index", v.index}, {"id", v.id}, {"cells", v.cells}};
}

void to_json(json& j, const TablePage& v) {
    j = json::object();
    j["total"] = v.total;
    j["offset"] = v.offset;
    j["rows"] = v.rows;
    j["elapsedMs"] = v.elapsedMs;
}

void from_json(const json& j, FilterSpec& v) {
    v.column = j.at("column").get<std::string>();
    v.op = j.at("op").get<std::string>();
    if (j.contains("value") && !j["value"].is_null()) v.value = j["value"].get<std::string>();
}

void from_json(const json& j, SortSpec& v) {
    v.column = j.at("column").get<std::string>();
    v.desc = j.value("desc", false);
}

void from_json(const json& j, TableRequest& v) {
    v.id = j.value("id", std::string());
    v.view = j.value("view", std::string());
    v.offset = j.value("offset", std::uint64_t{0});
    v.limit = j.value("limit", std::uint64_t{100});
    v.hideEmpty = j.value("hideEmpty", false);
    v.sort.clear();
    v.filters.clear();
    if (j.contains("sort") && j["sort"].is_array())
        for (const json& s : j["sort"]) v.sort.push_back(s.get<SortSpec>());
    if (j.contains("filters") && j["filters"].is_array())
        for (const json& f : j["filters"]) v.filters.push_back(f.get<FilterSpec>());
}

} // namespace qstate::decode
