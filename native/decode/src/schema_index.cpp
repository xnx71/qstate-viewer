#include "qstate/decode/schema_index.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace qstate::decode {

using schema::Field;
using schema::kNoType;
using schema::PrimKind;
using schema::RoleKind;
using schema::Type;
using schema::TypeId;
using schema::TypeKind;

bool assetNameLike(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto ends = [&](const char* suffix) {
        const std::size_t n = std::char_traits<char>::length(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    };
    return ends("assetname") || ends("asset") || ends("name");
}

namespace {

constexpr std::uint64_t kMaxCapacity = 0xFFFFFFFEull; // row indexes are 32 bit

const Field* findField(const Type& t, const char* name) {
    for (const Field& f : t.fields)
        if (f.name == name) return &f;
    return nullptr;
}

} // namespace

SchemaIndex::SchemaIndex(std::shared_ptr<const schema::Schema> schema) : schema_(std::move(schema)) {
    if (!schema_) throw std::invalid_argument("SchemaIndex: null schema");
    types_.resize(schema_->types.size());
    for (TypeId id = 0; id < schema_->types.size(); ++id) buildType(id);
}

const TypeX& SchemaIndex::x(TypeId id) const {
    if (id >= types_.size()) throw std::out_of_range("invalid type id");
    return types_[id];
}

void SchemaIndex::buildType(TypeId id) {
    const Type& t = schema_->types[id];
    TypeX& tx = types_[id];
    tx.size = t.size;
    switch (t.kind) {
    case TypeKind::Prim:
        switch (t.prim) {
        case PrimKind::Bool: tx.cls = t.size == 1 ? Cls::Bool : Cls::Opaque; break;
        case PrimKind::Char:
            tx.cls = t.size == 1 ? Cls::Char : Cls::Opaque;
            tx.intBits = 8;
            tx.intSigned = t.isSigned;
            break;
        case PrimKind::SInt:
        case PrimKind::UInt:
            tx.cls = (t.size == 1 || t.size == 2 || t.size == 4 || t.size == 8) ? Cls::Int : Cls::Opaque;
            tx.intBits = static_cast<std::uint32_t>(t.size * 8);
            tx.intSigned = t.prim == PrimKind::SInt || (t.prim == PrimKind::Char && t.isSigned);
            break;
        case PrimKind::Float: tx.cls = (t.size == 4 || t.size == 8) ? Cls::Float : Cls::Opaque; break;
        }
        return;
    case TypeKind::Enum:
        tx.cls = (t.size == 1 || t.size == 2 || t.size == 4 || t.size == 8) ? Cls::Enum : Cls::Opaque;
        tx.intBits = static_cast<std::uint32_t>(t.size * 8);
        if (t.underlying != kNoType && t.underlying < schema_->types.size())
            tx.intSigned = schema_->types[t.underlying].isSigned || schema_->types[t.underlying].prim == PrimKind::SInt;
        return;
    case TypeKind::Pointer: tx.cls = t.size == 8 ? Cls::Ptr : Cls::Opaque; return;
    case TypeKind::Array: {
        tx.cls = Cls::CArray;
        tx.elem = t.element;
        tx.count = t.count;
        if (t.element >= schema_->types.size()) {
            tx.cls = Cls::Opaque;
            return;
        }
        const Type& e = schema_->types[t.element];
        tx.elemSize = e.size;
        tx.byteElems = e.size == 1 && (e.kind == TypeKind::Prim && e.prim != PrimKind::Bool && e.prim != PrimKind::Float);
        if (tx.count > kMaxCapacity || (tx.elemSize != 0 && tx.count > 0xFFFFFFFFFFFFull / tx.elemSize)) tx.cls = Cls::Opaque;
        return;
    }
    case TypeKind::Record: break;
    }

    // Record
    tx.cls = t.recordKind == schema::RecordKind::Union ? Cls::Union : Cls::Record;
    buildMembers(id, tx);
    switch (t.role.kind) {
    case RoleKind::None: return;
    case RoleKind::Id: tx.cls = t.size == 32 ? Cls::Id : tx.cls; return;
    case RoleKind::Bit: tx.cls = t.size == 1 ? Cls::Bit : tx.cls; return;
    case RoleKind::Uint128: tx.cls = t.size == 16 ? Cls::U128 : tx.cls; return;
    case RoleKind::DateTime: tx.cls = t.size == 8 ? Cls::DateTime : tx.cls; return;
    default: break;
    }
    buildContainer(id, tx);
}

void SchemaIndex::buildMembers(TypeId id, TypeX& tx) {
    // Base members first (recursively, MSVC placement already resolved by the layout engine), then own fields.
    struct Walk {
        const schema::Schema& s;
        std::vector<Member>& out;
        void run(TypeId tid, std::uint64_t baseOffset, int depth) {
            if (depth > 16 || tid >= s.types.size()) return;
            const Type& t = s.types[tid];
            for (const schema::BaseClass& b : t.bases) run(b.type, baseOffset + b.offset, depth + 1);
            for (const Field& f : t.fields) {
                Member m;
                m.name = f.name;
                m.type = f.type;
                m.offset = baseOffset + f.offset;
                m.size = f.size;
                m.bitOffset = f.bitOffset;
                m.bitWidth = f.bitWidth;
                m.assetNameHint = assetNameLike(f.name);
                out.push_back(std::move(m));
            }
        }
    } walk{*schema_, tx.members};
    walk.run(id, 0, 0);
}

void SchemaIndex::buildContainer(TypeId id, TypeX& tx) {
    const Type& t = schema_->types[id];
    const schema::Role& role = t.role;
    auto err = [&](std::string msg) {
        tx.layoutError = std::move(msg);
        // keep Record / Union class: raw members only
    };
    auto arrayField = [&](const char* name, const Field*& f, const Type*& arr) -> bool {
        f = findField(t, name);
        if (!f) {
            err(std::string("missing member ") + name);
            return false;
        }
        if (f->type >= schema_->types.size() || schema_->types[f->type].kind != TypeKind::Array) {
            err(std::string("member ") + name + " is not an array");
            return false;
        }
        arr = &schema_->types[f->type];
        if (arr->element >= schema_->types.size()) {
            err(std::string("member ") + name + " has no element type");
            return false;
        }
        if (f->offset + arr->size > t.size) {
            err(std::string("member ") + name + " exceeds the container size");
            return false;
        }
        return true;
    };
    auto u64Field = [&](const char* name, std::uint64_t& off) -> bool {
        const Field* f = findField(t, name);
        if (!f || f->size != 8 || f->offset + 8 > t.size) {
            err(std::string("missing or malformed member ") + name);
            return false;
        }
        off = f->offset;
        return true;
    };
    auto sub = [&](const Type& rec, const char* name, std::uint64_t& off, std::uint64_t* size = nullptr,
                   TypeId* type = nullptr) -> bool {
        const Field* f = findField(rec, name);
        if (!f || f->offset + f->size > rec.size) {
            err(std::string("element record lacks member ") + name);
            return false;
        }
        off = f->offset;
        if (size) *size = f->size;
        if (type) *type = f->type;
        return true;
    };
    auto flagsOk = [&](const Field* f, const Type* arr, std::uint64_t words) -> bool {
        if (arr->count < words || arr->element >= schema_->types.size() ||
            schema_->types[arr->element].size != 8) {
            err(std::string("flag array ") + f->name + " too small");
            return false;
        }
        return true;
    };

    switch (role.kind) {
    case RoleKind::Array: {
        TypeId elem = role.element;
        std::uint64_t cap = role.capacity;
        if (const Field* f = findField(t, "_values")) {
            if (f->type < schema_->types.size() && schema_->types[f->type].kind == TypeKind::Array) {
                elem = schema_->types[f->type].element;
                cap = schema_->types[f->type].count;
            }
        }
        if (elem >= schema_->types.size() || cap == 0 || cap > kMaxCapacity) return err("array without element type / capacity");
        tx.cls = Cls::ArrayRole;
        tx.elem = elem;
        tx.count = cap;
        tx.elemSize = schema_->types[elem].size;
        const Type& e = schema_->types[elem];
        tx.byteElems = e.size == 1 && e.kind == TypeKind::Prim && e.prim != PrimKind::Bool && e.prim != PrimKind::Float;
        if (tx.elemSize * cap > t.size) {
            tx.cls = Cls::Record;
            return err("array elements exceed the record size");
        }
        return;
    }
    case RoleKind::BitArray: {
        std::uint64_t bits = role.capacity;
        if (const Field* f = findField(t, "_values")) {
            if (f->type < schema_->types.size() && schema_->types[f->type].kind == TypeKind::Array && bits == 0)
                bits = schema_->types[f->type].count * 64;
        }
        if (bits == 0 || (bits + 7) / 8 > t.size) return err("bit array without capacity");
        tx.cls = Cls::BitArray;
        tx.count = bits;
        return;
    }
    case RoleKind::HashMap:
    case RoleKind::HashSet: {
        const bool isMap = role.kind == RoleKind::HashMap;
        const Field* ef;
        const Type* ea;
        if (!arrayField(isMap ? "_elements" : "_keys", ef, ea)) return;
        HashLayout h;
        h.isMap = isMap;
        h.capacity = ea->count;
        if (h.capacity == 0 || h.capacity > kMaxCapacity) return err("bad capacity");
        h.elemsOff = ef->offset;
        h.elementType = ea->element;
        const Type& elem = schema_->types[ea->element];
        h.elemSize = elem.size;
        if (isMap) {
            if (elem.kind != TypeKind::Record) return err("map element is not a record");
            if (!sub(elem, "key", h.keyOff, &h.keySize, &h.keyType)) return;
            if (!sub(elem, "value", h.valueOff, &h.valueSize, &h.valueType)) return;
            h.keyAssetHint = false;
            h.valueAssetHint = false;
        } else {
            h.keyOff = 0;
            h.keySize = elem.size;
            h.keyType = ea->element;
        }
        const Field* ff;
        const Type* fa;
        if (!arrayField("_occupationFlags", ff, fa)) return;
        if (!flagsOk(ff, fa, (h.capacity * 2 + 63) / 64)) return;
        h.flagsOff = ff->offset;
        if (!u64Field("_population", h.popOff) || !u64Field("_markRemovalCounter", h.mrcOff)) return;
        tx.hash = h;
        tx.cls = isMap ? Cls::HashMap : Cls::HashSet;
        return;
    }
    case RoleKind::Collection: {
        CollectionLayout c;
        const Field *pf, *ff, *ef;
        const Type *pa, *fa, *ea;
        if (!arrayField("_povs", pf, pa) || !arrayField("_povOccupationFlags", ff, fa) || !arrayField("_elements", ef, ea))
            return;
        c.capacity = pa->count;
        if (c.capacity == 0 || c.capacity > kMaxCapacity || ea->count != c.capacity) return err("bad capacity");
        if (!flagsOk(ff, fa, (c.capacity * 2 + 63) / 64)) return;
        c.povsOff = pf->offset;
        c.povType = pa->element;
        c.povSize = schema_->types[pa->element].size;
        c.povFlagsOff = ff->offset;
        c.elemsOff = ef->offset;
        c.elementType = ea->element;
        c.elemSize = schema_->types[ea->element].size;
        const Type& pov = schema_->types[pa->element];
        const Type& el = schema_->types[ea->element];
        if (pov.kind != TypeKind::Record || el.kind != TypeKind::Record) return err("PoV / element are not records");
        std::uint64_t sz = 0;
        if (!sub(pov, "value", c.povValueOff, &sz) || !sub(pov, "population", c.povPopOff) ||
            !sub(pov, "headIndex", c.povHeadOff) || !sub(pov, "tailIndex", c.povTailOff) ||
            !sub(pov, "bstRootIndex", c.povRootOff))
            return;
        if (sz != 32) return err("PoV value is not an id");
        if (!sub(el, "value", c.valueOff, &c.valueSize, &c.valueType) || !sub(el, "priority", c.prioOff) ||
            !sub(el, "povIndex", c.povIdxOff) || !sub(el, "bstParentIndex", c.parentOff) ||
            !sub(el, "bstLeftIndex", c.leftOff) || !sub(el, "bstRightIndex", c.rightOff))
            return;
        if (!u64Field("_population", c.popOff) || !u64Field("_markRemovalCounter", c.mrcOff)) return;
        tx.coll = c;
        tx.cls = Cls::Collection;
        return;
    }
    case RoleKind::LinkedList: {
        ListLayout l;
        const Field *nf, *ff;
        const Type *na, *fa;
        if (!arrayField("_nodes", nf, na) || !arrayField("_occupiedFlags", ff, fa)) return;
        l.capacity = na->count;
        if (l.capacity == 0 || l.capacity > kMaxCapacity) return err("bad capacity");
        if (!flagsOk(ff, fa, (l.capacity + 63) / 64)) return;
        l.nodesOff = nf->offset;
        l.nodeType = na->element;
        l.nodeSize = schema_->types[na->element].size;
        l.flagsOff = ff->offset;
        const Type& node = schema_->types[na->element];
        if (node.kind != TypeKind::Record) return err("node is not a record");
        if (!sub(node, "value", l.valueOff, &l.valueSize, &l.valueType) || !sub(node, "nextIndex", l.nextOff) ||
            !sub(node, "prevIndex", l.prevOff))
            return;
        if (!u64Field("_headIndex", l.headOff) || !u64Field("_tailIndex", l.tailOff) ||
            !u64Field("_freeHeadIndex", l.freeHeadOff) || !u64Field("_nextUnusedIndex", l.nextUnusedOff) ||
            !u64Field("_population", l.popOff))
            return;
        tx.list = l;
        tx.cls = Cls::LinkedList;
        return;
    }
    default: return;
    }
}

} // namespace qstate::decode
