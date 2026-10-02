#include "impl.h"

#include <charconv>

namespace qstate::decode {

using Impl = StateDecoder::Impl;

namespace {

bool parseNum(std::string_view s, std::uint64_t& v) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::uint64_t clampLimit(std::uint64_t limit, std::uint64_t def, std::uint64_t max) {
    if (limit == 0) limit = def;
    return std::min(limit, max);
}

std::uint64_t nextInstanceId() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

} // namespace

std::string joinId(const NodeId& parent, const std::string& seg) {
    return parent.empty() ? seg : parent + "/" + seg;
}

Impl::Impl(std::shared_ptr<const SchemaIndex> index, TypeId root, std::shared_ptr<const ByteSource> source,
           DecoderConfig config)
    : idxp(std::move(index)),
      idx(*idxp),
      rootType(root),
      src(std::move(source)),
      cfg(std::move(config)),
      codec(cfg.identity ? cfg.identity : std::make_shared<BasicIdentityCodec>()),
      cache(cfg.cache ? cfg.cache : std::make_shared<DecodeCache>(cfg.cacheBytes)),
      scopePrefix(std::to_string(cfg.scope) + "#" + std::to_string(nextInstanceId()) + "|"),
      fmt(idx, *codec, cfg.contractName) {
    if (!src) throw std::invalid_argument("StateDecoder: null ByteSource");
    if (!idx.valid(rootType)) throw std::invalid_argument("StateDecoder: invalid root type");
}

Impl::~Impl() { cache->erasePrefix(scopePrefix); }

Ref Impl::containerOf(const Ref& pov) const {
    Ref c;
    c.rk = RK::Plain;
    c.type = pov.type;
    c.offset = pov.base;
    c.size = idx.x(pov.type).size;
    const std::size_t slash = pov.id.rfind('/');
    c.id = slash == std::string::npos ? NodeId() : pov.id.substr(0, slash);
    return c;
}

void Impl::touch(const Query& q) const {
    const std::uint64_t prev = lastGen.exchange(q.generation);
    if (prev != q.generation && genSeen.exchange(true)) cache->erasePrefix(scopePrefix);
}

// ---- references -----------------------------------------------------------------------------------------------------

Ref Impl::rootRef() const {
    Ref r;
    r.rk = RK::Plain;
    r.type = rootType;
    r.offset = 0;
    r.size = idx.x(rootType).size;
    r.label = cfg.rootLabel.empty() ? typeNameOf(rootType) : cfg.rootLabel;
    return r;
}

const Member* Impl::findMember(TypeId t, std::string_view name) const {
    if (!idx.valid(t)) return nullptr;
    for (const Member& m : idx.x(t).members)
        if (m.name == name) return &m;
    return nullptr;
}

Ref Impl::plainRef(const Ref& parent, std::string seg, std::string label, TypeId type, std::uint64_t rel,
                   std::uint64_t size, bool assetHint) const {
    if (!idx.valid(type)) throw NotFoundError("node has no valid type");
    Ref r;
    r.rk = RK::Plain;
    r.type = type;
    r.offset = parent.offset + rel;
    r.size = size;
    r.assetHint = assetHint;
    r.label = std::move(label);
    r.id = joinId(parent.id, seg);
    return r;
}

Ref Impl::memberRef(const Ref& parent, const Member& m) const {
    Ref r = plainRef(parent, "f:" + m.name, m.name, m.type, m.offset, m.size, m.assetNameHint);
    r.bitOffset = m.bitOffset;
    r.bitWidth = m.bitWidth;
    return r;
}

Ref Impl::indexRef(const Ref& parent, std::uint64_t i) const {
    const TypeX& tx = idx.x(parent.type);
    return plainRef(parent, "i:" + std::to_string(i), "[" + std::to_string(i) + "]", tx.elem, i * tx.elemSize,
                    tx.elemSize, parent.assetHint);
}

Ref Impl::bitRef(const Ref& parent, std::uint64_t i) const {
    Ref r;
    r.rk = RK::Bit;
    r.type = kNoType;
    r.offset = parent.offset + i / 8;
    r.size = 1;
    r.bitOffset = static_cast<std::uint32_t>(i % 8);
    r.bitWidth = 1;
    r.index = i;
    r.label = "[" + std::to_string(i) + "]";
    r.id = joinId(parent.id, "b:" + std::to_string(i));
    return r;
}

Ref Impl::mapEntryRef(const Ref& c, std::uint64_t slot) const {
    const HashLayout& h = idx.x(c.type).hash;
    Ref r;
    r.rk = RK::MapEntry;
    r.type = c.type;
    r.base = c.offset;
    r.index = slot;
    r.offset = c.offset + h.elemsOff + slot * h.elemSize;
    r.size = h.elemSize;
    r.labelPending = true;
    r.id = joinId(c.id, "e:" + std::to_string(slot));
    return r;
}

Ref Impl::setElemRef(const Ref& c, std::uint64_t slot) const {
    const HashLayout& h = idx.x(c.type).hash;
    Ref r = plainRef(c, "e:" + std::to_string(slot), "", h.keyType, h.elemsOff + slot * h.elemSize, h.keySize);
    r.index = slot;
    r.base = c.offset;
    r.labelPending = true;
    return r;
}

Ref Impl::povRef(const Ref& c, std::uint64_t slot) const {
    const CollectionLayout& l = idx.x(c.type).coll;
    Ref r;
    r.rk = RK::Pov;
    r.type = c.type;
    r.base = c.offset;
    r.index = slot;
    r.offset = c.offset + l.povsOff + slot * l.povSize;
    r.size = l.povSize;
    r.labelPending = true;
    r.id = joinId(c.id, "p:" + std::to_string(slot));
    return r;
}

Ref Impl::collEntryRef(const Ref& c, std::uint64_t i) const {
    const CollectionLayout& l = idx.x(c.type).coll;
    Ref r;
    r.rk = RK::CollEntry;
    r.type = c.type;
    r.base = c.offset;
    r.index = i;
    r.offset = c.offset + l.elemsOff + i * l.elemSize;
    r.size = l.elemSize;
    r.label = "[" + std::to_string(i) + "]";
    r.id = joinId(c.id, "e:" + std::to_string(i));
    return r;
}

Ref Impl::listElemRef(const Ref& c, std::uint64_t node) const {
    const ListLayout& l = idx.x(c.type).list;
    Ref r = plainRef(c, "e:" + std::to_string(node), "[" + std::to_string(node) + "]", l.valueType,
                     l.nodesOff + node * l.nodeSize + l.valueOff, l.valueSize);
    r.index = node;
    r.base = c.offset;
    return r;
}

Ref Impl::entryChild(const Ref& e, std::string_view name) const {
    const TypeX& tx = idx.x(e.type);
    if (e.rk == RK::MapEntry) {
        const HashLayout& h = tx.hash;
        if (name == "key") return plainRef(e, "f:key", "key", h.keyType, h.keyOff, h.keySize);
        if (name == "value" && h.isMap) return plainRef(e, "f:value", "value", h.valueType, h.valueOff, h.valueSize);
        throw NotFoundError("no such member in a map entry: " + std::string(name));
    }
    if (e.rk == RK::CollEntry) {
        const CollectionLayout& l = tx.coll;
        if (name == "value") return plainRef(e, "f:value", "value", l.valueType, l.valueOff, l.valueSize);
        if (name == "priority") {
            const Member* m = findMember(l.elementType, "priority");
            if (m) return plainRef(e, "f:priority", "priority", m->type, l.prioOff, 8);
        }
        if (name == "pov") {
            // The PoV id stored in the PoV table; falls back to the raw povIndex member when the slot is invalid.
            const auto povIdx = readU64(e.offset + l.povIdxOff);
            const Member* idm = findMember(l.povType, "value");
            if (povIdx && *povIdx < l.capacity && idm) {
                Ref r;
                r.rk = RK::Plain;
                r.type = idm->type;
                r.offset = e.base + l.povsOff + *povIdx * l.povSize + l.povValueOff;
                r.size = 32;
                r.label = "pov";
                r.id = joinId(e.id, "f:pov");
                return r;
            }
            if (const Member* pm = findMember(l.elementType, "povIndex")) {
                Ref r = plainRef(e, "f:pov", "pov (invalid slot)", pm->type, l.povIdxOff, 8);
                return r;
            }
        }
        // raw members of the element record
        Ref rec;
        rec.rk = RK::Plain;
        rec.type = l.elementType;
        rec.offset = e.offset;
        rec.size = l.elemSize;
        rec.id = e.id;
        if (const Member* m = findMember(l.elementType, name)) return memberRef(rec, *m);
    }
    throw NotFoundError("no such member: " + std::string(name));
}

void Impl::finalize(Ref& r) const {
    if (!r.labelPending) return;
    r.labelPending = false;
    switch (r.rk) {
    case RK::MapEntry: {
        const HashLayout& h = idx.x(r.type).hash;
        r.label = keySummary(h.keyType, r.offset + h.keyOff, h.keySize, false);
        break;
    }
    case RK::Pov: {
        const CollectionLayout& l = idx.x(r.type).coll;
        std::uint8_t id[32];
        if (readExact(r.offset + l.povValueOff, 32, id)) r.label = fmt.shortText(fmt.idValue(id));
        else r.label = "pov " + std::to_string(r.index);
        break;
    }
    default: r.label = keySummary(r.type, r.offset, r.size, r.assetHint); break;
    }
}

std::string Impl::keySummary(TypeId t, std::uint64_t off, std::uint64_t size, bool assetHint) const {
    std::uint8_t buf[256];
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, sizeof buf));
    const std::size_t got = read(off, want, buf);
    if (got == 0) return "?";
    return fmt.preview(t, buf, got, assetHint);
}

