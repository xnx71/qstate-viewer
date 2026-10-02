// Table views: columns derived from the schema, native filtering / sorting / paging.
#include <cctype>
#include <charconv>
#include <cmath>
#include <numeric>

#include "gather.h"
#include "impl.h"

namespace qstate::decode {

using Impl = StateDecoder::Impl;

namespace {

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128t;

constexpr std::size_t kMaxColumns = 32;
constexpr int kMaxDepth = 3;

enum class CK : std::uint8_t { Int, Bool, Char, Enum, Float, Id, U128, DateTime, Bytes, Ptr, Composite, Index, Ordinal, Pov };
enum class TK : std::uint8_t { Array, HashMap, HashSet, CollElems, CollPovs, List };
enum class Op : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge, Contains, Zero, NonZero };

struct Column {
    TableColumn info;
    CK ck = CK::Composite;
    TypeId type = kNoType;
    std::uint64_t rel = 0, size = 0;
    std::uint32_t bitOff = 0, bitW = 0, bits = 0;
    bool isSigned = false;
    bool assetHint = false;
};

struct RowSet {
    std::uint64_t n = 0;
    const std::vector<std::uint32_t>* ids = nullptr; // null = dense 0..n-1
    std::shared_ptr<const void> keep;
    std::vector<std::uint32_t> own;
    std::uint64_t at(std::uint64_t i) const { return ids ? (*ids)[static_cast<std::size_t>(i)] : i; }
};

struct RowCtx {
    std::uint64_t index = 0;
    std::uint64_t ordinal = 0;
    const std::uint8_t* data = nullptr;
    std::size_t avail = 0;
};

struct TableResult {
    std::vector<std::uint32_t> ids;
};

std::uint64_t loadLeN(const std::uint8_t* p, std::size_t n) { return fmt::loadLe(p, n); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseI128(const std::string& str, i128& out) {
    std::string s = str;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    unsigned base = 10;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= s.size()) return false;
    u128t v = 0;
    for (; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && std::isxdigit(c)) d = static_cast<unsigned>(std::tolower(c) - 'a' + 10);
        else return false;
        if (d >= base) return false;
        if (v > (~u128t{0} - d) / base) return false;
        v = v * base + d;
    }
    if (v > (static_cast<u128t>(1) << 126)) return false;
    out = neg ? -static_cast<i128>(v) : static_cast<i128>(v);
    return true;
}

const char* ckKind(CK ck) {
    switch (ck) {
    case CK::Int: case CK::Index: case CK::Ordinal: return "int";
    case CK::Bool: return "bool";
    case CK::Char: return "char";
    case CK::Enum: return "enum";
    case CK::Float: return "float";
    case CK::Id: case CK::Pov: return "id";
    case CK::U128: return "u128";
    case CK::DateTime: return "datetime";
    case CK::Bytes: return "bytes";
    case CK::Ptr: return "ptr";
    case CK::Composite: return "composite";
    }
    return "composite";
}

std::size_t keyWords(const Column& c) {
    switch (c.ck) {
    case CK::U128: case CK::Bytes: return 2;
    case CK::Id: case CK::Pov: return 4;
    default: return 1;
    }
}

} // namespace

// ============================================================================================================
// TableEngine
// ============================================================================================================

namespace {

class TableEngine {
public:
    TableEngine(const Impl& impl, const Query& q) : I(impl), q_(q) {}

    // ---- model
    TK tk = TK::Array;
    Ref container;
    std::string view;
    std::vector<TableView> views;
    std::vector<Column> cols;
    std::uint64_t rowsOff = 0, rowSize = 0;
    std::uint64_t totalRows = 0;
    std::optional<ContainerStats> stats;
    TypeId elementType = kNoType;
    // data owners
    std::shared_ptr<const Flags2Scan> flags;
    std::shared_ptr<const CollInfo> ci;
    std::shared_ptr<const IndexList> listIds;
    std::vector<std::uint32_t> povSlots;
    std::uint64_t arrayCount = 0;
    std::string rowSeg; // "i" / "e" / "p"
    bool hideEmpty = false;

    void build(const NodeId& id, const std::string& wantedView);
    void setHideEmpty(bool h) { hideEmpty = h; }
    RowSet rows() const;
    TablePage page(const TableRequest& req);
    TableInfo info() const;

private:
    const Impl& I;
    const Query& q_;

    void addColumn(Column c, const std::string& id, const std::string& label, const char* group) {
        c.info.id = id;
        c.info.label = label;
        c.info.group = group;
        c.info.kind = ckKind(c.ck);
        c.info.sortable = c.ck != CK::Composite;
        c.info.filterable = c.ck != CK::Composite;
        cols.push_back(std::move(c));
    }
    void flatten(std::vector<Column>& out, const std::string& id, const std::string& label, const char* group,
                 TypeId t, std::uint64_t rel, std::uint64_t size, int depth, bool assetHint, std::uint32_t bitOff,
                 std::uint32_t bitW) const;
    void typedColumns(const std::string& prefix, const char* group, TypeId t, std::uint64_t rel, std::uint64_t size);
    void metaInt(const std::string& id, const std::string& label, std::uint64_t rel, bool isSigned, TypeId rec,
                 const char* member);

