// Internal implementation of StateDecoder.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "format.h"
#include "int128.h"
#include "qstate/decode/decoder.h"

namespace qstate::decode {

using schema::kNoType;

constexpr std::int64_t kNullIndex = -1;

// ---- node references ------------------------------------------------------------------------------------------------

enum class RK : std::uint8_t {
    Plain,     // a value of schema type `type` at `offset`
    MapEntry,  // live HashMap slot; `base` = container offset, `index` = slot, `type` = container type
    CollEntry, // Collection element; `index` = element index
    Pov,       // live Collection PoV; `index` = slot
    Bit        // one bit of a BitArray: offset = byte, bitOffset = bit in byte
};

struct Ref {
    RK rk = RK::Plain;
    TypeId type = kNoType;
    std::uint64_t offset = 0, size = 0;
    std::uint32_t bitOffset = 0, bitWidth = 0;
    std::uint64_t index = 0;
    std::uint64_t base = 0;
    bool assetHint = false;
    bool ledgerTypeHint = false; // the `type` byte of a universe record: rendered as an enum (issuance / ownership / possession)
    bool labelPending = false; // label must be derived from the bytes (entries, PoVs, set elements)
    std::string label;
    NodeId id;
};

// ---- cached derived data --------------------------------------------------------------------------------------------

struct Flags2Scan {
    std::vector<std::uint32_t> live; // slots in state 0b01, ascending
    std::uint64_t tombstones = 0;    // state 0b10
    std::uint64_t invalid = 0;       // state 0b11
    bool truncated = false;          // flag array extends beyond the end of the file
};

struct PovRec {
    std::uint32_t slot = 0;
    std::uint64_t population = 0;
    std::int64_t head = kNullIndex, tail = kNullIndex, root = kNullIndex;
    std::uint8_t id[32] = {};
};

struct CollInfo {
    std::shared_ptr<const Flags2Scan> flags;
    std::vector<PovRec> povs;           // live PoVs, ascending slot
    std::uint64_t storedPop = 0, storedMrc = 0;
    std::uint64_t elemCount = 0;        // min(storedPop, capacity, elements inside the file)
    std::uint64_t popSum = 0;           // sum of PoV populations
    bool popReadable = false;
    ContainerStats stats;
    const PovRec* findPov(std::uint64_t slot) const;
};

struct ListInfo {
    bool readable = false;
    std::uint64_t population = 0;       // stored, clamped to capacity
    std::uint64_t storedPopulation = 0;
    std::int64_t head = kNullIndex, tail = kNullIndex;
    std::uint64_t flagPopulation = 0;   // popcount of the occupied bits
    ContainerStats stats;
};

struct HashInfo {
    std::shared_ptr<const Flags2Scan> flags;
    ContainerStats stats;
};

struct IndexList { // generic cached vector of 32-bit ids
    std::vector<std::uint32_t> ids;
};

// One bulk pass over a ledger table (ledger.cpp): the live slots plus the aggregates of ledger.summary and the
// cross-slot maps the universe table views need (asset name of an ownership / possession). Cached per generation.
struct LedgerScan {
    std::vector<std::uint32_t> live;        // live slots, ascending
    bool truncated = false;                 // the file ends before the table does
    // spectrum
    wide::i128 totalSupply = 0;             // sum of incomingAmount - outgoingAmount
    std::uint64_t nonZeroBalances = 0;      // balance > 0
    std::uint32_t maxSlot = 0;
    wide::i128 maxBalance = 0;
    bool haveMax = false;
    // universe
    std::vector<std::uint32_t> byType[3];   // slots of type 1, 2, 3 (ascending)
    std::uint64_t otherTypes = 0;           // live records with a type outside 1..3
    struct Issuance {
        std::uint32_t slot = 0;
        std::uint8_t key[32] = {};
        char name[8] = {};                  // NUL terminated
        std::int8_t decimals = 0;
        std::uint8_t uom[7] = {};
        std::uint64_t ownerships = 0, possessions = 0;
        wide::i128 totalShares = 0;
    };
    std::vector<Issuance> issuances;        // ascending slot
    struct Ownership {
        std::uint32_t slot = 0, issuanceIndex = 0;
        std::int64_t shares = 0;
    };
    std::vector<Ownership> ownerships;      // ascending slot
    struct Possession {
        std::uint32_t slot = 0, ownershipIndex = 0;
    };
    std::vector<Possession> possessions;    // ascending slot