std::string Impl::previewAt(TypeId t, std::uint64_t off, std::uint64_t size, bool assetHint) const {
    return keySummary(t, off, size, assetHint);
}

LeafValue Impl::leafAt(TypeId t, std::uint64_t off, std::uint32_t bitOff, std::uint32_t bitW, bool assetHint) const {
    if (!idx.valid(t)) return LeafValue::unavailable("unknown type");
    std::uint8_t buf[64];
    const std::uint64_t size = idx.x(t).size;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, sizeof buf));
    const std::size_t got = read(off, want, buf);
    if (got < size) return LeafValue::unavailable("beyond end of file");
    return fmt.leaf(t, buf, got, assetHint, bitOff, bitW);
}

// ---- resolve --------------------------------------------------------------------------------------------------------

Ref Impl::childBySegment(const Ref& p, std::string_view seg, const Query& q) const {
    auto nf = [&]() -> NotFoundError {
        return NotFoundError("no child '" + std::string(seg) + "' under node '" + p.id + "'");
    };
    if (seg.size() < 2 || seg[1] != ':') throw nf();
    const char kind = seg[0];
    const std::string_view arg = seg.substr(2);
    std::uint64_t n = 0;

    switch (p.rk) {
    case RK::Bit: throw nf();
    case RK::MapEntry:
    case RK::CollEntry:
        if (kind != 'f') throw nf();
        return entryChild(p, arg);
    case RK::Pov: {
        if (kind != 'e' || !parseNum(arg, n)) throw nf();
        const Ref c = containerOf(p);
        const auto ci = collInfo(c, q);
        if (n >= ci->elemCount) throw nf();
        return collEntryRef(c, n);
    }
    case RK::Plain: break;
    }

    const TypeX& tx = idx.x(p.type);
    switch (kind) {
    case 'f': {
        switch (tx.cls) {
        case Cls::Record:
        case Cls::Union:
        case Cls::HashMap:
        case Cls::HashSet:
        case Cls::Collection:
        case Cls::LinkedList:
        case Cls::ArrayRole:
        case Cls::BitArray: {
            const Member* m = findMember(p.type, arg);
            if (!m) throw nf();
            return memberRef(p, *m);
        }
        default: throw nf();
        }
    }
    case 'i':
        if ((tx.cls == Cls::CArray || tx.cls == Cls::ArrayRole) && parseNum(arg, n) && n < tx.count) return indexRef(p, n);
        throw nf();
    case 'b':
        if (tx.cls == Cls::BitArray && parseNum(arg, n) && n < tx.count) return bitRef(p, n);
        throw nf();
    case 'e':
        if (!parseNum(arg, n)) throw nf();
        switch (tx.cls) {
        case Cls::HashMap:
            if (n < tx.hash.capacity && slotLive(p, tx.hash.flagsOff, n)) return mapEntryRef(p, n);
            throw nf();
        case Cls::HashSet:
            if (n < tx.hash.capacity && slotLive(p, tx.hash.flagsOff, n)) return setElemRef(p, n);
            throw nf();
        case Cls::Collection: {
            const auto ci = collInfo(p, q);
            if (n < ci->elemCount) return collEntryRef(p, n);
            throw nf();
        }
        case Cls::LinkedList:
            if (n < tx.list.capacity && listNodeOccupied(tx.list, p.offset, n)) return listElemRef(p, n);
            throw nf();
        default: throw nf();
        }
    case 'p':
        if (tx.cls == Cls::Collection && parseNum(arg, n) && n < tx.coll.capacity &&
            slotLive(p, tx.coll.povFlagsOff, n))
            return povRef(p, n);
        throw nf();
    default: throw nf();
    }
}