    // ---- cells / keys / filters
public:
    LeafValue cell(const Column& c, const RowCtx& r) const;
    void sortKey(const Column& c, const RowCtx& r, std::uint64_t* out) const;
    struct Filter {
        std::size_t col = 0;
        Op op = Op::Eq;
        i128 num = 0;
        bool numOk = false;
        double flt = 0;
        std::uint8_t id[32] = {};
        bool idOk = false;
        u128t u = 0;
        std::string text;
        bool boolean = false;
    };
    Filter compileFilter(const FilterSpec& f) const;
    bool evalFilter(const Filter& f, const RowCtx& r) const;
    std::size_t columnIndex(const std::string& id) const {
        for (std::size_t i = 0; i < cols.size(); ++i)
            if (cols[i].info.id == id) return i;
        throw InvalidArgumentError("unknown column: " + id);
    }
    bool numericOf(const Column& c, const RowCtx& r, i128& out) const;
    std::string cellText(const Column& c, const RowCtx& r) const;
    bool cellZero(const Column& c, const RowCtx& r, bool& known) const;
    std::uint64_t rowAbsOffset(std::uint64_t rowIdx) const { return rowsOff + rowIdx * rowSize; }
    NodeId rowId(std::uint64_t rowIdx) const { return joinId(container.id, rowSeg + ":" + std::to_string(rowIdx)); }
};

void TableEngine::flatten(std::vector<Column>& out, const std::string& id, const std::string& label, const char* group,
                          TypeId t, std::uint64_t rel, std::uint64_t size, int depth, bool assetHint,
                          std::uint32_t bitOff, std::uint32_t bitW) const {
    auto base = [&](CK ck) {
        Column c;
        c.ck = ck;
        c.type = t;
        c.rel = rel;
        c.size = size;
        c.bitOff = bitOff;
        c.bitW = bitW;
        c.assetHint = assetHint;
        c.info.id = id;
        c.info.label = label;
        c.info.group = group;
        c.info.typeName = I.typeNameOf(t);
        c.info.kind = ckKind(ck);
        c.info.sortable = ck != CK::Composite;
        c.info.filterable = ck != CK::Composite;
        return c;
    };
    if (!I.idx.valid(t)) return;
    const TypeX& tx = I.idx.x(t);
    switch (tx.cls) {
    case Cls::Int: case Cls::Enum: {
        Column c = base(tx.cls == Cls::Int ? CK::Int : CK::Enum);
        c.bits = bitW ? bitW : tx.intBits;
        c.isSigned = tx.intSigned;
        out.push_back(std::move(c));
        return;
    }
    case Cls::Char: {
        Column c = base(CK::Char);
        c.bits = 8;
        out.push_back(std::move(c));
        return;
    }
    case Cls::Bool: case Cls::Bit: out.push_back(base(CK::Bool)); return;
    case Cls::Float: out.push_back(base(CK::Float)); return;
    case Cls::Ptr: out.push_back(base(CK::Ptr)); return;
    case Cls::Id: out.push_back(base(CK::Id)); return;
    case Cls::U128: out.push_back(base(CK::U128)); return;
    case Cls::DateTime: out.push_back(base(CK::DateTime)); return;
    case Cls::Opaque: out.push_back(base(CK::Bytes)); return;
    case Cls::CArray:
    case Cls::ArrayRole:
        out.push_back(base(tx.byteElems ? CK::Bytes : CK::Composite));
        return;
    case Cls::Record:
        if (depth < kMaxDepth && !tx.members.empty()) {
            for (const Member& m : tx.members) {
                const std::string sub = id + "." + m.name;
                flatten(out, sub, label.empty() ? m.name : label + "." + m.name, group, m.type, rel + m.offset, m.size,
                        depth + 1, m.assetNameHint, m.bitOffset, m.bitWidth);
            }
            return;
        }
        out.push_back(base(CK::Composite));
        return;
    default: out.push_back(base(CK::Composite)); return;
    }
}

void TableEngine::typedColumns(const std::string& prefix, const char* group, TypeId t, std::uint64_t rel,
                               std::uint64_t size) {
    for (int depth = kMaxDepth; depth >= 0; --depth) {
        std::vector<Column> tmp;
        // depth parameter counts already used levels: start at (kMaxDepth - allowed)
        flatten(tmp, prefix, "", group, t, rel, size, kMaxDepth - depth, false, 0, 0);
        if (tmp.size() + cols.size() <= kMaxColumns || depth == 0) {
            for (Column& c : tmp) {
                // a leaf type gets the group name as its label
                if (c.info.label.empty()) c.info.label = prefix;
                cols.push_back(std::move(c));
            }
            return;
        }
    }
}

void TableEngine::metaInt(const std::string& id, const std::string& label, std::uint64_t rel, bool isSigned, TypeId rec,
                          const char* member) {
    Column c;
    c.ck = CK::Int;
    if (const Member* m = I.findMember(rec, member)) c.type = m->type;
    c.rel = rel;
    c.size = 8;
    c.bits = 64;
    c.isSigned = isSigned;
    c.info.typeName = isSigned ? "sint64" : "uint64";
    addColumn(std::move(c), id, label, "meta");
}

void TableEngine::build(const NodeId& id, const std::string& wantedView) {
    container = I.resolve(id, q_);
    if (container.rk != RK::Plain) throw NotFoundError("node is not a table: " + id);
    const TypeX& tx = I.idx.x(container.type);
    cols.clear();
    auto metaIndex = [&](const char* cid, const char* label) {
        Column c;
        c.ck = CK::Index;
        c.info.typeName = "index";
        addColumn(std::move(c), cid, label, "meta");
    };

    switch (tx.cls) {
    case Cls::CArray:
    case Cls::ArrayRole: {
        tk = TK::Array;
        views = {{"elements", "Elements"}};
        view = "elements";
        if (!wantedView.empty() && wantedView != view) throw NotFoundError("unknown view: " + wantedView);
        rowsOff = container.offset;
        rowSize = tx.elemSize;
        arrayCount = tx.count;
        totalRows = tx.count;
        elementType = tx.elem;
        rowSeg = "i";
        metaIndex("$index", "#");
        typedColumns("value", "value", tx.elem, 0, tx.elemSize);
        break;
    }
    case Cls::HashMap:
    case Cls::HashSet: {
        const HashLayout& h = tx.hash;
        tk = h.isMap ? TK::HashMap : TK::HashSet;
        views = {{"entries", "Entries"}};
        view = "entries";
        if (!wantedView.empty() && wantedView != view) throw NotFoundError("unknown view: " + wantedView);
        const HashInfo hi = I.hashInfo(container, q_);
        flags = hi.flags;
        stats = hi.stats;
        totalRows = flags->live.size();
        rowsOff = container.offset + h.elemsOff;
        rowSize = h.elemSize;
        elementType = h.isMap ? h.elementType : h.keyType;
        rowSeg = "e";
        metaIndex("$index", "slot");
        typedColumns("key", "key", h.keyType, h.keyOff, h.keySize);
        if (h.isMap) typedColumns("value", "value", h.valueType, h.valueOff, h.valueSize);
        break;
    }
    case Cls::Collection: {
        const CollectionLayout& l = tx.coll;
        views = {{"elements", "Elements"}, {"povs", "PoVs"}};
        view = wantedView.empty() ? "elements" : wantedView;
        ci = I.collInfo(container, q_);
        stats = ci->stats;
        if (view == "elements") {
            tk = TK::CollElems;
            totalRows = ci->elemCount;
            rowsOff = container.offset + l.elemsOff;
            rowSize = l.elemSize;
            elementType = l.valueType;
            rowSeg = "e";
            metaIndex("$index", "#");
            {
                Column c;
                c.ck = CK::Pov;
                c.rel = l.povIdxOff;
                c.size = 8;
                c.info.typeName = "id";
                addColumn(std::move(c), "$pov", "PoV", "meta");
            }
            metaInt("$priority", "priority", l.prioOff, true, l.elementType, "priority");
            typedColumns("value", "value", l.valueType, l.valueOff, l.valueSize);
        } else if (view == "povs") {
            tk = TK::CollPovs;
            totalRows = ci->povs.size();
            povSlots.clear();
            povSlots.reserve(ci->povs.size());
            for (const PovRec& p : ci->povs) povSlots.push_back(p.slot);
            rowsOff = container.offset + l.povsOff;
            rowSize = l.povSize;
            elementType = l.povType;
            rowSeg = "p";
            metaIndex("$index", "slot");
            {
                Column c;
                c.ck = CK::Id;
                c.type = I.idx.valid(l.povType) ? [&] { const Member* m = I.findMember(l.povType, "value"); return m ? m->type : kNoType; }() : kNoType;
                c.rel = l.povValueOff;
                c.size = 32;
                c.info.typeName = "id";
                addColumn(std::move(c), "$pov", "PoV", "meta");
            }
            metaInt("$population", "population", l.povPopOff, false, l.povType, "population");
            metaInt("$head", "head", l.povHeadOff, true, l.povType, "headIndex");
            metaInt("$tail", "tail", l.povTailOff, true, l.povType, "tailIndex");
            metaInt("$root", "BST root", l.povRootOff, true, l.povType, "bstRootIndex");
        } else {
            throw NotFoundError("unknown view: " + wantedView);
        }
        break;
    }
    case Cls::LinkedList: {
        const ListLayout& l = tx.list;
        tk = TK::List;
        views = {{"elements", "Elements"}};
        view = "elements";
        if (!wantedView.empty() && wantedView != view) throw NotFoundError("unknown view: " + wantedView);
        const ListInfo li = I.listInfo(container, q_);
        stats = li.stats;
        listIds = I.listOrder(container, q_);
        totalRows = listIds->ids.size();
        rowsOff = container.offset + l.nodesOff;
        rowSize = l.nodeSize;
        elementType = l.valueType;
        rowSeg = "e";
        metaIndex("$index", "node");
        {
            Column c;
            c.ck = CK::Ordinal;
            c.info.typeName = "position";
            addColumn(std::move(c), "$position", "position", "meta");
        }
        typedColumns("value", "value", l.valueType, l.valueOff, l.valueSize);
        break;
    }
    default: throw NotFoundError("node is not tabular: " + id);
    }
}

RowSet TableEngine::rows() const {
    RowSet rs;
    switch (tk) {
    case TK::Array:
        if (hideEmpty) {
            auto ne = I.nonEmptyIndex(rowsOff, rowSize, arrayCount, q_);
            rs.keep = ne;
            rs.ids = &ne->ids;
            rs.n = ne->ids.size();
        } else {
            rs.n = arrayCount;
        }
        break;
    case TK::HashMap:
    case TK::HashSet:
        rs.keep = flags;
        rs.ids = &flags->live;
        rs.n = flags->live.size();
        break;
    case TK::CollElems: rs.n = ci->elemCount; break;
    case TK::CollPovs:
        rs.ids = &povSlots;
        rs.n = povSlots.size();
        break;
    case TK::List:
        rs.keep = listIds;
        rs.ids = &listIds->ids;
        rs.n = listIds->ids.size();
        break;
    }
    return rs;
}

TableInfo TableEngine::info() const {
    TableInfo ti;
    ti.id = container.id;
    ti.views = views;
    ti.view = view;
    for (const Column& c : cols) ti.columns.push_back(c.info);
    ti.totalRows = hideEmpty && tk == TK::Array ? rows().n : totalRows;
    ti.container = stats;
    ti.elementTypeId = elementType;
    return ti;
}

// ---- cells ----------------------------------------------------------------------------------------------------------

LeafValue TableEngine::cell(const Column& c, const RowCtx& r) const {
    auto uintValue = [&](std::uint64_t v) {
        LeafValue lv;
        lv.k = LeafValue::Kind::Int;
        lv.v = std::to_string(v);
        lv.isUnsigned = true;
        lv.bits = 64;
        lv.hex = fmt::hexNumber(v, 64);
        return lv;
    };
    if (c.ck == CK::Index) return uintValue(r.index);
    if (c.ck == CK::Ordinal) return uintValue(r.ordinal);
    if (c.rel + c.size > r.avail) return LeafValue::unavailable("beyond end of file");
    const std::uint8_t* p = r.data + c.rel;
    const std::size_t avail = r.avail - static_cast<std::size_t>(c.rel);
    switch (c.ck) {
    case CK::Pov: {
        const std::uint64_t slot = loadLeN(p, 8);
        if (!ci) return LeafValue::unavailable("no PoV table");
        if (const PovRec* pov = ci->findPov(slot)) return I.fmt.idValue(pov->id);
        return LeafValue::unavailable("PoV slot " + std::to_string(slot) + " is not live");
    }
    case CK::Composite: return LeafValue::composite(I.fmt.preview(c.type, p, avail, c.assetHint));
    case CK::Bytes:
        if (I.idx.valid(c.type) && I.idx.x(c.type).cls == Cls::Opaque) return I.fmt.leaf(c.type, p, avail);
        return I.fmt.bytesValue(p, static_cast<std::size_t>(c.size));
    default: return I.fmt.leaf(c.type, p, avail, c.assetHint, c.bitOff, c.bitW);
    }
}

bool TableEngine::numericOf(const Column& c, const RowCtx& r, i128& out) const {
    switch (c.ck) {
    case CK::Index: out = r.index; return true;
    case CK::Ordinal: out = r.ordinal; return true;
    case CK::Int: case CK::Enum: case CK::Char: case CK::Bool: case CK::Ptr: case CK::DateTime: {
        if (c.rel + c.size > r.avail || c.size > 8) return false;
        std::uint64_t raw = loadLeN(r.data + c.rel, static_cast<std::size_t>(c.size));
        std::uint32_t bits = static_cast<std::uint32_t>(c.size * 8);
        if (c.bitW) {
            raw >>= c.bitOff;
            bits = c.bitW;
            if (bits < 64) raw &= (std::uint64_t{1} << bits) - 1;
        }
        if (c.ck == CK::Bool) {
            out = raw != 0;
            return true;
        }
        if (c.isSigned) {
            std::int64_t sv = static_cast<std::int64_t>(raw);
            if (bits < 64 && ((raw >> (bits - 1)) & 1)) sv = static_cast<std::int64_t>(raw | ~((std::uint64_t{1} << bits) - 1));
            out = sv;
        } else {
            out = raw;
        }
        return true;
    }
    default: return false;
    }
}

void TableEngine::sortKey(const Column& c, const RowCtx& r, std::uint64_t* out) const {
    const std::size_t w = keyWords(c);
    for (std::size_t i = 0; i < w; ++i) out[i] = 0;
    switch (c.ck) {
    case CK::Composite: return;
    case CK::Pov: {
        if (c.rel + 8 > r.avail || !ci) return;
        if (const PovRec* pov = ci->findPov(loadLeN(r.data + c.rel, 8)))
            for (int i = 0; i < 4; ++i) out[i] = loadLeN(pov->id + 8 * (3 - i), 8);
        return;
    }
    case CK::Id:
        if (c.rel + 32 > r.avail) return;
        for (int i = 0; i < 4; ++i) out[i] = loadLeN(r.data + c.rel + 8 * (3 - i), 8);
        return;
    case CK::U128:
        if (c.rel + 16 > r.avail) return;
        out[0] = loadLeN(r.data + c.rel + 8, 8);
        out[1] = loadLeN(r.data + c.rel, 8);
        return;
    case CK::Bytes: {
        if (c.rel >= r.avail) return;
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(c.size), r.avail - static_cast<std::size_t>(c.rel));
        const std::uint8_t* p = r.data + c.rel;
        std::size_t len = 0;
        while (len < n && p[len] != 0) ++len; // text order: up to the first NUL
        for (std::size_t i = 0; i < 16 && i < len; ++i) out[i / 8] |= static_cast<std::uint64_t>(p[i]) << (56 - 8 * (i % 8));
        return;
    }
    case CK::Float: {
        if (c.rel + c.size > r.avail) return;
        double d;
        if (c.size == 4) {
            std::uint32_t u = static_cast<std::uint32_t>(loadLeN(r.data + c.rel, 4));
            float f;
            std::memcpy(&f, &u, 4);
            d = f;
        } else {
            std::uint64_t u = loadLeN(r.data + c.rel, 8);
            std::memcpy(&d, &u, 8);
        }
        std::uint64_t bits;
        std::memcpy(&bits, &d, 8);
        out[0] = (bits >> 63) ? ~bits : (bits | (std::uint64_t{1} << 63));
        return;
    }
    default: {
        i128 v;
        if (!numericOf(c, r, v)) return;
        // order-preserving map of the value onto unsigned 64 bit
        if (c.isSigned) out[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(v)) ^ (std::uint64_t{1} << 63);
        else out[0] = static_cast<std::uint64_t>(v);
        return;
    }
    }
}

