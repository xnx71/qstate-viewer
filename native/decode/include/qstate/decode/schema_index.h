// Pre-digested view of a schema::Schema for decoding: per type classification, flattened member lists (base classes
// inlined) and validated container layouts.
//
// Container record-field conventions (what schema extraction must deliver as Role + fields, mirroring qpi_containers.h):
//
//   Id (32 bytes), Bit (1 byte), Uint128 (16 bytes: low@0, high@8), DateTime (8 bytes uint64)
//   Array / SlowAnySizeArray : role.element, role.capacity; the data member is `_values` (T[L], offset 0)
//   BitArray<L>              : role.capacity = number of BITS; member `_values` (uint64[(L+63)/64])
//   HashMap<K,V,L>           : `_elements` (Element[L], Element = record with `key`, `value`),
//                              `_occupationFlags` (uint64[(2L+63)/64]), `_population`, `_markRemovalCounter` (uint64)
//   HashSet<K,L>             : `_keys` (K[L]), `_occupationFlags`, `_population`, `_markRemovalCounter`
//   Collection<T,L>          : `_povs` (PoV[L], PoV = record: `value` (id), `population`, `headIndex`, `tailIndex`,
//                              `bstRootIndex`), `_povOccupationFlags`, `_elements` (Element[L], Element = record:
//                              `value`, `priority`, `povIndex`, `bstParentIndex`, `bstLeftIndex`, `bstRightIndex`),
//                              `_population`, `_markRemovalCounter`
//   LinkedList<T,L>          : `_nodes` (Node[L], Node = record: `value`, `nextIndex`, `prevIndex`), `_occupiedFlags`
//                              (uint64[(L+63)/64]), `_headIndex`, `_tailIndex`, `_freeHeadIndex`, `_nextUnusedIndex`,
//                              `_population`
//
// A container whose record does not follow the convention degrades to a plain struct (raw members only) and carries
// `layoutError`.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qstate/schema/model.h"