Ref Impl::resolve(const NodeId& id, const Query& q) const {
    Ref cur = rootRef();
    std::size_t pos = 0;
    while (pos < id.size()) {
        std::size_t end = id.find('/', pos);
        if (end == std::string::npos) end = id.size();
        cur = childBySegment(cur, std::string_view(id).substr(pos, end - pos), q);
        pos = end + 1;
        if (end == id.size()) break;
    }
    return cur;
}

// ---- describe -------------------------------------------------------------------------------------------------------

NodeInfo Impl::describe(Ref r, const Query& q) const {
    finalize(r);
    NodeInfo n;
    n.id = r.id;
    n.label = r.label;
    n.offset = r.offset;
    n.size = r.size;
    n.inFile = inFile(r.offset, r.size);
    n.kind = NodeKind::Leaf;

    auto readPrefix = [&](std::uint64_t off, std::uint64_t size, std::size_t cap) {
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min<std::uint64_t>(size, cap)));
        buf.resize(read(off, buf.size(), buf.data()));
        return buf;
    };

    switch (r.rk) {
    case RK::Bit: {
        n.typeName = "bit";
        n.kind = NodeKind::Leaf;
        n.bit = std::make_pair(r.bitOffset, 1u);
        std::uint8_t b = 0;
        if (read(r.offset, 1, &b) == 1) {
            LeafValue v;
            v.k = LeafValue::Kind::Bool;
            v.raw = (b >> r.bitOffset) & 1u;
            v.boolean = v.raw != 0;
            n.value = v;
        } else {
            n.value = LeafValue::unavailable("beyond end of file");
        }
        return n;
    }
    case RK::MapEntry: {
        const HashLayout& h = idx.x(r.type).hash;
        n.typeId = h.elementType;
        n.typeName = typeNameOf(h.elementType);
        n.kind = NodeKind::Entry;
        n.childCount = h.isMap ? 2 : 1;
        const auto buf = readPrefix(r.offset, r.size, 512);
        std::string prev;
        if (buf.size() > h.keyOff) {
            prev = fmt.preview(h.keyType, buf.data() + h.keyOff, buf.size() - static_cast<std::size_t>(h.keyOff));
            if (h.isMap && buf.size() > h.valueOff)
                prev += " \xE2\x86\x92 " + fmt.preview(h.valueType, buf.data() + h.valueOff,
                                                       buf.size() - static_cast<std::size_t>(h.valueOff));
        }
        n.preview = prev;
        break;
    }
    case RK::CollEntry: {
        const CollectionLayout& l = idx.x(r.type).coll;
        n.typeId = l.elementType;
        n.typeName = typeNameOf(l.elementType);
        n.kind = NodeKind::Entry;
        n.childCount = 3;
        n.rawChildCount = idx.x(l.elementType).members.size();
        const auto buf = readPrefix(r.offset, r.size, 512);
        std::string prev;
        if (buf.size() >= l.prioOff + 8) {
            prev = "priority " + std::to_string(static_cast<std::int64_t>(fmt::loadLe(buf.data() + l.prioOff, 8)));
            if (buf.size() >= l.valueOff)
                prev += ": " + fmt.preview(l.valueType, buf.data() + l.valueOff,
                                           buf.size() - static_cast<std::size_t>(l.valueOff));
        }
        n.preview = prev;
        break;
    }
    case RK::Pov: {
        const CollectionLayout& l = idx.x(r.type).coll;
        n.typeId = l.povType;
        n.typeName = typeNameOf(l.povType);
        n.kind = NodeKind::Pov;
        std::uint8_t id[32];
        if (readExact(r.offset + l.povValueOff, 32, id)) n.value = fmt.idValue(id);
        else n.value = LeafValue::unavailable("beyond end of file");
        const auto ci = collInfo(containerOf(r), q);
        if (const PovRec* pov = ci->findPov(r.index)) {
            n.childCount = std::min(pov->population, ci->elemCount);
            n.preview = "population " + std::to_string(pov->population);
        }
        break;
    }
    case RK::Plain: {
        n.typeId = r.type;
        n.typeName = typeNameOf(r.type);
        const TypeX& tx = idx.x(r.type);
        const schema::Type& st = idx.type(r.type);
        if (r.bitWidth) n.bit = std::make_pair(r.bitOffset, r.bitWidth);
        switch (tx.cls) {
        case Cls::Int: case Cls::Bool: case Cls::Char: case Cls::Enum: case Cls::Float: case Cls::Ptr:
        case Cls::Id: case Cls::Bit: case Cls::U128: case Cls::DateTime: case Cls::Opaque:
            n.kind = NodeKind::Leaf;
            n.value = leafAt(r.type, r.offset, r.bitOffset, r.bitWidth, r.assetHint);
            break;
        case Cls::Record:
        case Cls::Union: {
            n.kind = tx.cls == Cls::Union ? NodeKind::Union : NodeKind::Struct;
            n.childCount = tx.members.size();
            const auto buf = readPrefix(r.offset, r.size, 2048); // proposal slot summaries read the vote array
            if (!buf.empty() && !tx.members.empty()) n.preview = fmt.preview(r.type, buf.data(), buf.size());
            if (!tx.layoutError.empty()) {
                ContainerStats cs;
                cs.capacity = st.role.capacity;
                cs.warning = "layout not decodable: " + tx.layoutError;
                n.container = cs;
            }
            break;
        }
        case Cls::CArray:
        case Cls::ArrayRole: {
            n.kind = NodeKind::Array;
            n.childCount = tx.count;
            if (tx.cls == Cls::ArrayRole) n.rawChildCount = tx.members.size();
            const auto buf = readPrefix(r.offset, r.size, 1024);
            if (!buf.empty()) n.preview = fmt.preview(r.type, buf.data(), buf.size());
            if (tx.byteElems) {
                LeafValue v = fmt.bytesValue(buf.data(), buf.size());
                v.length = tx.count;
                if (buf.size() < tx.count) {
                    v.truncated = true;
                    v.text.reset();
                }
                if (buf.size() < r.size) v.text.reset();
                n.value = v;
            }
            n.tabular = tx.count > 0;
            break;
        }
        case Cls::BitArray: {
            n.kind = NodeKind::BitArray;
            n.childCount = tx.count;
            n.rawChildCount = tx.members.size();
            const auto buf = readPrefix(r.offset, (tx.count + 7) / 8, 4096);
            LeafValue v = fmt.bitsValue(buf.data(), buf.size(), std::min<std::uint64_t>(tx.count, buf.size() * 8));
            v.count = tx.count;
            if (buf.size() < (tx.count + 7) / 8) v.truncated = true;
            n.value = v;
            break;
        }
        case Cls::HashMap:
        case Cls::HashSet:
        case Cls::Collection:
        case Cls::LinkedList: {
            n.kind = tx.cls == Cls::HashMap ? NodeKind::HashMap
                     : tx.cls == Cls::HashSet ? NodeKind::HashSet
                     : tx.cls == Cls::Collection ? NodeKind::Collection : NodeKind::LinkedList;
            n.rawChildCount = tx.members.size();
            n.tabular = true;
            ContainerStats cs = containerStats(r, q);
            if (tx.cls == Cls::Collection) n.childCount = cs.povs.value_or(0);
            else n.childCount = cs.population.value_or(0);
            n.preview = std::to_string(cs.population.value_or(0)) + " / " + std::to_string(cs.capacity) +
                        (tx.cls == Cls::Collection ? " elements, " + std::to_string(cs.povs.value_or(0)) + " PoVs"
                                                   : " entries");
            n.container = std::move(cs);
            break;
        }
        }
        break;
    }
    }
    if (!r.bitWidth && n.size <= 4096 && n.inFile) n.zero = src->isAllZero(n.offset, n.size);
    return n;
}