std::string TableEngine::cellText(const Column& c, const RowCtx& r) const {
    const LeafValue v = cell(c, r);
    using K = LeafValue::Kind;
    switch (v.k) {
    case K::Id: return v.identity + " " + v.hex;
    case K::Int: return v.v + (v.text ? " " + *v.text : "") + " " + v.hex;
    case K::Enum: return v.v + (v.name ? " " + *v.name : "");
    case K::Bytes: return v.text ? *v.text : v.hex;
    case K::Bool: return v.boolean ? "true" : "false";
    case K::DateTime: return v.text.value_or("");
    default: return I.fmt.shortText(v);
    }
}

bool TableEngine::cellZero(const Column& c, const RowCtx& r, bool& known) const {
    known = true;
    if (c.ck == CK::Index) return r.index == 0;
    if (c.ck == CK::Ordinal) return r.ordinal == 0;
    if (c.ck == CK::Pov) {
        std::uint64_t k[4];
        sortKey(c, r, k);
        return !(k[0] | k[1] | k[2] | k[3]);
    }
    if (c.rel + c.size > r.avail) {
        known = false;
        return false;
    }
    const std::uint8_t* p = r.data + c.rel;
    if (c.bitW) {
        i128 v;
        if (!numericOf(c, r, v)) {
            known = false;
            return false;
        }
        return v == 0;
    }
    for (std::uint64_t i = 0; i < c.size; ++i)
        if (p[i]) return false;
    return true;
}