    const Issuance* issuance(std::uint64_t slot) const;    // by slot, nullptr when the slot is not an issuance
    const Ownership* ownership(std::uint64_t slot) const;
    // Name of the asset a record of `type` with index `idx` (issuanceIndex / ownershipIndex) refers to; "" if unresolved.
    std::string assetName(unsigned type, std::uint64_t idx) const;
    std::size_t bytes() const;
};

struct ChildList {
    std::uint64_t total = 0;
    std::vector<Ref> items;
};

struct TableModel;

class StateDecoder::Impl {
public:
    Impl(std::shared_ptr<const SchemaIndex> index, TypeId rootType, std::shared_ptr<const ByteSource> source,
         DecoderConfig config);

    ~Impl();

    // ---- configuration / services
    std::shared_ptr<const SchemaIndex> idxp;
    const SchemaIndex& idx;
    TypeId rootType;
    std::shared_ptr<const ByteSource> src;
    DecoderConfig cfg;
    std::shared_ptr<const IdentityCodec> codec;
    std::shared_ptr<DecodeCache> cache;
    std::string scopePrefix;
    mutable std::atomic<std::uint64_t> lastGen{0};
    mutable std::atomic<bool> genSeen{false};
    Formatter fmt;

    void touch(const Query& q) const; // generation handling
    static void checkCancel(const Query& q) {
        if (q.cancel && q.cancel->load(std::memory_order_relaxed)) throw CancelledError();
    }
    std::string key(const Query& q, const std::string& what) const {
        return scopePrefix + std::to_string(q.generation) + "|" + what;
    }

    // ---- raw reading
    std::size_t read(std::uint64_t off, std::size_t n, std::uint8_t* out) const { return src->read(off, n, out); }
    bool readExact(std::uint64_t off, std::size_t n, std::uint8_t* out) const { return src->read(off, n, out) == n; }
    std::optional<std::uint64_t> readU64(std::uint64_t off) const;
    std::optional<std::int64_t> readS64(std::uint64_t off) const {
        auto v = readU64(off);
        if (!v) return std::nullopt;
        return static_cast<std::int64_t>(*v);
    }
    bool inFile(std::uint64_t off, std::uint64_t size) const {
        const std::uint64_t fs = src->size();
        return off <= fs && size <= fs - off;
    }

    // ---- nodes
    Ref rootRef() const;
    Ref resolve(const NodeId& id, const Query& q) const;
    Ref childBySegment(const Ref& parent, std::string_view seg, const Query& q) const;
    void finalize(Ref& r) const; // computes pending labels
    NodeInfo describe(Ref r, const Query& q) const;
    ChildList listChildren(const Ref& parent, ChildView view, std::uint64_t offset, std::uint64_t limit,
                           bool hideEmpty, const Query& q) const;
    NodeLocation locate(std::uint64_t offset, const Query& q) const;
    NodeReveal reveal(const NodeId& id, bool hideEmpty, const Query& q) const;
    std::optional<Ref> povOfElement(const Ref& container, const Ref& element, const Query& q) const; // reveal.cpp
    // Position of the child addressed by segment `kind:arg` inside `parent`'s child list (reveal.cpp).
    struct ChildPos {
        std::uint64_t index = 0;
        bool raw = false;
        bool listed = true;
        const char* blocked = ""; // why it is not listed
    };
    ChildPos childPosition(const Ref& parent, char kind, std::string_view arg, bool hideEmpty, const Query& q) const;
    std::string typeNameOfRef(const Ref& r) const;
    SearchResult search(const SearchRequest& req, const Query& q) const;

    // classification
    Cls clsOf(const Ref& r) const {
        return r.rk == RK::Plain ? idx.x(r.type).cls : Cls::Record;
    }
    bool isContainerCls(Cls c) const {
        return c == Cls::HashMap || c == Cls::HashSet || c == Cls::Collection || c == Cls::LinkedList;
    }
    std::string typeNameOf(TypeId t) const { return idx.valid(t) ? idx.type(t).name : std::string(); }

