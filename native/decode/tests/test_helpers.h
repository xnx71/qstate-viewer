// Test helpers: hand-built schemas, byte image writers that mirror the QPI container encodings (independently of the
// decoder), sparse / pread byte sources.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qstate/decode/byte_source.h"
#include "qstate/decode/decoder.h"
#include "qstate/decode/identity.h"
#include "qstate/schema/model.h"

namespace qstate::decode::testing {

using schema::PrimKind;
using schema::RoleKind;
using schema::TypeId;
using schema::TypeKind;

inline std::uint64_t up(std::uint64_t x, std::uint64_t a) { return (x + a - 1) / a * a; }

// ---- schema builder -------------------------------------------------------------------------------------------------

class SB {
public:
    schema::Schema s;

    TypeId add(schema::Type t) {
        t.id = static_cast<TypeId>(s.types.size());
        s.types.push_back(std::move(t));
        return s.types.back().id;
    }
    TypeId prim(const std::string& name, std::uint64_t size, PrimKind k, bool isSigned) {
        schema::Type t;
        t.name = name;
        t.kind = TypeKind::Prim;
        t.size = size;
        t.align = size;
        t.prim = k;
        t.isSigned = isSigned;
        return add(std::move(t));
    }
    TypeId u8() { return get("uint8", [&] { return prim("uint8", 1, PrimKind::UInt, false); }); }
    TypeId s8() { return get("sint8", [&] { return prim("sint8", 1, PrimKind::SInt, true); }); }
    TypeId u16() { return get("uint16", [&] { return prim("uint16", 2, PrimKind::UInt, false); }); }
    TypeId s16() { return get("sint16", [&] { return prim("sint16", 2, PrimKind::SInt, true); }); }
    TypeId u32() { return get("uint32", [&] { return prim("uint32", 4, PrimKind::UInt, false); }); }
    TypeId s32() { return get("sint32", [&] { return prim("sint32", 4, PrimKind::SInt, true); }); }
    TypeId u64() { return get("uint64", [&] { return prim("uint64", 8, PrimKind::UInt, false); }); }
    TypeId s64() { return get("sint64", [&] { return prim("sint64", 8, PrimKind::SInt, true); }); }
    TypeId boolean() { return get("bool", [&] { return prim("bool", 1, PrimKind::Bool, false); }); }
    TypeId chr() { return get("char", [&] { return prim("char", 1, PrimKind::Char, true); }); }
    TypeId f32() { return get("float", [&] { return prim("float", 4, PrimKind::Float, true); }); }
    TypeId f64() { return get("double", [&] { return prim("double", 8, PrimKind::Float, true); }); }

    TypeId roleLeaf(const std::string& name, std::uint64_t size, RoleKind role) {
        return get(name, [&] {
            schema::Type t;
            t.name = name;
            t.kind = TypeKind::Record;
            t.size = size;
            t.align = 8;
            t.role.kind = role;
            return add(std::move(t));
        });
    }
    TypeId id() { return roleLeaf("id", 32, RoleKind::Id); }
    TypeId bit() {
        return get("bit", [&] {
            schema::Type t;
            t.name = "bit";
            t.kind = TypeKind::Record;
            t.size = 1;
            t.align = 1;
            t.role.kind = RoleKind::Bit;
            return add(std::move(t));
        });
    }
    TypeId u128() { return roleLeaf("uint128", 16, RoleKind::Uint128); }
    TypeId dateTime() { return roleLeaf("DateAndTime", 8, RoleKind::DateTime); }

    TypeId array(TypeId elem, std::uint64_t count) {
        schema::Type t;
        t.name = s.types[elem].name + "[" + std::to_string(count) + "]";
        t.kind = TypeKind::Array;
        t.element = elem;
        t.count = count;
        t.size = s.types[elem].size * count;
        t.align = s.types[elem].align;
        return add(std::move(t));
    }
    TypeId enumT(const std::string& name, TypeId underlying, std::vector<schema::Enumerator> values) {
        schema::Type t;
        t.name = name;
        t.kind = TypeKind::Enum;
        t.size = s.types[underlying].size;
        t.align = t.size;
        t.underlying = underlying;
        t.enumerators = std::move(values);
        return add(std::move(t));
    }