// ---- children -------------------------------------------------------------------------------------------------------

ChildList Impl::listChildren(const Ref& p, ChildView view, std::uint64_t offset, std::uint64_t limit, bool hideEmpty,
                             const Query& q) const {
    ChildList out;
    auto sliceRange = [&](std::uint64_t total, auto&& make) {
        out.total = total;
        if (offset >= total) return;
        const std::uint64_t end = std::min(total, offset + limit);
        out.items.reserve(static_cast<std::size_t>(end - offset));
        for (std::uint64_t i = offset; i < end; ++i) out.items.push_back(make(i));
    };
    auto sliceIds = [&](const std::vector<std::uint32_t>& ids, auto&& make) {
        sliceRange(ids.size(), [&](std::uint64_t i) { return make(ids[static_cast<std::size_t>(i)]); });
    };
    auto rawMembers = [&](const Ref& rec) {
        const auto& members = idx.x(rec.type).members;
        sliceRange(members.size(), [&](std::uint64_t i) { return memberRef(rec, members[static_cast<std::size_t>(i)]); });
    };

    switch (p.rk) {
    case RK::Bit: return out;
    case RK::MapEntry: {
        const HashLayout& h = idx.x(p.type).hash;
        std::vector<Ref> kids;
        kids.push_back(entryChild(p, "key"));
        if (h.isMap) kids.push_back(entryChild(p, "value"));
        sliceRange(kids.size(), [&](std::uint64_t i) { return kids[static_cast<std::size_t>(i)]; });
        return out;
    }
    case RK::CollEntry: {
        const CollectionLayout& l = idx.x(p.type).coll;
        if (view == ChildView::Raw) {
            Ref rec;
            rec.rk = RK::Plain;
            rec.type = l.elementType;
            rec.offset = p.offset;
            rec.size = l.elemSize;
            rec.id = p.id;
            rawMembers(rec);
            return out;
        }
        static const char* names[3] = {"value", "priority", "pov"};
        sliceRange(3, [&](std::uint64_t i) { return entryChild(p, names[i]); });
        return out;
    }
    case RK::Pov: {
        const Ref c = containerOf(p);
        const auto ci = collInfo(c, q);
        const PovRec* pov = ci->findPov(p.index);
        if (!pov) return out;
        const std::uint64_t total = std::min(pov->population, ci->elemCount);
        out.total = total;
        if (offset >= total) return out;
        const std::uint64_t need = std::min(total, offset + limit);
        std::vector<std::uint32_t> ids;
        std::shared_ptr<const IndexList> full = cache->getAs<IndexList>(
            key(q, "po:" + std::to_string(c.offset) + ":" + std::to_string(pov->slot)));
        if (full) {
            ids.assign(full->ids.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(offset, full->ids.size())),
                       full->ids.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(need, full->ids.size())));
        } else if (need <= 2048) {
            const auto walked = povWalk(c, *ci, *pov, need, q);
            if (offset < walked.size()) ids.assign(walked.begin() + static_cast<std::ptrdiff_t>(offset), walked.end());
        } else {
            const auto all = povOrder(c, *ci, *pov, q);
            if (offset < all->ids.size())
                ids.assign(all->ids.begin() + static_cast<std::ptrdiff_t>(offset),
                           all->ids.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(need, all->ids.size())));
        }
        for (std::uint32_t e : ids) out.items.push_back(collEntryRef(c, e));
        return out;
    }
    case RK::Plain: break;
    }

    const TypeX& tx = idx.x(p.type);
    const bool raw = view == ChildView::Raw;
    switch (tx.cls) {
    case Cls::Record:
    case Cls::Union: rawMembers(p); return out;
    case Cls::CArray:
    case Cls::ArrayRole:
        if (raw && tx.cls == Cls::ArrayRole) {
            rawMembers(p);
            return out;
        }
        if (hideEmpty) {
            const auto ne = nonEmptyIndex(p.offset, tx.elemSize, tx.count, q);
            sliceIds(ne->ids, [&](std::uint64_t i) { return indexRef(p, i); });
        } else {
            sliceRange(tx.count, [&](std::uint64_t i) { return indexRef(p, i); });
        }
        return out;
    case Cls::BitArray: {
        if (raw) {
            rawMembers(p);
            return out;
        }
        if (hideEmpty) {
            std::vector<std::uint32_t> setBits;
            std::vector<std::uint8_t> buf(static_cast<std::size_t>((tx.count + 7) / 8));
            buf.resize(read(p.offset, buf.size(), buf.data()));
            for (std::size_t b = 0; b < buf.size(); ++b)
                for (unsigned k = 0; k < 8; ++k)
                    if (((buf[b] >> k) & 1u) && b * 8 + k < tx.count) setBits.push_back(static_cast<std::uint32_t>(b * 8 + k));
            sliceIds(setBits, [&](std::uint64_t i) { return bitRef(p, i); });
        } else {
            sliceRange(tx.count, [&](std::uint64_t i) { return bitRef(p, i); });
        }
        return out;
    }
    case Cls::HashMap:
    case Cls::HashSet: {
        if (raw) {
            rawMembers(p);
            return out;
        }
        const HashInfo hi = hashInfo(p, q);
        const bool isMap = tx.cls == Cls::HashMap;
        sliceIds(hi.flags->live, [&](std::uint64_t slot) { return isMap ? mapEntryRef(p, slot) : setElemRef(p, slot); });
        return out;
    }
    case Cls::Collection: {
        if (raw) {
            rawMembers(p);
            return out;
        }
        const auto ci = collInfo(p, q);
        out.total = ci->povs.size();
        if (offset >= out.total) return out;
        const std::uint64_t end = std::min<std::uint64_t>(out.total, offset + limit);
        for (std::uint64_t i = offset; i < end; ++i) out.items.push_back(povRef(p, ci->povs[static_cast<std::size_t>(i)].slot));
        return out;
    }
    case Cls::LinkedList: {
        if (raw) {
            rawMembers(p);
            return out;
        }
        const auto order = listOrder(p, q);
        sliceIds(order->ids, [&](std::uint64_t node) { return listElemRef(p, node); });
        return out;
    }
    default: return out;
    }
}