TableEngine::Filter TableEngine::compileFilter(const FilterSpec& f) const {
    Filter out;
    out.col = columnIndex(f.column);
    const Column& c = cols[out.col];
    static const std::pair<const char*, Op> ops[] = {{"eq", Op::Eq}, {"ne", Op::Ne}, {"lt", Op::Lt}, {"le", Op::Le},
                                                      {"gt", Op::Gt}, {"ge", Op::Ge}, {"contains", Op::Contains},
                                                      {"zero", Op::Zero}, {"nonzero", Op::NonZero}};
    bool found = false;
    for (const auto& [name, op] : ops)
        if (f.op == name) {
            out.op = op;
            found = true;
        }
    if (!found) throw InvalidArgumentError("unknown filter operator: " + f.op);
    if (out.op == Op::Zero || out.op == Op::NonZero) return out;
    if (!f.value) throw InvalidArgumentError("filter '" + f.op + "' on column " + f.column + " needs a value");
    const std::string& value = *f.value;
    if (c.ck == CK::Composite) throw InvalidArgumentError("column " + f.column + " cannot be filtered");
    out.text = value;
    if (out.op == Op::Contains) {
        if (c.ck == CK::Id || c.ck == CK::Pov) {
            bool hexLike = false;
            for (char ch : value)
                if (!(ch >= 'A' && ch <= 'Z')) hexLike = true;
            out.text = hexLike ? lower(value) : value;
            out.boolean = hexLike; // reused: hex match
        } else {
            out.text = lower(value);
        }
        return out;
    }
    switch (c.ck) {
    case CK::Int: case CK::Index: case CK::Ordinal: case CK::Ptr: {
        if (parseI128(value, out.num)) {
            out.numOk = true;
        } else if (c.assetHint) {
            if (auto a = fmt::assetNameValue(value)) {
                out.num = *a;
                out.numOk = true;
            }
        }
        if (!out.numOk) throw InvalidArgumentError("not an integer: " + value);
        break;
    }
    case CK::Enum: {
        if (parseI128(value, out.num)) {
            out.numOk = true;
        } else if (I.idx.valid(c.type)) {
            for (const schema::Enumerator& e : I.idx.type(c.type).enumerators)
                if (e.name == value) {
                    out.num = e.value;
                    out.numOk = true;
                }
        }
        if (!out.numOk) throw InvalidArgumentError("unknown enumerator or number: " + value);
        break;
    }
    case CK::Char: {
        if (parseI128(value, out.num)) out.numOk = true;
        else if (value.size() == 1) {
            out.num = static_cast<unsigned char>(value[0]);
            out.numOk = true;
        } else throw InvalidArgumentError("not a character: " + value);
        break;
    }
    case CK::Bool: {
        const std::string v = lower(value);
        if (v == "true" || v == "1") out.num = 1;
        else if (v == "false" || v == "0") out.num = 0;
        else throw InvalidArgumentError("not a boolean: " + value);
        out.numOk = true;
        break;
    }
    case CK::Float: {
        char* end = nullptr;
        out.flt = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != 0) throw InvalidArgumentError("not a number: " + value);
        break;
    }
    case CK::DateTime: {
        if (parseI128(value, out.num)) {
            out.numOk = true;
            break;
        }
        // YYYY-MM-DD[ hh:mm:ss[.mmm['uuu]]]
        unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0, us = 0;
        const int n = std::sscanf(value.c_str(), "%u-%u-%u %u:%u:%u.%u'%u", &y, &mo, &d, &h, &mi, &s, &ms, &us);
        if (n < 3) throw InvalidArgumentError("not a date / raw value: " + value);
        out.num = static_cast<i128>((std::uint64_t{y} << 46) | (std::uint64_t{mo} << 42) | (std::uint64_t{d} << 37) |
                                    (std::uint64_t{h} << 32) | (std::uint64_t{mi} << 26) | (std::uint64_t{s} << 20) |
                                    (std::uint64_t{ms} << 10) | std::uint64_t{us});
        out.numOk = true;
        break;
    }
    case CK::U128: {
        i128 v;
        if (!parseI128(value, v) || v < 0) throw InvalidArgumentError("not an unsigned integer: " + value);
        out.u = static_cast<u128t>(v);
        break;
    }
    case CK::Id: case CK::Pov: {
        std::string err;
        if (value.size() == 60) {
            if (!I.codec->decode(value, out.id, &err)) {
                BasicIdentityCodec lax;
                if (!lax.decode(value, out.id, &err)) throw InvalidArgumentError(err);
            }
            out.idOk = true;
        } else if (value.size() == 64) {
            for (int i = 0; i < 32; ++i) {
                const char a = value[static_cast<std::size_t>(2 * i)], b = value[static_cast<std::size_t>(2 * i + 1)];
                if (!std::isxdigit(static_cast<unsigned char>(a)) || !std::isxdigit(static_cast<unsigned char>(b)))
                    throw InvalidArgumentError("not a hex id: " + value);
                auto hv = [](char ch) { return ch <= '9' ? ch - '0' : std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10; };
                out.id[i] = static_cast<std::uint8_t>(hv(a) * 16 + hv(b));
            }
            out.idOk = true;
        } else {
            throw InvalidArgumentError("an id filter needs a 60-letter identity or 64 hex digits");
        }
        break;
    }
    case CK::Bytes: break; // text compare
    case CK::Composite: break;
    }
    return out;
}