    // Struct / union with natural alignment.
    TypeId record(const std::string& name, const std::vector<std::pair<std::string, TypeId>>& fields, bool isUnion = false) {
        schema::Type t;
        t.name = name;
        t.kind = TypeKind::Record;
        t.recordKind = isUnion ? schema::RecordKind::Union : schema::RecordKind::Struct;
        std::uint64_t off = 0, align = 1, maxSize = 0;
        for (const auto& [fname, ft] : fields) {
            const schema::Type& f = s.types[ft];
            schema::Field fld;
            fld.name = fname;
            fld.type = ft;
            fld.typeName = f.name;
            fld.size = f.size;
            if (isUnion) {
                fld.offset = 0;
                maxSize = std::max(maxSize, f.size);
            } else {
                off = up(off, f.align);
                fld.offset = off;
                off += f.size;
            }
            align = std::max(align, f.align);
            t.fields.push_back(std::move(fld));
        }
        t.align = align;
        t.size = up(isUnion ? maxSize : off, align);
        if (t.size == 0) t.size = 1;
        return add(std::move(t));
    }

    // ---- QPI containers (member order of qpi_containers.h)
    TypeId qpiArray(TypeId elem, std::uint64_t L) {
        TypeId arr = array(elem, L);
        TypeId r = record("QPI::Array<" + s.types[elem].name + "," + std::to_string(L) + ">", {{"_values", arr}});
        s.types[r].role = {RoleKind::Array, elem, schema::kNoType, schema::kNoType, L};
        return r;
    }
    TypeId bitArray(std::uint64_t L) {
        TypeId arr = array(u64(), (L + 63) / 64);
        TypeId r = record("QPI::BitArray<" + std::to_string(L) + ">", {{"_values", arr}});
        s.types[r].role.kind = RoleKind::BitArray;
        s.types[r].role.capacity = L;
        return r;
    }
    TypeId hashMap(TypeId K, TypeId V, std::uint64_t L) {
        TypeId el = record("HashMap::Element", {{"key", K}, {"value", V}});
        TypeId r = record("QPI::HashMap<" + s.types[K].name + "," + s.types[V].name + "," + std::to_string(L) + ">",
                          {{"_elements", array(el, L)},
                           {"_occupationFlags", array(u64(), (L * 2 + 63) / 64)},
                           {"_population", u64()},
                           {"_markRemovalCounter", u64()}});
        s.types[r].role = {RoleKind::HashMap, schema::kNoType, K, V, L};
        return r;
    }
    TypeId hashSet(TypeId K, std::uint64_t L) {
        TypeId r = record("QPI::HashSet<" + s.types[K].name + "," + std::to_string(L) + ">",
                          {{"_keys", array(K, L)},
                           {"_occupationFlags", array(u64(), (L * 2 + 63) / 64)},
                           {"_population", u64()},
                           {"_markRemovalCounter", u64()}});
        s.types[r].role = {RoleKind::HashSet, schema::kNoType, K, schema::kNoType, L};
        return r;
    }
    TypeId collection(TypeId T, std::uint64_t L) {
        TypeId pov = record("Collection::PoV", {{"value", id()}, {"population", u64()}, {"headIndex", s64()},
                                                {"tailIndex", s64()}, {"bstRootIndex", s64()}});
        TypeId el = record("Collection::Element", {{"value", T}, {"priority", s64()}, {"povIndex", s64()},
                                                   {"bstParentIndex", s64()}, {"bstLeftIndex", s64()},
                                                   {"bstRightIndex", s64()}});
        TypeId r = record("QPI::Collection<" + s.types[T].name + "," + std::to_string(L) + ">",
                          {{"_povs", array(pov, L)},
                           {"_povOccupationFlags", array(u64(), (L * 2 + 63) / 64)},
                           {"_elements", array(el, L)},
                           {"_population", u64()},
                           {"_markRemovalCounter", u64()}});
        s.types[r].role = {RoleKind::Collection, T, schema::kNoType, schema::kNoType, L};
        return r;
    }
    TypeId linkedList(TypeId T, std::uint64_t L) {
        TypeId node = record("LinkedList::Node", {{"value", T}, {"nextIndex", s64()}, {"prevIndex", s64()}});
        TypeId r = record("QPI::LinkedList<" + s.types[T].name + "," + std::to_string(L) + ">",
                          {{"_nodes", array(node, L)},
                           {"_occupiedFlags", array(u64(), (L + 63) / 64)},
                           {"_headIndex", s64()},
                           {"_tailIndex", s64()},
                           {"_freeHeadIndex", s64()},
                           {"_nextUnusedIndex", u64()},
                           {"_population", u64()}});
        s.types[r].role = {RoleKind::LinkedList, T, schema::kNoType, schema::kNoType, L};
        return r;
    }

