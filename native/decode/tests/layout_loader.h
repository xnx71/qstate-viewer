// TESTS ONLY: turns the ground-truth layout JSON of docs/research/data (contract-layouts-a/b.json) into a
// schema::Schema (what the real schema extraction will deliver) for one contract.
#pragma once

#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "qstate/schema/model.h"

namespace qstate::decode::testing {

struct LoadedContract {
    std::shared_ptr<schema::Schema> schema;
    schema::TypeId root = schema::kNoType;
    std::uint64_t stateSize = 0;
    std::string name;
};

class LayoutLoader {
public:
    using json = nlohmann::json;

    LayoutLoader(const std::string& path, const std::string& version) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open " + path);
        doc_ = json::parse(in);
        ver_ = &doc_.at("versions").at(version);
        types_ = &ver_->at("types");
    }

    bool hasContract(int index) const { return findContract(index) != nullptr; }

    LoadedContract load(int index) {
        const json* c = findContract(index);
        if (!c) throw std::runtime_error("contract not in the layout file");
        LoadedContract out;
        out.schema = std::make_shared<schema::Schema>();
        s_ = out.schema.get();
        memo_.clear();
        const std::string stateType = c->contains("stateTypeCanonical") ? (*c)["stateTypeCanonical"].get<std::string>()
                                                                          : (*c)["stateType"].get<std::string>();
        out.root = resolve(stateType);
        out.stateSize = s_->types[out.root].size;
        out.name = c->value("assetName", std::string());
        return out;
    }

private:
    json doc_;
    const json* ver_ = nullptr;
    const json* types_ = nullptr;
    schema::Schema* s_ = nullptr;
    std::map<std::string, schema::TypeId> memo_;

    const json* findContract(int index) const {
        const json& cs = ver_->at("contracts");
        if (cs.is_object()) {
            auto it = cs.find(std::to_string(index));
            return it == cs.end() ? nullptr : &*it;
        }
        for (const json& c : cs)
            if (c.contains("index") && c["index"].is_number() && c["index"].get<int>() == index) return &c;
        return nullptr;
    }

    schema::TypeId add(schema::Type t) {
        t.id = static_cast<schema::TypeId>(s_->types.size());
        s_->types.push_back(std::move(t));
        return s_->types.back().id;
    }

    schema::TypeId prim(const std::string& name, std::uint64_t size, schema::PrimKind k, bool sgn) {
        schema::Type t;
        t.name = name;
        t.kind = schema::TypeKind::Prim;
        t.size = size;
        t.align = size;
        t.prim = k;
        t.isSigned = sgn;
        return add(std::move(t));
    }

    static schema::RoleKind roleOf(const std::string& n) {
        if (n.empty() || n.back() != '>') return schema::RoleKind::None;
        auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
        if (starts("HashMap<")) return schema::RoleKind::HashMap;
        if (starts("HashSet<")) return schema::RoleKind::HashSet;
        if (starts("Collection<")) return schema::RoleKind::Collection;
        if (starts("LinkedList<")) return schema::RoleKind::LinkedList;
        if (starts("Array<") || starts("SlowAnySizeArray<")) return schema::RoleKind::Array;
        if (starts("BitArray<")) return schema::RoleKind::BitArray;
        return schema::RoleKind::None;
    }

    schema::TypeId resolve(const std::string& name) {
        if (auto it = memo_.find(name); it != memo_.end()) return it->second;
        schema::TypeId id = build(name);
        memo_[name] = id;
        return id;
    }

