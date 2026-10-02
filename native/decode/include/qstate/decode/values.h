// C++ mirror of the data shapes of ui/src/rpc/contract.ts that the decoder produces and consumes
// (NodeInfo, LeafValue, ContainerStats, ChildrenPage, NodeLocation, SearchResult, Table*). Conversion to / from the exact
// JSON of the contract is in json.h.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qstate/schema/model.h"

namespace qstate::decode {

using NodeId = std::string; // "" = root; opaque, stable path-like text ("f:_assetOrders/p:12/e:7")
using TypeId = schema::TypeId;

enum class NodeKind : std::uint8_t {
    Struct, Union, Array, BitArray, HashMap, HashSet, Collection, LinkedList, Entry, Pov, Leaf
};
const char* kindName(NodeKind kind); // "struct", "union", "array", "bitArray", "hashMap", ... (contract.ts NodeKind)

// One decoded value. Flat struct: `k` selects which members are meaningful (see contract.ts LeafValue).
struct LeafValue {
    enum class Kind : std::uint8_t {
        Int, Bool, Char, Enum, Id, U128, Float, DateTime, Bits, Bytes, Ptr, Unavailable,
        Composite // table cells only: { k: "composite", preview }
    };
    Kind k = Kind::Unavailable;

    // Int / Enum / U128 / Float: exact decimal (Float: shortest round-trip text). Char: the character as text.
    // Unavailable: the reason. Composite: the preview.
    std::string v;
    bool isUnsigned = false;       // Int
    std::uint32_t bits = 0;        // Int
    std::string hex;               // Int ("0x.."), Id (64 chars), U128 ("0x.."), Bits, Bytes (first <= 64 bytes), Ptr
    std::optional<std::string> text; // Int (asset name), Id, Bytes (printable), DateTime (formatted text)
    bool boolean = false;          // Bool
    std::uint32_t raw = 0;         // Bool: raw byte; Char: code
    std::optional<std::string> name; // Enum: enumerator name
    // Id
    std::string identity;          // 60 letters
    bool zero = false;
    std::optional<std::uint32_t> contractIndex;
    std::string contractName;      // meaningful when contractIndex is set (may be "")
    // DateTime
    std::string rawDecimal;        // uint64 as decimal
    bool valid = false;
    // Bits
    std::uint64_t count = 0, set = 0;
    // Bits / Bytes
    bool truncated = false;
    std::uint64_t length = 0;      // Bytes

    static LeafValue unavailable(std::string reason);
    static LeafValue composite(std::string preview);
};

const char* kindName(LeafValue::Kind kind); // "int", "bool", ... (contract.ts LeafValue["k"]) / "composite"

struct ContainerStats {
    std::uint64_t capacity = 0;
    std::optional<std::uint64_t> population; // live elements
    std::optional<std::uint64_t> removed;    // hash containers: slots marked for removal
    std::optional<std::uint64_t> povs;       // collections: number of live PoVs
    std::string warning;                     // empty = none
};

struct NodeInfo {
    NodeId id;
    std::string label;
    TypeId typeId = schema::kNoType;
    std::string typeName;
    NodeKind kind = NodeKind::Leaf;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> bit; // (offset, width) for bit-fields
    std::optional<LeafValue> value;
    std::string preview;                      // empty = none
    std::uint64_t childCount = 0;
    std::optional<std::uint64_t> rawChildCount;
    std::optional<ContainerStats> container;
    bool tabular = false;
    bool inFile = false;
    std::optional<bool> zero;
};

struct ChildrenPage {
    std::uint64_t total = 0;
    std::uint64_t offset = 0;
    std::vector<NodeInfo> items;
};

struct PathStep {
    NodeId id;
    std::string label;
};

struct NodeLocation {
    NodeId id;
    std::vector<PathStep> path; // root -> node
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::string typeName;
};

// One element of the path returned by StateDecoder::reveal: where `id` sits in its parent's child list.
struct RevealStep {
    NodeId id;
    std::string label;
    std::int64_t index = 0;          // position inside the parent's child list of `raw`; 0 for the root, -1 = not listed
    bool raw = false;                // the parent's child list is the raw (C++ members) view
    std::uint64_t childTotal = 0;    // number of children of this node in the view the next step uses (logical for the last)
};

struct NodeReveal {
    NodeId id;
    std::vector<RevealStep> path;    // root -> node, like NodeLocation::path but with the exact child positions
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::string typeName;
    std::string blocked;             // "" | "hideEmpty" (a node on the path is hidden) | "orphan" (collection element without a PoV)
};

struct SearchMatch {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    NodeLocation location;
};

struct SearchResult {
    std::string mode;     // "id" | "hex" | "int" | "text"
    std::string patternHex;
    std::string note;     // empty = none
    std::vector<SearchMatch> matches;
    bool truncated = false;
    double elapsedMs = 0;
};

// ---- tables ---------------------------------------------------------------------------------------------------------

struct TableColumn {
    std::string id;       // "$index", "$pov", "$priority", "key", "value.entity"
    std::string label;
    std::string typeName;
    std::string kind;     // LeafValue kind name ("int", "id", ...) or "composite"
    std::string group;    // "meta" | "key" | "value"
    bool sortable = false;
    bool filterable = false;
};

struct TableView {
    std::string id, label;
};

struct TableInfo {
    NodeId id;
    std::vector<TableView> views;
    std::string view;
    std::vector<TableColumn> columns;
    std::uint64_t totalRows = 0;
    std::optional<ContainerStats> container;
    std::optional<TypeId> elementTypeId;
};

struct FilterSpec {
    std::string column;
    std::string op; // eq ne lt le gt ge contains zero nonzero
    std::optional<std::string> value;
};

struct SortSpec {
    std::string column;
    bool desc = false;
};

struct TableRequest {
    NodeId id;
    std::string view; // "" = default view
    std::uint64_t offset = 0;
    std::uint64_t limit = 100; // clamped to 1000
    std::vector<SortSpec> sort;
    std::vector<FilterSpec> filters;
    bool hideEmpty = false;
};

struct TableRow {
    std::uint64_t index = 0;
    NodeId id;
    std::vector<LeafValue> cells; // LeafValue::Kind::Composite for composite cells
};

struct TablePage {
    std::uint64_t total = 0;
    std::uint64_t offset = 0;
    std::vector<TableRow> rows;
    double elapsedMs = 0;
};

} // namespace qstate::decode