    std::uint64_t size(TypeId t) const { return s.types[t].size; }
    std::shared_ptr<const schema::Schema> finish() const { return std::make_shared<schema::Schema>(s); }

private:
    std::map<std::string, TypeId> cache_;
    template <class F>
    TypeId get(const std::string& name, F&& make) {
        auto it = cache_.find(name);
        if (it != cache_.end()) return it->second;
        const TypeId t = make();
        cache_[name] = t;
        return t;
    }
};

// ---- image writer ---------------------------------------------------------------------------------------------------

struct Img {
    std::vector<std::uint8_t> b;
    explicit Img(std::uint64_t size = 0) : b(size, 0) {}
    void put(std::uint64_t off, std::uint64_t v, int n = 8) {
        for (int i = 0; i < n; ++i) b.at(off + static_cast<std::uint64_t>(i)) = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
    void putBytes(std::uint64_t off, const std::vector<std::uint8_t>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) b.at(off + i) = v[i];
    }
    void putText(std::uint64_t off, const std::string& t) {
        for (std::size_t i = 0; i < t.size(); ++i) b.at(off + i) = static_cast<std::uint8_t>(t[i]);
    }
    std::uint64_t get(std::uint64_t off, int n = 8) const {
        std::uint64_t v = 0;
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | b.at(off + static_cast<std::uint64_t>(i));
        return v;
    }
};

// 32 byte id with the four little-endian words.
inline std::vector<std::uint8_t> idBytes(std::uint64_t w0, std::uint64_t w1 = 0, std::uint64_t w2 = 0, std::uint64_t w3 = 0) {
    std::vector<std::uint8_t> v(32);
    const std::uint64_t w[4] = {w0, w1, w2, w3};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) v[static_cast<std::size_t>(8 * i + j)] = static_cast<std::uint8_t>((w[i] >> (8 * j)) & 0xFF);
    return v;
}