namespace qstate::decode {

enum class Cls : std::uint8_t {
    Record, Union,
    CArray,            // T[N]
    ArrayRole,         // QPI::Array / SlowAnySizeArray (same decoding as CArray)
    BitArray,
    HashMap, HashSet, Collection, LinkedList,
    Int, Bool, Char, Enum, Float, Ptr, Id, Bit, U128, DateTime,
    Opaque,            // anything else with bytes but no decoding (odd primitive sizes)
    // Ledger files (spectrum.EEE / universe.EEE), see LedgerLayout.
    Ledger,            // the whole table: recordType[capacity], open addressing by publicKey (elem / count / elemSize set)
    EntityRecord,      // one spectrum record (a Record with members; `ledger` holds the field offsets)
    AssetRecord        // one universe record (a Record whose single member is the issuance / ownership / possession union)
};

struct Member {
    std::string name;
    schema::TypeId type = schema::kNoType;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t bitOffset = 0;
    std::uint32_t bitWidth = 0;
    bool assetNameHint = false; // uint64 named like *assetName / *asset / *name
};

struct HashLayout {
    bool isMap = false;
    std::uint64_t capacity = 0;
    schema::TypeId keyType = schema::kNoType, valueType = schema::kNoType, elementType = schema::kNoType;
    std::uint64_t elemsOff = 0, elemSize = 0, keyOff = 0, valueOff = 0, keySize = 0, valueSize = 0;
    std::uint64_t flagsOff = 0, popOff = 0, mrcOff = 0;
    bool keyAssetHint = false, valueAssetHint = false;
};

struct CollectionLayout {
    std::uint64_t capacity = 0;
    schema::TypeId valueType = schema::kNoType, povType = schema::kNoType, elementType = schema::kNoType;
    std::uint64_t povsOff = 0, povSize = 0, povValueOff = 0, povPopOff = 0, povHeadOff = 0, povTailOff = 0,
                  povRootOff = 0, povFlagsOff = 0;
    std::uint64_t elemsOff = 0, elemSize = 0, valueOff = 0, valueSize = 0, prioOff = 0, povIdxOff = 0, parentOff = 0,
                  leftOff = 0, rightOff = 0;
    std::uint64_t popOff = 0, mrcOff = 0;
};

struct ListLayout {
    std::uint64_t capacity = 0;
    schema::TypeId valueType = schema::kNoType, nodeType = schema::kNoType;
    std::uint64_t nodesOff = 0, nodeSize = 0, valueOff = 0, valueSize = 0, nextOff = 0, prevOff = 0;
    std::uint64_t flagsOff = 0, headOff = 0, tailOff = 0, freeHeadOff = 0, nextUnusedOff = 0, popOff = 0;
};

// Field offsets of the ledger record types (both the table and its record type carry a copy). Universe records are a
// union of three alternatives selected by the `type` byte (1 issuance, 2 ownership, 3 possession); every alternative
// starts with publicKey (32 bytes) and type (1 byte).
struct LedgerLayout {
    schema::LedgerKind kind = schema::LedgerKind::Spectrum;
    schema::TypeId recordType = schema::kNoType;
    std::uint64_t recordSize = 0;
    std::uint64_t capacity = 0;       // table only
    std::uint64_t keyOff = 0;         // publicKey
    // spectrum
    std::uint64_t incomingOff = 0, outgoingOff = 0; // long long
    // universe
    std::uint64_t typeOff = 0;        // unsigned char
    std::string unionField;           // "varStruct"
    schema::TypeId unionType = schema::kNoType;
    struct Alternative {
        std::string name;             // issuance | ownership | possession
        schema::TypeId type = schema::kNoType;
        std::uint64_t offset = 0;     // of the alternative inside the record (its members are relative to it)
        // issuance
        std::uint64_t nameOff = 0, decimalsOff = 0, uomOff = 0;
        // ownership / possession
        std::uint64_t contractOff = 0, indexOff = 0, sharesOff = 0; // indexOff: issuanceIndex / ownershipIndex (u32)
    };
    Alternative alts[3];              // [type - 1]
};

struct TypeX {
    Cls cls = Cls::Opaque;
    std::uint64_t size = 0;
    std::vector<Member> members;          // Record / Union / container records: base members first, declaration order
    schema::TypeId elem = schema::kNoType; // CArray / ArrayRole / Ledger element
    std::uint64_t count = 0;               // CArray / ArrayRole / Ledger element count; BitArray: bits
    std::uint64_t elemSize = 0;
    bool byteElems = false;                // elements are 1-byte integers / chars (text-like)
    std::uint32_t intBits = 0;             // Int / Enum / Char: width in bits
    bool intSigned = false;
    HashLayout hash;
    CollectionLayout coll;
    ListLayout list;
    LedgerLayout ledger;                   // Ledger / EntityRecord / AssetRecord
    std::string layoutError;               // container degraded to Record when non-empty
};

class SchemaIndex {
public:
    explicit SchemaIndex(std::shared_ptr<const schema::Schema> schema);

    const schema::Schema& schema() const { return *schema_; }
    std::shared_ptr<const schema::Schema> schemaPtr() const { return schema_; }
    const TypeX& x(schema::TypeId id) const; // throws std::out_of_range for an invalid id
    bool valid(schema::TypeId id) const { return id < types_.size(); }
    const schema::Type& type(schema::TypeId id) const { return schema_->types.at(id); }

private:
    void buildType(schema::TypeId id);
    void buildMembers(schema::TypeId id, TypeX& tx);
    void buildContainer(schema::TypeId id, TypeX& tx);
    // Validates a ledger record type; returns the error ("" = ok) and fills `out`.
    std::string buildLedgerRecord(schema::TypeId id, schema::LedgerKind kind, LedgerLayout& out) const;

    std::shared_ptr<const schema::Schema> schema_;
    std::vector<TypeX> types_;
};

bool assetNameLike(const std::string& fieldName);

} // namespace qstate::decode