// ---- locate ---------------------------------------------------------------------------------------------------------

NodeLocation Impl::locate(std::uint64_t off, const Query& q) const {
    Ref cur = rootRef();
    if (off >= cur.size) throw NotFoundError("offset " + std::to_string(off) + " is outside the state type");
    NodeLocation loc;
    {
        Ref r = cur;
        finalize(r);
        loc.path.push_back({r.id, r.label});
    }
    auto within = [&](const Ref& r) { return off >= r.offset && off - r.offset < r.size; };
    for (int depth = 0; depth < 64; ++depth) {
        checkCancel(q);
        std::optional<Ref> next;
        std::optional<Ref> via; // intermediate node between cur and next (the PoV of a collection element)
        const std::uint64_t rel = off - cur.offset;
        auto tryRef = [&](Ref r) {
            if (within(r)) next = std::move(r);
        };
        switch (cur.rk) {
        case RK::Bit:
        case RK::Pov: break;
        case RK::MapEntry: {
            const HashLayout& h = idx.x(cur.type).hash;
            if (rel >= h.keyOff && rel < h.keyOff + h.keySize) tryRef(entryChild(cur, "key"));
            else if (h.isMap && rel >= h.valueOff && rel < h.valueOff + h.valueSize) tryRef(entryChild(cur, "value"));
            break;
        }
        case RK::CollEntry: {
            const CollectionLayout& l = idx.x(cur.type).coll;
            if (rel >= l.valueOff && rel < l.valueOff + l.valueSize) tryRef(entryChild(cur, "value"));
            else if (rel >= l.prioOff && rel < l.prioOff + 8) tryRef(entryChild(cur, "priority"));
            break;
        }
        case RK::Plain: {
            const TypeX& tx = idx.x(cur.type);
            auto viaMembers = [&]() {
                for (const Member& m : tx.members) {
                    if (m.size == 0) continue;
                    if (rel >= m.offset && rel < m.offset + m.size) {
                        tryRef(memberRef(cur, m));
                        return;
                    }
                }
            };
            switch (tx.cls) {
            case Cls::Record:
            case Cls::Union: viaMembers(); break;
            case Cls::CArray:
            case Cls::ArrayRole:
                if (tx.elemSize > 0 && rel / tx.elemSize < tx.count) tryRef(indexRef(cur, rel / tx.elemSize));
                break;
            case Cls::HashMap:
            case Cls::HashSet: {
                const HashLayout& h = tx.hash;
                if (rel >= h.elemsOff && rel < h.elemsOff + h.capacity * h.elemSize && h.elemSize > 0) {
                    const std::uint64_t slot = (rel - h.elemsOff) / h.elemSize;
                    if (slotLive(cur, h.flagsOff, slot)) {
                        tryRef(tx.cls == Cls::HashMap ? mapEntryRef(cur, slot) : setElemRef(cur, slot));
                        break;
                    }
                }
                viaMembers();
                break;
            }
            case Cls::Collection: {
                const CollectionLayout& l = tx.coll;
                if (rel >= l.povsOff && rel < l.povsOff + l.capacity * l.povSize && l.povSize > 0) {
                    const std::uint64_t slot = (rel - l.povsOff) / l.povSize;
                    if (slotLive(cur, l.povFlagsOff, slot)) {
                        tryRef(povRef(cur, slot));
                        break;
                    }
                } else if (rel >= l.elemsOff && rel < l.elemsOff + l.capacity * l.elemSize && l.elemSize > 0) {
                    const std::uint64_t i = (rel - l.elemsOff) / l.elemSize;
                    if (i < collInfo(cur, q)->elemCount) {
                        tryRef(collEntryRef(cur, i));
                        if (next) via = povOfElement(cur, *next, q);
                        break;
                    }
                }
                viaMembers();
                break;
            }
            case Cls::LinkedList: {
                const ListLayout& l = tx.list;
                if (rel >= l.nodesOff && rel < l.nodesOff + l.capacity * l.nodeSize && l.nodeSize > 0) {
                    const std::uint64_t node = (rel - l.nodesOff) / l.nodeSize;
                    const std::uint64_t inNode = (rel - l.nodesOff) % l.nodeSize;
                    if (inNode >= l.valueOff && inNode < l.valueOff + l.valueSize && listNodeOccupied(l, cur.offset, node)) {
                        tryRef(listElemRef(cur, node));
                        break;
                    }
                }
                viaMembers();
                break;
            }
            default: break;
            }
            break;
        }
        }
        if (!next) break;
        if (via) {
            finalize(*via);
            loc.path.push_back({via->id, via->label});
        }
        finalize(*next);
        loc.path.push_back({next->id, next->label});
        cur = std::move(*next);
    }
    loc.id = cur.id;
    loc.offset = cur.offset;
    loc.size = cur.size;
    loc.typeName = typeNameOfRef(cur);
    return loc;
}

