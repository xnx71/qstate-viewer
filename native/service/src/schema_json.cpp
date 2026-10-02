#include "qstate/service/schema_json.h"

namespace qstate::service {

namespace {

using nlohmann::json;

const char* kindName(schema::TypeKind k) {
    switch (k) {
    case schema::TypeKind::Prim: return "prim";
    case schema::TypeKind::Enum: return "enum";
    case schema::TypeKind::Array: return "array";
    case schema::TypeKind::Pointer: return "pointer";
    case schema::TypeKind::Record: return "record";
    }
    return "record";
}

const char* primName(schema::PrimKind k) {
    switch (k) {
    case schema::PrimKind::Bool: return "bool";
    case schema::PrimKind::Char: return "char";
    case schema::PrimKind::SInt: return "sint";
    case schema::PrimKind::UInt: return "uint";
    case schema::PrimKind::Float: return "float";
    }
    return "uint";
}

const char* recordKindName(schema::RecordKind k) {
    switch (k) {
    case schema::RecordKind::Struct: return "struct";
    case schema::RecordKind::Class: return "class";
    case schema::RecordKind::Union: return "union";
    }
    return "struct";
}

json roleJson(const schema::Role& r) {
    using schema::RoleKind;
    switch (r.kind) {
    case RoleKind::None: return nullptr;
    case RoleKind::Id: return {{"kind", "id"}};
    case RoleKind::Bit: return {{"kind", "bit"}};
    case RoleKind::Uint128: return {{"kind", "uint128"}};
    case RoleKind::DateTime: return {{"kind", "dateTime"}};
    case RoleKind::Array: return {{"kind", "array"}, {"element", r.element}, {"capacity", r.capacity}};
    case RoleKind::BitArray: return {{"kind", "bitArray"}, {"capacity", r.capacity}};
    case RoleKind::HashMap: return {{"kind", "hashMap"}, {"key", r.key}, {"value", r.value}, {"capacity", r.capacity}};
    case RoleKind::HashSet: return {{"kind", "hashSet"}, {"key", r.key}, {"capacity", r.capacity}};
    case RoleKind::Collection: return {{"kind", "collection"}, {"element", r.element}, {"capacity", r.capacity}};
    case RoleKind::LinkedList: return {{"kind", "linkedList"}, {"element", r.element}, {"capacity", r.capacity}};
    }
    return nullptr;
}

} // namespace

json typeInfoJson(const schema::Type& t) {
    json j = {{"id", t.id}, {"name", t.name}, {"kind", kindName(t.kind)}, {"size", t.size}, {"align", t.align}};
    switch (t.kind) {
    case schema::TypeKind::Prim:
        j["prim"] = primName(t.prim);
        break;
    case schema::TypeKind::Enum: {
        if (t.underlying != schema::kNoType) j["underlying"] = t.underlying;
        json list = json::array();
        for (const schema::Enumerator& e : t.enumerators) list.push_back({{"name", e.name}, {"value", std::to_string(e.value)}});
        j["enumerators"] = std::move(list);
        break;
    }
    case schema::TypeKind::Array:
        j["element"] = t.element;
        j["count"] = t.count;
        break;
    case schema::TypeKind::Pointer:
        if (t.element != schema::kNoType) j["element"] = t.element;
        break;
    case schema::TypeKind::Record: {
        j["recordKind"] = recordKindName(t.recordKind);
        json fields = json::array();
        for (const schema::Field& f : t.fields) {
            json fj = {{"name", f.name}, {"type", f.type}, {"typeName", f.typeName}, {"offset", f.offset}, {"size", f.size}};
            if (f.bitWidth > 0) {
                fj["bitOffset"] = f.bitOffset;
                fj["bitWidth"] = f.bitWidth;
            }
            fields.push_back(std::move(fj));
        }
        j["fields"] = std::move(fields);
        if (!t.bases.empty()) {
            json bases = json::array();
            for (const schema::BaseClass& b : t.bases) bases.push_back({{"type", b.type}, {"typeName", b.typeName}, {"offset", b.offset}});
            j["bases"] = std::move(bases);
        }
        break;
    }
    }
    if (t.templateInfo) j["template"] = {{"name", t.templateInfo->name}, {"args", t.templateInfo->args}};
    if (t.role.kind != schema::RoleKind::None) j["role"] = roleJson(t.role);
    if (t.source) j["source"] = {{"file", t.source->file}, {"line", t.source->line}};
    return j;
}

} // namespace qstate::service