bool TableEngine::evalFilter(const Filter& f, const RowCtx& r) const {
    const Column& c = cols[f.col];
    if (f.op == Op::Zero || f.op == Op::NonZero) {
        bool known = false;
        const bool z = cellZero(c, r, known);
        if (!known) return false;
        return f.op == Op::Zero ? z : !z;
    }
    if (f.op == Op::Contains) {
        if (c.ck == CK::Id || c.ck == CK::Pov) {
            const LeafValue v = cell(c, r);
            if (v.k != LeafValue::Kind::Id) return false;
            return (f.boolean ? v.hex : v.identity).find(f.text) != std::string::npos;
        }
        return lower(cellText(c, r)).find(f.text) != std::string::npos;
    }
    auto cmpRes = [&](int cmp) {
        switch (f.op) {
        case Op::Eq: return cmp == 0;
        case Op::Ne: return cmp != 0;
        case Op::Lt: return cmp < 0;
        case Op::Le: return cmp <= 0;
        case Op::Gt: return cmp > 0;
        case Op::Ge: return cmp >= 0;
        default: return false;
        }
    };
    switch (c.ck) {
    case CK::Int: case CK::Index: case CK::Ordinal: case CK::Ptr: case CK::Enum: case CK::Char: case CK::Bool:
    case CK::DateTime: {
        i128 v;
        if (!numericOf(c, r, v)) return false;
        return cmpRes(v < f.num ? -1 : (v > f.num ? 1 : 0));
    }
    case CK::Float: {
        if (c.rel + c.size > r.avail) return false;
        double d;
        if (c.size == 4) {
            std::uint32_t u = static_cast<std::uint32_t>(loadLeN(r.data + c.rel, 4));
            float fl;
            std::memcpy(&fl, &u, 4);
            d = fl;
        } else {
            std::uint64_t u = loadLeN(r.data + c.rel, 8);
            std::memcpy(&d, &u, 8);
        }
        if (std::isnan(d) || std::isnan(f.flt)) return f.op == Op::Ne;
        return cmpRes(d < f.flt ? -1 : (d > f.flt ? 1 : 0));
    }
    case CK::U128: {
        if (c.rel + 16 > r.avail) return false;
        const u128t v = (static_cast<u128t>(loadLeN(r.data + c.rel + 8, 8)) << 64) | loadLeN(r.data + c.rel, 8);
        return cmpRes(v < f.u ? -1 : (v > f.u ? 1 : 0));
    }
    case CK::Id: case CK::Pov: {
        std::uint64_t k[4];
        sortKey(c, r, k);
        std::uint64_t want[4];
        for (int i = 0; i < 4; ++i) want[i] = loadLeN(f.id + 8 * (3 - i), 8);
        int cmp = 0;
        for (int i = 0; i < 4 && cmp == 0; ++i) cmp = k[i] < want[i] ? -1 : (k[i] > want[i] ? 1 : 0);
        return cmpRes(cmp);
    }
    case CK::Bytes: {
        if (c.rel + c.size > r.avail) return false;
        const std::uint8_t* p = r.data + c.rel;
        std::size_t len = 0;
        while (len < c.size && p[len] != 0) ++len;
        const std::string s(reinterpret_cast<const char*>(p), len);
        const int cmp = s.compare(f.text);
        return cmpRes(cmp < 0 ? -1 : (cmp > 0 ? 1 : 0));
    }
    case CK::Composite: return false;
    }
    return false;
}

