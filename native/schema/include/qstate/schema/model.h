// Schema model: the fully laid-out types of all contract states. Produced by schema extraction (qstate::schema,
// from the Qubic core headers via qstate::cpp) and consumed by the decoder (qstate::decode). Plain data, no logic
// beyond small helpers, so tests can build schemas by hand.
//
// All sizes / offsets are bytes of the x86-64 layout the node was compiled with.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qstate/cpp/diag.h"

namespace qstate::schema {

using TypeId = std::uint32_t;
constexpr TypeId kNoType = 0xFFFFFFFFu;

enum class TypeKind : std::uint8_t { Prim, Enum, Array, Pointer, Record };
enum class PrimKind : std::uint8_t { Bool, Char, SInt, UInt, Float };
enum class RecordKind : std::uint8_t { Struct, Class, Union };

// Semantic meaning recognised for a type (QPI types). RoleKind::None = plain type.
enum class RoleKind : std::uint8_t {
    None,
    Id,         // QPI::id / m256i: 32 bytes
    Bit,        // QPI::bit: 1 byte, value 0 / 1
    Uint128,    // QPI::uint128
    DateTime,   // QPI::DateAndTime
    Array,      // QPI::Array<T, L> and QPI::SlowAnySizeArray<T, L>
    BitArray,   // QPI::BitArray<L>
    HashMap,    // QPI::HashMap<K, V, L, HashFunc>
    HashSet,    // QPI::HashSet<K, L, HashFunc>
    Collection, // QPI::Collection<T, L>
    LinkedList, // QPI::LinkedList<T, L>
};

struct Role {
    RoleKind kind = RoleKind::None;
    TypeId element = kNoType; // Array, Collection, LinkedList: element type
    TypeId key = kNoType;     // HashMap, HashSet
    TypeId value = kNoType;   // HashMap
    std::uint64_t capacity = 0;
};

struct SourceLoc {
    std::string file; // relative to the core root
    std::uint32_t line = 0;
};

struct Field {
    std::string name;
    TypeId type = kNoType;
    std::string typeName; // display name of `type`
    std::uint64_t offset = 0; // from the start of the owning record (for union members always 0)
    std::uint64_t size = 0;
    // Bit-fields only (bitWidth > 0): offset is the byte offset of the storage unit, bitOffset counts from its LSB.
    std::uint32_t bitOffset = 0;
    std::uint32_t bitWidth = 0;
};

struct BaseClass {
    TypeId type = kNoType;
    std::string typeName;
    std::uint64_t offset = 0;
};

struct Enumerator {
    std::string name;
    std::int64_t value = 0;
};

struct TemplateInfo {
    std::string name;               // "QPI::Collection"
    std::vector<std::string> args;  // rendered arguments: "QX::AssetOrder", "2097152"
};

struct Type {
    TypeId id = kNoType;
    // Canonical display name, e.g. "QPI::Collection<QX::AssetOrder, 2097152>", "unsigned long long", "QX::StateData".
    std::string name;
    TypeKind kind = TypeKind::Prim;
    std::uint64_t size = 0;
    std::uint64_t align = 1;

    // Prim
    PrimKind prim = PrimKind::UInt;
    bool isSigned = false;
    // Enum: underlying integer type
    TypeId underlying = kNoType;
    std::vector<Enumerator> enumerators;
    // Array (C array T[count]); Pointer: element = pointee
    TypeId element = kNoType;
    std::uint64_t count = 0;
    // Record
    RecordKind recordKind = RecordKind::Struct;
    std::vector<Field> fields; // non-static data members in declaration order (anonymous struct / union members are
                               // flattened with an empty name is NOT allowed: give them names like "$anon0")
    std::vector<BaseClass> bases;
    std::optional<TemplateInfo> templateInfo;
    Role role;
    std::optional<SourceLoc> source;
};

struct ContractSchema {
    std::uint32_t index = 0;
    std::string name;        // contractDescriptions[].assetName; "" for index 0
    std::string structName;  // C++ struct that declares the contract ("QX"); empty for index 0
    std::string stateTypeName; // "QX::StateData" | "IPO" | "Contract0State"
    TypeId stateType = kNoType; // kNoType if the layout could not be computed
    std::string headerFile;  // relative to the core root; empty when unknown
    std::uint32_t constructionEpoch = 0;
    std::uint32_t destructionEpoch = 0;
    std::uint64_t expectedSize = 0; // sizeof(state type) as evaluated from the sources (0 if unknown)
    std::string error;       // non-empty when the layout could not be computed
};

struct Schema {
    std::vector<Type> types; // types[i].id == i
    std::vector<ContractSchema> contracts; // ascending by index
    qstate::cpp::Diags diags;

    const Type& type(TypeId id) const { return types.at(id); }
    const ContractSchema* contract(std::uint32_t index) const {
        for (const ContractSchema& c : contracts)
            if (c.index == index) return &c;
        return nullptr;
    }
    const Field* findField(const Type& t, const std::string& name) const {
        for (const Field& f : t.fields)
            if (f.name == name) return &f;
        return nullptr;
    }
};

} // namespace qstate::schema