    // ref construction
    Ref makeRoot() const;
    Ref plainRef(const Ref& parent, std::string seg, std::string label, TypeId type, std::uint64_t relOffset,
                 std::uint64_t size, bool assetHint = false) const;
    Ref memberRef(const Ref& parent, const Member& m) const;
    Ref indexRef(const Ref& parent, std::uint64_t i) const; // C array / role array element
    Ref bitRef(const Ref& parent, std::uint64_t i) const;
    Ref mapEntryRef(const Ref& container, std::uint64_t slot) const;
    Ref setElemRef(const Ref& container, std::uint64_t slot) const;
    Ref povRef(const Ref& container, std::uint64_t slot) const;
    Ref collEntryRef(const Ref& container, std::uint64_t idx) const;
    Ref listElemRef(const Ref& container, std::uint64_t node) const;
    Ref entryChild(const Ref& entry, std::string_view name) const; // f:key / f:value / f:priority / f:pov ...
    Ref containerOf(const Ref& pov) const;                          // the Collection a PoV node belongs to
    const Member* findMember(TypeId t, std::string_view name) const;

    // ---- container data
    std::shared_ptr<const Flags2Scan> scanFlags2(std::uint64_t flagsOff, std::uint64_t capacity, const Query& q) const;
    HashInfo hashInfo(const Ref& container, const Query& q) const;
    std::shared_ptr<const CollInfo> collInfo(const Ref& container, const Query& q) const;
    ListInfo listInfo(const Ref& container, const Query& q) const;
    std::shared_ptr<const IndexList> listOrder(const Ref& container, const Query& q) const;
    std::shared_ptr<const IndexList> nonEmptyIndex(std::uint64_t offset, std::uint64_t elemSize, std::uint64_t count,
                                                   const Query& q) const;
    // Element indices of a PoV in priority order (highest first), at most `upTo` of them.
    std::vector<std::uint32_t> povWalk(const Ref& container, const CollInfo& ci, const PovRec& pov, std::uint64_t upTo,
                                       const Query& q) const;
    std::shared_ptr<const IndexList> povOrder(const Ref& container, const CollInfo& ci, const PovRec& pov,
                                              const Query& q) const;
    ContainerStats containerStats(const Ref& r, const Query& q) const;
    bool slotLive(const Ref& container, std::uint64_t flagsOff, std::uint64_t slot) const;
    bool listNodeOccupied(const ListLayout& l, std::uint64_t base, std::uint64_t node) const;

    // ---- tables (tables.cpp)
    TableInfo describeTable(const NodeId& id, const std::string& view, const Query& q) const;
    TablePage tableRows(const TableRequest& req, const Query& q) const;

    // ---- ledgers (ledger.cpp)
    bool rootIsLedger() const { return idx.x(rootType).cls == Cls::Ledger; }
    const LedgerLayout& rootLedger() const { return idx.x(rootType).ledger; } // only when rootIsLedger()
    // The cached bulk pass over the table `c` (a Ref of class Ledger).
    std::shared_ptr<const LedgerScan> ledgerScan(const Ref& c, const Query& q) const;
    static bool ledgerRecordLive(const LedgerLayout& l, const std::uint8_t* rec, std::size_t avail);
    static unsigned assetType(const LedgerLayout& l, const std::uint8_t* rec, std::size_t avail); // 0 when unreadable
    // Logical children of a universe record: the members of the alternative selected by `type` ("a:<name>" ids).
    std::vector<Ref> assetLogicalChildren(const Ref& rec) const;
    Ref assetAltMember(const Ref& rec, std::string_view name) const; // throws NotFoundError
    // One-line previews with the asset name resolved through the other slots (small direct reads, no scan).
    std::string entityPreview(const std::uint8_t* rec, std::size_t avail) const;
    std::string assetPreview(const Ref& table, const std::uint8_t* rec, std::size_t avail) const;
    // Asset name a record refers to, resolved by reading the referenced slots of `table` ("" when unresolved).
    std::string assetNameByReads(const Ref& table, unsigned type, std::uint64_t index) const;
    LedgerSummary ledgerSummary(const Query& q) const;
    LedgerLookup ledgerLookup(std::string_view identity, const Query& q) const;
    // Parses a 60-letter identity or a 64-hex key (InvalidArgumentError otherwise).
    void parseKey(std::string_view text, std::uint8_t* key32) const;

    // ---- helpers
    std::string keySummary(TypeId keyType, std::uint64_t keyOffset, std::uint64_t keySize, bool assetHint) const;
    std::string previewAt(TypeId t, std::uint64_t offset, std::uint64_t size, bool assetHint) const;
    LeafValue leafAt(TypeId t, std::uint64_t offset, std::uint32_t bitOff, std::uint32_t bitW, bool assetHint) const;
};

std::string joinId(const NodeId& parent, const std::string& seg);

} // namespace qstate::decode