    schema::TypeId build(const std::string& name) {
        // C arrays: "T[a][b]" = array a of (array b of T)
        if (!name.empty() && name.back() == ']') {
            const std::size_t br = name.find('[');
            const std::string base = name.substr(0, br);
            const std::string rest = name.substr(br);
            const std::size_t close = rest.find(']');
            const std::uint64_t count = std::stoull(rest.substr(1, close - 1));
            const std::string inner = base + rest.substr(close + 1);
            const schema::TypeId elem = resolve(inner);
            schema::Type t;
            t.name = name;
            t.kind = schema::TypeKind::Array;
            t.element = elem;
            t.count = count;
            t.size = s_->types[elem].size * count;
            t.align = s_->types[elem].align;
            return add(std::move(t));
        }
        using PK = schema::PrimKind;
        if (name == "uint8") return prim(name, 1, PK::UInt, false);
        if (name == "uint16") return prim(name, 2, PK::UInt, false);
        if (name == "uint32") return prim(name, 4, PK::UInt, false);
        if (name == "uint64") return prim(name, 8, PK::UInt, false);
        if (name == "sint8") return prim(name, 1, PK::SInt, true);
        if (name == "sint16") return prim(name, 2, PK::SInt, true);
        if (name == "sint32") return prim(name, 4, PK::SInt, true);
        if (name == "sint64") return prim(name, 8, PK::SInt, true);
        if (name == "bool") return prim(name, 1, PK::Bool, false);
        if (name == "char") return prim(name, 1, PK::Char, true);
        if (name == "id") {
            schema::Type t;
            t.name = "id";
            t.kind = schema::TypeKind::Record;
            t.size = 32;
            t.align = 8;
            t.role.kind = schema::RoleKind::Id;
            return add(std::move(t));
        }
        auto it = types_->find(name);
        if (it == types_->end()) throw std::runtime_error("unknown type in layout file: " + name);
        const json& j = *it;
        const std::string kind = j.at("kind");
        if (kind == "enum") {
            schema::Type t;
            t.name = name;
            t.kind = schema::TypeKind::Enum;
            t.size = j.at("size");
            t.align = j.value("align", t.size);
            t.underlying = resolve(j.at("underlying"));
            for (auto e = j["enumerators"].begin(); e != j["enumerators"].end(); ++e)
                t.enumerators.push_back({e.key(), e.value().get<std::int64_t>()});
            return add(std::move(t));
        }
        schema::Type t;
        t.name = name;
        t.kind = schema::TypeKind::Record;
        t.recordKind = kind == "union" ? schema::RecordKind::Union : schema::RecordKind::Struct;
        t.size = j.at("size");
        t.align = j.value("align", std::uint64_t{1});
        for (const json& f : j.at("fields")) {
            schema::Field fld;
            fld.name = f.at("name");
            const std::string ftype = f.contains("canonicalType") ? f["canonicalType"].get<std::string>()
                                                                   : f["resolvedType"].get<std::string>();
            fld.type = resolve(ftype);
            fld.typeName = ftype;
            fld.offset = f.at("offset");
            fld.size = f.at("size");
            t.fields.push_back(std::move(fld));
        }
        if (name.rfind("ProposalWithAllVoteData<", 0) == 0) {
            // the layout JSON lists only the derived members; the real extraction reports the base class
            const std::size_t comma = name.find(',');
            const std::string base = name.substr(name.find('<') + 1, comma - name.find('<') - 1);
            schema::BaseClass b;
            b.type = resolve(base);
            b.typeName = base;
            b.offset = 0;
            t.bases.push_back(b);
        }
        if (name == "DateAndTime") t.role.kind = schema::RoleKind::DateTime;
        else if (name == "uint128_t") t.role.kind = schema::RoleKind::Uint128;
        else if (name == "bit") t.role.kind = schema::RoleKind::Bit;
        else t.role.kind = roleOf(name);
        auto fieldType = [&](const schema::Type& rec, const char* fname) -> schema::TypeId {
            for (const auto& f : rec.fields)
                if (f.name == fname) return f.type;
            return schema::kNoType;
        };
        auto arrayElem = [&](const char* fname) -> schema::TypeId {
            const schema::TypeId at = fieldType(t, fname);
            return at == schema::kNoType ? at : s_->types[at].element;
        };
        auto arrayCount = [&](const char* fname) -> std::uint64_t {
            const schema::TypeId at = fieldType(t, fname);
            return at == schema::kNoType ? 0 : s_->types[at].count;
        };
        switch (t.role.kind) {
        case schema::RoleKind::HashMap: {
            const schema::TypeId el = arrayElem("_elements");
            t.role.key = fieldType(s_->types[el], "key");
            t.role.value = fieldType(s_->types[el], "value");
            t.role.capacity = arrayCount("_elements");
            break;
        }
        case schema::RoleKind::HashSet:
            t.role.key = arrayElem("_keys");
            t.role.capacity = arrayCount("_keys");
            break;
        case schema::RoleKind::Collection: {
            const schema::TypeId el = arrayElem("_elements");
            t.role.element = fieldType(s_->types[el], "value");
            t.role.capacity = arrayCount("_elements");
            break;
        }
        case schema::RoleKind::LinkedList: {
            const schema::TypeId el = arrayElem("_nodes");
            t.role.element = fieldType(s_->types[el], "value");
            t.role.capacity = arrayCount("_nodes");
            break;
        }
        case schema::RoleKind::Array:
            t.role.element = arrayElem("_values");
            t.role.capacity = arrayCount("_values");
            break;
        case schema::RoleKind::BitArray: {
            const std::size_t lt = name.find('<');
            t.role.capacity = std::stoull(name.substr(lt + 1));
            break;
        }
        default: break;
        }
        return add(std::move(t));
    }
};

} // namespace qstate::decode::testing