std::string Impl::typeNameOfRef(const Ref& r) const {
    switch (r.rk) {
    case RK::Bit: return "bit";
    case RK::MapEntry: return typeNameOf(idx.x(r.type).hash.elementType);
    case RK::CollEntry: return typeNameOf(idx.x(r.type).coll.elementType);
    case RK::Pov: return typeNameOf(idx.x(r.type).coll.povType);
    case RK::Plain: return typeNameOf(r.type);
    }
    return {};
}

// ---- public wrappers ------------------------------------------------------------------------------------------------

StateDecoder::StateDecoder(std::shared_ptr<const SchemaIndex> index, TypeId rootType,
                           std::shared_ptr<const ByteSource> source, DecoderConfig config)
    : impl_(std::make_unique<Impl>(std::move(index), rootType, std::move(source), std::move(config))) {}

StateDecoder::StateDecoder(std::shared_ptr<const schema::Schema> schema, TypeId rootType,
                           std::shared_ptr<const ByteSource> source, DecoderConfig config)
    : StateDecoder(std::make_shared<SchemaIndex>(std::move(schema)), rootType, std::move(source), std::move(config)) {}

StateDecoder::~StateDecoder() = default;

NodeInfo StateDecoder::node(const NodeId& id, const Query& q) const {
    impl_->touch(q);
    return impl_->describe(impl_->resolve(id, q), q);
}