inline std::vector<std::uint8_t> le(std::uint64_t v, int n = 8) {
    std::vector<std::uint8_t> r(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) r[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    return r;
}

// Hash container writer (HashMap<K,V,L> / HashSet<K,L>) from the closed-form layout of report 01 section 3.4/3.5.
struct HashW {
    Img& img;
    std::uint64_t base, L, keySize, valSize, valOff, elemSize, flagsOff, popOff, mrcOff;

    HashW(Img& i, std::uint64_t base_, std::uint64_t L_, std::uint64_t ks, std::uint64_t vs, std::uint64_t valAlign,
          std::uint64_t elemAlign)
        : img(i), base(base_), L(L_), keySize(ks), valSize(vs) {
        valOff = vs ? up(ks, valAlign) : 0;
        elemSize = vs ? up(valOff + vs, elemAlign) : ks;
        flagsOff = up(L * elemSize, 8);
        popOff = flagsOff + 8 * ((L * 2 + 63) / 64);
        mrcOff = popOff + 8;
    }
    std::uint64_t total() const { return mrcOff + 8; }
    void setState(std::uint64_t slot, unsigned st) {
        const std::uint64_t w = base + flagsOff + (slot >> 5) * 8;
        std::uint64_t v = img.get(w);
        v &= ~(std::uint64_t{3} << ((slot & 31) * 2));
        v |= std::uint64_t{st} << ((slot & 31) * 2);
        img.put(w, v);
    }
    void set(std::uint64_t slot, const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& val = {}) {
        img.putBytes(base + slot * elemSize, key);
        if (!val.empty()) img.putBytes(base + slot * elemSize + valOff, val);
        setState(slot, 1);
    }
    void tombstone(std::uint64_t slot) {
        for (std::uint64_t i = 0; i < elemSize; ++i) img.b.at(base + slot * elemSize + i) = 0;
        setState(slot, 2);
    }
    void counters(std::uint64_t pop, std::uint64_t mrc) {
        img.put(base + popOff, pop);
        img.put(base + mrcOff, mrc);
    }
};

// Collection writer following the algorithms of qpi_collection_impl.h (add into PoV queue, BST by priority).
struct CollW {
    Img& img;
    std::uint64_t base, L, T, elemSize, valueSize, povFlagsOff, elemsOff, popOff, mrcOff;

    CollW(Img& i, std::uint64_t base_, std::uint64_t L_, std::uint64_t valueSize_) : img(i), base(base_), L(L_), valueSize(valueSize_) {
        T = up(valueSize_, 8);
        elemSize = T + 40;
        povFlagsOff = 64 * L;
        elemsOff = povFlagsOff + 8 * ((L * 2 + 63) / 64);
        popOff = elemsOff + L * elemSize;
        mrcOff = popOff + 8;
    }
    std::uint64_t total() const { return mrcOff + 8; }
    std::uint64_t povBase(std::uint64_t slot) const { return base + slot * 64; }
    std::uint64_t el(std::uint64_t idx) const { return base + elemsOff + idx * elemSize; }
    std::uint64_t population() const { return img.get(base + popOff); }
    unsigned state(std::uint64_t slot) const {
        return static_cast<unsigned>((img.get(base + povFlagsOff + (slot >> 5) * 8) >> ((slot & 31) * 2)) & 3);
    }
    void setState(std::uint64_t slot, unsigned st) {
        const std::uint64_t w = base + povFlagsOff + (slot >> 5) * 8;
        std::uint64_t v = img.get(w);
        v &= ~(std::uint64_t{3} << ((slot & 31) * 2));
        v |= std::uint64_t{st} << ((slot & 31) * 2);
        img.put(w, v);
    }
    std::int64_t sget(std::uint64_t off) const { return static_cast<std::int64_t>(img.get(off)); }
    void sput(std::uint64_t off, std::int64_t v) { img.put(off, static_cast<std::uint64_t>(v)); }
    // field offsets inside the element
    std::uint64_t prio(std::uint64_t i) const { return el(i) + T; }
    std::uint64_t povIdx(std::uint64_t i) const { return el(i) + T + 8; }
    std::uint64_t par(std::uint64_t i) const { return el(i) + T + 16; }
    std::uint64_t left(std::uint64_t i) const { return el(i) + T + 24; }
    std::uint64_t right(std::uint64_t i) const { return el(i) + T + 32; }

    std::uint64_t findOrCreatePov(const std::vector<std::uint8_t>& id) {
        std::uint64_t home = 0;
        for (int j = 7; j >= 0; --j) home = (home << 8) | id[static_cast<std::size_t>(j)];
        std::uint64_t i = home & (L - 1);
        std::int64_t firstFree = -1;
        for (std::uint64_t n = 0; n < L; ++n, i = (i + 1) & (L - 1)) {
            const unsigned st = state(i);
            if (st == 0) {
                if (firstFree < 0) firstFree = static_cast<std::int64_t>(i);
                break;
            }
            if (st == 1) {
                bool eq = true;
                for (int j = 0; j < 32; ++j)
                    if (img.b.at(povBase(i) + static_cast<std::uint64_t>(j)) != id[static_cast<std::size_t>(j)]) eq = false;
                if (eq) return i;
            } else if (firstFree < 0) {
                firstFree = static_cast<std::int64_t>(i);
            }
        }
        const std::uint64_t slot = static_cast<std::uint64_t>(firstFree);
        img.putBytes(povBase(slot), id);
        img.put(povBase(slot) + 32, 0);
        sput(povBase(slot) + 40, -1);
        sput(povBase(slot) + 48, -1);
        sput(povBase(slot) + 56, -1);
        setState(slot, 1);
        return slot;
    }

    // returns the element index
    std::uint64_t add(const std::vector<std::uint8_t>& povId, const std::vector<std::uint8_t>& value, std::int64_t priority) {
        const std::uint64_t slot = findOrCreatePov(povId);
        const std::uint64_t pb = povBase(slot);
        const std::uint64_t n = population();
        img.put(base + popOff, n + 1);
        img.putBytes(el(n), value);
        sput(prio(n), priority);
        sput(povIdx(n), static_cast<std::int64_t>(slot));
        sput(par(n), -1);
        sput(left(n), -1);
        sput(right(n), -1);
        const std::uint64_t popPov = img.get(pb + 32);
        if (popPov == 0) {
            sput(pb + 40, static_cast<std::int64_t>(n));
            sput(pb + 48, static_cast<std::int64_t>(n));
            sput(pb + 56, static_cast<std::int64_t>(n));
        } else {
            std::int64_t cur = sget(pb + 56);
            while (true) {
                const std::uint64_t c = static_cast<std::uint64_t>(cur);
                if (sget(prio(c)) >= priority) { // go right
                    if (sget(right(c)) < 0) {
                        sput(right(c), static_cast<std::int64_t>(n));
                        sput(par(n), cur);
                        break;
                    }
                    cur = sget(right(c));
                } else {
                    if (sget(left(c)) < 0) {
                        sput(left(c), static_cast<std::int64_t>(n));
                        sput(par(n), cur);
                        break;
                    }
                    cur = sget(left(c));
                }
            }
            if (priority > sget(prio(static_cast<std::uint64_t>(sget(pb + 40))))) sput(pb + 40, static_cast<std::int64_t>(n));
            if (priority <= sget(prio(static_cast<std::uint64_t>(sget(pb + 48))))) sput(pb + 48, static_cast<std::int64_t>(n));
        }
        img.put(pb + 32, popPov + 1);
        return n;
    }

    // Removes the LAST element added when it is alone in its PoV (the PoV becomes a tombstone with stale data).
    void removeSoleLast() {
        const std::uint64_t n = population() - 1;
        const std::uint64_t slot = img.get(povIdx(n));
        for (std::uint64_t i = 0; i < elemSize; ++i) img.b.at(el(n) + i) = 0;
        img.put(base + popOff, n);
        img.put(povBase(slot) + 32, 0);
        setState(slot, 2);
        img.put(base + mrcOff, img.get(base + mrcOff) + 1);
    }
};

// LinkedList writer (addTail only; free list untouched).
struct ListW {
    Img& img;
    std::uint64_t base, L, valueSize, nodeSize, flagsOff, headOff, tailOff, freeOff, nextUnusedOff, popOff;

    ListW(Img& i, std::uint64_t base_, std::uint64_t L_, std::uint64_t vs) : img(i), base(base_), L(L_), valueSize(vs) {
        nodeSize = up(vs, 8) + 16;
        flagsOff = L * nodeSize;
        headOff = flagsOff + 8 * ((L + 63) / 64);
        tailOff = headOff + 8;
        freeOff = tailOff + 8;
        nextUnusedOff = freeOff + 8;
        popOff = nextUnusedOff + 8;
    }
    std::uint64_t total() const { return popOff + 8; }
    void initEmpty() { // like reset(): sentinels -1
        img.put(base + headOff, ~std::uint64_t{0});
        img.put(base + tailOff, ~std::uint64_t{0});
        img.put(base + freeOff, ~std::uint64_t{0});
    }
    void addTail(const std::vector<std::uint8_t>& value) {
        const std::uint64_t n = img.get(base + nextUnusedOff);
        const std::uint64_t pop = img.get(base + popOff);
        const std::uint64_t nb = base + n * nodeSize;
        img.putBytes(nb, value);
        const std::uint64_t nextOff = up(valueSize, 8);
        img.put(nb + nextOff, ~std::uint64_t{0});
        if (pop == 0) {
            img.put(nb + nextOff + 8, ~std::uint64_t{0});
            img.put(base + headOff, n);
        } else {
            const std::uint64_t t = img.get(base + tailOff);
            img.put(base + t * nodeSize + nextOff, n);
            img.put(nb + nextOff + 8, t);
        }
        img.put(base + tailOff, n);
        img.put(base + flagsOff + (n >> 6) * 8, img.get(base + flagsOff + (n >> 6) * 8) | (std::uint64_t{1} << (n & 63)));
        img.put(base + nextUnusedOff, n + 1);
        img.put(base + popOff, pop + 1);
    }
};

// ---- byte sources ---------------------------------------------------------------------------------------------------

// Sparse image: logical size may be huge; only written pages exist.
class SparseSource final : public ByteSource {
public:
    explicit SparseSource(std::uint64_t size) : size_(size) {}
    std::uint64_t size() const override { return size_; }
    void setSize(std::uint64_t s) { size_ = s; }
    void write(std::uint64_t off, const std::vector<std::uint8_t>& data) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            const std::uint64_t a = off + i;
            auto& pg = pages_[a / kPage];
            if (pg.empty()) pg.assign(kPage, 0);
            pg[a % kPage] = data[i];
        }
    }
    void write64(std::uint64_t off, std::uint64_t v) { write(off, le(v)); }
    std::size_t read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const override {
        if (offset >= size_) return 0;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
        std::memset(out, 0, n);
        const std::uint64_t first = offset / kPage, last = (offset + n - 1) / kPage;
        for (auto it = pages_.lower_bound(first); it != pages_.end() && it->first <= last; ++it) {
            const std::uint64_t pageStart = it->first * kPage;
            const std::uint64_t lo = std::max(pageStart, offset), hi = std::min(pageStart + kPage, offset + n);
            std::memcpy(out + (lo - offset), it->second.data() + (lo - pageStart), hi - lo);
        }
        return n;
    }
    bool isAllZero(std::uint64_t offset, std::uint64_t length) const override {
        if (length == 0) return true;
        if (offset > size_ || length > size_ - offset) return false;
        const std::uint64_t first = offset / kPage, last = (offset + length - 1) / kPage;
        for (auto it = pages_.lower_bound(first); it != pages_.end() && it->first <= last; ++it) {
            const std::uint64_t pageStart = it->first * kPage;
            const std::uint64_t lo = std::max(pageStart, offset), hi = std::min(pageStart + kPage, offset + length);
            for (std::uint64_t a = lo; a < hi; ++a)
                if (it->second[a - pageStart]) return false;
        }
        return true;
    }

private:
    static constexpr std::uint64_t kPage = 4096;
    std::uint64_t size_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> pages_;
};

// Identity codec for tests without K12: the checksum of the known vectors of docs/research/data/identity_vectors.txt
// (NULL_ID, contracts 1 and 2, all-0xFF); any other key gets checksum 0.
inline std::shared_ptr<IdentityCodec> testCodec() {
    return std::make_shared<BasicIdentityCodec>([](const std::uint8_t* k) -> std::uint32_t {
        bool zero = true, ones = true;
        for (int i = 1; i < 32; ++i) {
            zero = zero && k[i] == 0;
            ones = ones && k[i] == 0xFF;
        }
        if (zero && k[0] == 0) return 0x3c5c23;
        if (zero && k[0] == 1) return 0x0ce461;
        if (zero && k[0] == 2) return 0x470ef4;
        if (ones && k[0] == 0xFF) return 0x4372a7;
        return 0;
    });
}

// Convenience: decoder over an in-memory image.
inline std::shared_ptr<MemoryByteSource> mem(const Img& i) { return std::make_shared<MemoryByteSource>(i.b); }

} // namespace qstate::decode::testing