// ---- paging ---------------------------------------------------------------------------------------------------------

TablePage TableEngine::page(const TableRequest& req) {
    const auto t0 = std::chrono::steady_clock::now();
    TablePage out;
    out.offset = req.offset;
    const std::uint64_t limit = std::max<std::uint64_t>(1, std::min<std::uint64_t>(req.limit ? req.limit : 100, 1000));

    const RowSet all = rows();
    std::vector<Filter> filters;
    for (const FilterSpec& f : req.filters) filters.push_back(compileFilter(f));
    struct SortCol {
        std::size_t col;
        bool desc;
    };
    std::vector<SortCol> sorts;
    for (const SortSpec& s : req.sort) {
        const std::size_t ci2 = columnIndex(s.column);
        if (cols[ci2].ck == CK::Composite) throw InvalidArgumentError("column " + s.column + " cannot be sorted");
        sorts.push_back({ci2, s.desc});
    }

    // the sequence of row ids in output order
    const std::vector<std::uint32_t>* seqIds = nullptr;
    std::shared_ptr<const TableResult> result;
    std::uint64_t total = all.n;

    const bool needPass = !filters.empty() || !sorts.empty();
    // For lists the ordinal of a row = its position in list order.
    std::vector<std::uint32_t> listPos;
    if (tk == TK::List) {
        // listPos[node] = position; sized by the largest node id seen
        std::uint32_t maxNode = 0;
        for (std::uint64_t i = 0; i < all.n; ++i) maxNode = std::max<std::uint32_t>(maxNode, static_cast<std::uint32_t>(all.at(i)));
        listPos.assign(all.n ? static_cast<std::size_t>(maxNode) + 1 : 0, 0);
        for (std::uint64_t i = 0; i < all.n; ++i) listPos[static_cast<std::size_t>(all.at(i))] = static_cast<std::uint32_t>(i);
    }
    auto ordinalOf = [&](std::uint64_t rowIdx) -> std::uint64_t {
        return tk == TK::List && rowIdx < listPos.size() ? listPos[static_cast<std::size_t>(rowIdx)] : 0;
    };

    if (needPass) {
        std::string k = "tr:" + std::to_string(container.offset) + ":" + std::to_string(container.type) + ":" + view +
                        (hideEmpty ? ":h" : ":-");
        for (const SortCol& s : sorts) k += "|s:" + cols[s.col].info.id + (s.desc ? "-" : "+");
        for (const FilterSpec& f : req.filters) k += "|f:" + f.column + ":" + f.op + ":" + f.value.value_or("") ;
        k = I.key(q_, k);
        result = I.cache->getAs<TableResult>(k);
        if (!result) {
            auto res = std::make_shared<TableResult>();
            std::size_t stride = 0;
            std::vector<std::size_t> keyBase;
            for (const SortCol& s : sorts) {
                keyBase.push_back(stride);
                stride += keyWords(cols[s.col]);
            }
            std::vector<std::uint64_t> keys;
            gatherRows(
                *I.src, all.n, [&](std::uint64_t i) { return rowAbsOffset(all.at(i)); },
                static_cast<std::size_t>(rowSize),
                [&](std::uint64_t i, const std::uint8_t* d, std::size_t avail) {
                    if (avail == 0) return; // row lies beyond the end of the file
                    RowCtx rc{all.at(i), tk == TK::List ? i : 0, d, avail};
                    for (const Filter& f : filters)
                        if (!evalFilter(f, rc)) return;
                    res->ids.push_back(static_cast<std::uint32_t>(rc.index));
                    if (stride) {
                        const std::size_t at = keys.size();
                        keys.resize(at + stride);
                        for (std::size_t s = 0; s < sorts.size(); ++s) sortKey(cols[sorts[s].col], rc, keys.data() + at + keyBase[s]);
                    }
                },
                q_.cancel);
            if (stride) {
                std::vector<std::uint32_t> order(res->ids.size());
                std::iota(order.begin(), order.end(), 0u);
                auto cmp = [&](std::uint32_t a, std::uint32_t b) {
                    for (std::size_t s = 0; s < sorts.size(); ++s) {
                        const std::size_t w = keyWords(cols[sorts[s].col]);
                        const std::uint64_t* ka = keys.data() + std::size_t{a} * stride + keyBase[s];
                        const std::uint64_t* kb = keys.data() + std::size_t{b} * stride + keyBase[s];
                        for (std::size_t j = 0; j < w; ++j) {
                            if (ka[j] == kb[j]) continue;
                            return sorts[s].desc ? ka[j] > kb[j] : ka[j] < kb[j];
                        }
                    }
                    return a < b; // stable: original row order
                };
                std::sort(order.begin(), order.end(), cmp);
                std::vector<std::uint32_t> sorted(order.size());
                for (std::size_t i = 0; i < order.size(); ++i) sorted[i] = res->ids[order[i]];
                res->ids.swap(sorted);
            }
            res->ids.shrink_to_fit();
            I.cache->put(k, res, sizeof(TableResult) + res->ids.capacity() * 4);
            result = res;
        }
        seqIds = &result->ids;
        total = result->ids.size();
    }
    out.total = total;

    // page window
    std::vector<std::uint64_t> pageRows;
    if (req.offset < total) {
        const std::uint64_t end = std::min(total, req.offset + limit);
        for (std::uint64_t i = req.offset; i < end; ++i)
            pageRows.push_back(seqIds ? (*seqIds)[static_cast<std::size_t>(i)] : all.at(i));
    }
    gatherRows(
        *I.src, pageRows.size(), [&](std::uint64_t i) { return rowAbsOffset(pageRows[static_cast<std::size_t>(i)]); },
        static_cast<std::size_t>(rowSize),
        [&](std::uint64_t i, const std::uint8_t* d, std::size_t avail) {
            const std::uint64_t rowIdx = pageRows[static_cast<std::size_t>(i)];
            RowCtx rc{rowIdx, ordinalOf(rowIdx), d, avail};
            TableRow row;
            row.index = rowIdx;
            row.id = rowId(rowIdx);
            row.cells.reserve(cols.size());
            for (const Column& c : cols) row.cells.push_back(cell(c, rc));
            out.rows.push_back(std::move(row));
        },
        q_.cancel);
    // gatherRows visits rows in request order, so out.rows is already in output order
    out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

} // namespace

TableInfo Impl::describeTable(const NodeId& id, const std::string& view, const Query& q) const {
    TableEngine t(*this, q);
    t.build(id, view);
    return t.info();
}

TablePage Impl::tableRows(const TableRequest& req, const Query& q) const {
    TableEngine t(*this, q);
    t.build(req.id, req.view);
    t.setHideEmpty(req.hideEmpty);
    return t.page(req);
}

} // namespace qstate::decode