ChildrenPage StateDecoder::children(const NodeId& id, const ChildrenRequest& req, const Query& q) const {
    impl_->touch(q);
    const Ref parent = impl_->resolve(id, q);
    ChildList cl = impl_->listChildren(parent, req.view, req.offset, clampLimit(req.limit, 200, 1000), req.hideEmpty, q);
    ChildrenPage page;
    page.total = cl.total;
    page.offset = req.offset;
    page.items.reserve(cl.items.size());
    for (Ref& r : cl.items) {
        Impl::checkCancel(q);
        page.items.push_back(impl_->describe(std::move(r), q));
    }
    return page;
}

NodeLocation StateDecoder::locate(std::uint64_t offset, const Query& q) const {
    impl_->touch(q);
    return impl_->locate(offset, q);
}

NodeReveal StateDecoder::reveal(const NodeId& id, bool hideEmpty, const Query& q) const {
    impl_->touch(q);
    return impl_->reveal(id, hideEmpty, q);
}

SearchResult StateDecoder::search(const SearchRequest& req, const Query& q) const {
    impl_->touch(q);
    return impl_->search(req, q);
}

TableInfo StateDecoder::describeTable(const NodeId& id, const std::string& view, const Query& q) const {
    impl_->touch(q);
    return impl_->describeTable(id, view, q);
}

TablePage StateDecoder::tableRows(const TableRequest& req, const Query& q) const {
    impl_->touch(q);
    return impl_->tableRows(req, q);
}

std::uint64_t StateDecoder::stateSize() const { return impl_->idx.x(impl_->rootType).size; }
std::uint64_t StateDecoder::fileSize() const { return impl_->src->size(); }
DecodeCache& StateDecoder::cache() const { return *impl_->cache; }

} // namespace qstate::decode
