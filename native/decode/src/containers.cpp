// Container data access: flag scans, PoV tables, BST walks, linked list order, non-empty element indexes.
#include <bit>

#include "gather.h"
#include "impl.h"

namespace qstate::decode {

namespace {

std::uint64_t loadLe64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (std::endian::native == std::endian::big) v = __builtin_bswap64(v);
    return v;
}

std::string joinWarnings(const std::vector<std::string>& w) {
    std::string s;
    for (const std::string& x : w) {
        if (!s.empty()) s += "; ";
        s += x;
    }
    return s;
}

bool anyNonZero(const std::uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w) return true;
    }
    for (; i < n; ++i)
        if (p[i]) return true;
    return false;
}

} // namespace

const PovRec* CollInfo::findPov(std::uint64_t slot) const {
    auto it = std::lower_bound(povs.begin(), povs.end(), slot,
                               [](const PovRec& p, std::uint64_t s) { return p.slot < s; });
    return (it != povs.end() && it->slot == slot) ? &*it : nullptr;
}

std::optional<std::uint64_t> StateDecoder::Impl::readU64(std::uint64_t off) const {
    std::uint8_t b[8];
    if (read(off, 8, b) != 8) return std::nullopt;
    return loadLe64(b);
}

std::shared_ptr<const Flags2Scan> StateDecoder::Impl::scanFlags2(std::uint64_t flagsOff, std::uint64_t capacity,
                                                                  const Query& q) const {
    const std::string k = key(q, "f2:" + std::to_string(flagsOff) + ":" + std::to_string(capacity));
    if (auto hit = cache->getAs<Flags2Scan>(k)) return hit;

    auto scan = std::make_shared<Flags2Scan>();
    const std::uint64_t words = (capacity * 2 + 63) / 64;
    constexpr std::uint64_t kBlockWords = 1u << 17; // 1 MiB
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min(words, kBlockWords) * 8));
    constexpr std::uint64_t kEven = 0x5555555555555555ull;
    for (std::uint64_t w0 = 0; w0 < words; w0 += kBlockWords) {
        checkCancel(q);
        const std::uint64_t n = std::min(kBlockWords, words - w0);
        const std::size_t want = static_cast<std::size_t>(n * 8);
        const std::size_t got = read(flagsOff + w0 * 8, want, buf.data());
        if (got < want) {
            scan->truncated = true;
            std::memset(buf.data() + got, 0, want - got);
        }
        for (std::uint64_t i = 0; i < n; ++i) {
            std::uint64_t word = loadLe64(buf.data() + i * 8);
            if (!word) continue;
            const std::uint64_t absWord = w0 + i;
            if (absWord == words - 1 && (capacity % 32) != 0)
                word &= (std::uint64_t{1} << (2 * (capacity % 32))) - 1;
            const std::uint64_t lo = word & kEven, hi = (word >> 1) & kEven;
            std::uint64_t liveMask = lo & ~hi;
            scan->tombstones += static_cast<std::uint64_t>(std::popcount(hi & ~lo));
            scan->invalid += static_cast<std::uint64_t>(std::popcount(lo & hi));
            while (liveMask) {
                const int b = std::countr_zero(liveMask);
                scan->live.push_back(static_cast<std::uint32_t>(absWord * 32 + static_cast<std::uint64_t>(b / 2)));
                liveMask &= liveMask - 1;
            }
        }
    }
    scan->live.shrink_to_fit();
    cache->put(k, scan, sizeof(Flags2Scan) + scan->live.capacity() * 4);
    return scan;
}

HashInfo StateDecoder::Impl::hashInfo(const Ref& c, const Query& q) const {
    const TypeX& tx = idx.x(c.type);
    const HashLayout& h = tx.hash;
    HashInfo info;
    info.flags = scanFlags2(c.offset + h.flagsOff, h.capacity, q);
    const Flags2Scan& s = *info.flags;
    ContainerStats& st = info.stats;
    st.capacity = h.capacity;
    st.population = s.live.size();
    st.removed = s.tombstones;
    std::vector<std::string> w;
    if (s.truncated || !inFile(c.offset, tx.size)) w.push_back("container extends beyond the end of the file");
    const auto pop = readU64(c.offset + h.popOff);
    const auto mrc = readU64(c.offset + h.mrcOff);
    if (pop && *pop != s.live.size())
        w.push_back("stored _population (" + std::to_string(*pop) + ") differs from the number of occupied slots (" +
                    std::to_string(s.live.size()) + ")");
    if (s.invalid) w.push_back(std::to_string(s.invalid) + " slots have the unused flag state 0b11");
    if (mrc && s.tombstones > *mrc)
        w.push_back("marked slots (" + std::to_string(s.tombstones) + ") exceed _markRemovalCounter (" +
                    std::to_string(*mrc) + ")");
    st.warning = joinWarnings(w);
    return info;
}

std::shared_ptr<const CollInfo> StateDecoder::Impl::collInfo(const Ref& c, const Query& q) const {
    const std::string k = key(q, "ci:" + std::to_string(c.offset) + ":" + std::to_string(c.type));
    if (auto hit = cache->getAs<CollInfo>(k)) return hit;

    const TypeX& tx = idx.x(c.type);
    const CollectionLayout& l = tx.coll;
    auto ci = std::make_shared<CollInfo>();
    ci->flags = scanFlags2(c.offset + l.povFlagsOff, l.capacity, q);
    const Flags2Scan& fs = *ci->flags;

    ci->povs.reserve(fs.live.size());
    const std::uint64_t povBase = c.offset + l.povsOff;
    gatherRows(
        *src, fs.live.size(), [&](std::uint64_t i) { return povBase + std::uint64_t{fs.live[i]} * l.povSize; },
        static_cast<std::size_t>(l.povSize),
        [&](std::uint64_t i, const std::uint8_t* d, std::size_t avail) {
            PovRec r;
            r.slot = fs.live[i];
            if (avail < l.povSize) return; // PoV cut by the end of the file: skip
            std::memcpy(r.id, d + l.povValueOff, 32);
            r.population = loadLe64(d + l.povPopOff);
            r.head = static_cast<std::int64_t>(loadLe64(d + l.povHeadOff));
            r.tail = static_cast<std::int64_t>(loadLe64(d + l.povTailOff));
            r.root = static_cast<std::int64_t>(loadLe64(d + l.povRootOff));
            ci->povs.push_back(r);
        },
        q.cancel);
    for (const PovRec& p : ci->povs) ci->popSum += p.population;

    std::vector<std::string> w;
    const auto pop = readU64(c.offset + l.popOff);
    ci->popReadable = pop.has_value();
    ci->storedPop = pop.value_or(0);
    if (const auto mrc = readU64(c.offset + l.mrcOff)) ci->storedMrc = *mrc;
    std::uint64_t n = std::min(ci->storedPop, l.capacity);
    if (ci->storedPop > l.capacity) w.push_back("_population exceeds the capacity");
    // elements fully inside the file
    const std::uint64_t elemBase = c.offset + l.elemsOff;
    const std::uint64_t fsz = src->size();
    const std::uint64_t fit = fsz > elemBase && l.elemSize ? (fsz - elemBase) / l.elemSize : 0;
    if (n > fit) {
        n = fit;
        w.push_back("elements extend beyond the end of the file");
    }
    ci->elemCount = n;
    if (fs.truncated && fs.live.empty()) w.push_back("PoV table extends beyond the end of the file");
    if (pop && ci->popSum != ci->storedPop)
        w.push_back("sum of PoV populations (" + std::to_string(ci->popSum) + ") differs from _population (" +
                    std::to_string(ci->storedPop) + ")");
    if (fs.invalid) w.push_back(std::to_string(fs.invalid) + " PoV slots have the unused flag state 0b11");
    ci->stats.capacity = l.capacity;
    ci->stats.population = ci->elemCount;
    ci->stats.removed = fs.tombstones;
    ci->stats.povs = ci->povs.size();
    ci->stats.warning = joinWarnings(w);

    cache->put(k, ci, sizeof(CollInfo) + ci->povs.capacity() * sizeof(PovRec) + fs.live.capacity() * 0);
    return ci;
}

bool StateDecoder::Impl::slotLive(const Ref& c, std::uint64_t flagsOff, std::uint64_t slot) const {
    const std::uint64_t word = slot >> 5;
    const auto w = readU64(c.offset + flagsOff + word * 8);
    if (!w) return false;
    return ((*w >> ((slot & 31) * 2)) & 3) == 1;
}

bool StateDecoder::Impl::listNodeOccupied(const ListLayout& l, std::uint64_t base, std::uint64_t node) const {
    const auto w = readU64(base + l.flagsOff + (node >> 6) * 8);
    if (!w) return false;
    return ((*w >> (node & 63)) & 1) != 0;
}

ListInfo StateDecoder::Impl::listInfo(const Ref& c, const Query& q) const {
    const ListLayout& l = idx.x(c.type).list;
    ListInfo li;
    const auto pop = readU64(c.offset + l.popOff);
    const auto head = readS64(c.offset + l.headOff);
    const auto tail = readS64(c.offset + l.tailOff);
    li.readable = pop && head && tail;
    li.storedPopulation = pop.value_or(0);
    li.population = std::min(li.storedPopulation, l.capacity);
    li.head = head.value_or(kNullIndex);
    li.tail = tail.value_or(kNullIndex);
    // popcount of the occupied bits
    const std::uint64_t words = (l.capacity + 63) / 64;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min<std::uint64_t>(words, 1u << 17) * 8));
    bool truncated = false;
    for (std::uint64_t w0 = 0; w0 < words; w0 += (1u << 17)) {
        checkCancel(q);
        const std::uint64_t n = std::min<std::uint64_t>(1u << 17, words - w0);
        const std::size_t want = static_cast<std::size_t>(n * 8);
        const std::size_t got = read(c.offset + l.flagsOff + w0 * 8, want, buf.data());
        if (got < want) {
            truncated = true;
            std::memset(buf.data() + got, 0, want - got);
        }
        for (std::uint64_t i = 0; i < n; ++i) {
            std::uint64_t word = loadLe64(buf.data() + i * 8);
            if (w0 + i == words - 1 && (l.capacity % 64) != 0) word &= (std::uint64_t{1} << (l.capacity % 64)) - 1;
            li.flagPopulation += static_cast<std::uint64_t>(std::popcount(word));
        }
    }
    std::vector<std::string> w;
    if (truncated || !inFile(c.offset, idx.x(c.type).size)) w.push_back("container extends beyond the end of the file");
    if (li.readable && li.storedPopulation > l.capacity) w.push_back("_population exceeds the capacity");
    if (li.readable && li.flagPopulation != li.storedPopulation)
        w.push_back("stored _population (" + std::to_string(li.storedPopulation) +
                    ") differs from the number of occupied nodes (" + std::to_string(li.flagPopulation) + ")");
    li.stats.capacity = l.capacity;
    li.stats.population = li.population;
    li.stats.warning = joinWarnings(w);
    return li;
}

std::shared_ptr<const IndexList> StateDecoder::Impl::listOrder(const Ref& c, const Query& q) const {
    const std::string k = key(q, "lo:" + std::to_string(c.offset) + ":" + std::to_string(c.type));
    if (auto hit = cache->getAs<IndexList>(k)) return hit;
    const ListLayout& l = idx.x(c.type).list;
    const ListInfo li = listInfo(c, q);
    auto out = std::make_shared<IndexList>();
    // An empty list never looks at head/tail: a never-used list has head == tail == 0, not -1.
    if (li.readable && li.population > 0) {
        std::int64_t cur = li.head;
        std::uint64_t n = 0;
        while (cur != kNullIndex && n < li.population) {
            if ((n & 0xFFF) == 0) checkCancel(q);
            if (cur < 0 || static_cast<std::uint64_t>(cur) >= l.capacity) break;
            const std::uint64_t node = static_cast<std::uint64_t>(cur);
            if (!listNodeOccupied(l, c.offset, node)) break;
            out->ids.push_back(static_cast<std::uint32_t>(node));
            ++n;
            const auto next = readS64(c.offset + l.nodesOff + node * l.nodeSize + l.nextOff);
            if (!next) break;
            cur = *next;
        }
    }
    cache->put(k, out, sizeof(IndexList) + out->ids.capacity() * 4);
    return out;
}

std::shared_ptr<const IndexList> StateDecoder::Impl::nonEmptyIndex(std::uint64_t offset, std::uint64_t elemSize,
                                                                   std::uint64_t count, const Query& q) const {
    const std::string k = key(q, "ne:" + std::to_string(offset) + ":" + std::to_string(elemSize) + ":" +
                                     std::to_string(count));
    if (auto hit = cache->getAs<IndexList>(k)) return hit;
    auto out = std::make_shared<IndexList>();
    if (elemSize > 0) {
        constexpr std::uint64_t kChunkBytes = 1u << 20;
        const std::uint64_t perChunk = std::max<std::uint64_t>(1, kChunkBytes / elemSize);
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min(perChunk, count) * elemSize));
        for (std::uint64_t i0 = 0; i0 < count; i0 += perChunk) {
            checkCancel(q);
            const std::uint64_t n = std::min(perChunk, count - i0);
            const std::size_t want = static_cast<std::size_t>(n * elemSize);
            const std::size_t got = read(offset + i0 * elemSize, want, buf.data());
            const std::uint64_t full = got / elemSize; // elements entirely inside the file
            if (elemSize % 8 == 0 && !anyNonZero(buf.data(), static_cast<std::size_t>(full * elemSize))) {
                if (full < n) break;
                continue;
            }
            for (std::uint64_t i = 0; i < full; ++i)
                if (anyNonZero(buf.data() + i * elemSize, static_cast<std::size_t>(elemSize)))
                    out->ids.push_back(static_cast<std::uint32_t>(i0 + i));
            if (full < n) break;
        }
    }
    out->ids.shrink_to_fit();
    cache->put(k, out, sizeof(IndexList) + out->ids.capacity() * 4);
    return out;
}

std::vector<std::uint32_t> StateDecoder::Impl::povWalk(const Ref& c, const CollInfo& ci, const PovRec& pov,
                                                       std::uint64_t upTo, const Query& q) const {
    const CollectionLayout& l = idx.x(c.type).coll;
    std::vector<std::uint32_t> out;
    const std::uint64_t limit = std::min<std::uint64_t>(upTo, pov.population);
    const std::uint64_t n = ci.elemCount;
    const std::uint64_t base = c.offset + l.elemsOff;
    struct Links {
        std::int64_t parent, left, right;
    };
    auto links = [&](std::int64_t i, Links& out2) -> bool {
        if (i < 0 || static_cast<std::uint64_t>(i) >= n) return false;
        std::uint8_t buf[512];
        const std::uint64_t lo = std::min({l.parentOff, l.leftOff, l.rightOff});
        const std::uint64_t hi = std::max({l.parentOff, l.leftOff, l.rightOff}) + 8;
        const std::uint64_t eo = base + static_cast<std::uint64_t>(i) * l.elemSize;
        if (hi - lo > sizeof buf) return false;
        if (read(eo + lo, static_cast<std::size_t>(hi - lo), buf) != hi - lo) return false;
        out2.parent = static_cast<std::int64_t>(loadLe64(buf + (l.parentOff - lo)));
        out2.left = static_cast<std::int64_t>(loadLe64(buf + (l.leftOff - lo)));
        out2.right = static_cast<std::int64_t>(loadLe64(buf + (l.rightOff - lo)));
        return true;
    };
    std::int64_t cur = pov.head;
    std::uint64_t steps = 0;
    const std::uint64_t maxSteps = 4 * n + 64;
    while (cur != kNullIndex && out.size() < limit) {
        if ((steps & 0xFFF) == 0) checkCancel(q);
        if (cur < 0 || static_cast<std::uint64_t>(cur) >= n) break;
        out.push_back(static_cast<std::uint32_t>(cur));
        // in-order successor (towards lower priority)
        Links L;
        if (!links(cur, L)) break;
        std::int64_t next = kNullIndex;
        if (L.right != kNullIndex) {
            std::int64_t j = L.right;
            bool ok = true;
            while (true) {
                if (++steps > maxSteps) {
                    ok = false;
                    break;
                }
                Links lj;
                if (!links(j, lj)) {
                    ok = false;
                    break;
                }
                if (lj.left == kNullIndex) break;
                j = lj.left;
            }
            if (!ok) break;
            next = j;
        } else {
            std::int64_t child = cur, p = L.parent;
            bool ok = true;
            while (p != kNullIndex) {
                if (++steps > maxSteps) {
                    ok = false;
                    break;
                }
                Links lp;
                if (!links(p, lp)) {
                    ok = false;
                    break;
                }
                if (lp.right != child) break;
                child = p;
                p = lp.parent;
            }
            if (!ok) break;
            next = p;
        }
        cur = next;
        if (++steps > maxSteps) break;
    }
    return out;
}

std::shared_ptr<const IndexList> StateDecoder::Impl::povOrder(const Ref& c, const CollInfo& ci, const PovRec& pov,
                                                              const Query& q) const {
    const std::string k = key(q, "po:" + std::to_string(c.offset) + ":" + std::to_string(pov.slot));
    if (auto hit = cache->getAs<IndexList>(k)) return hit;
    auto out = std::make_shared<IndexList>();
    out->ids = povWalk(c, ci, pov, pov.population, q);
    out->ids.shrink_to_fit();
    cache->put(k, out, sizeof(IndexList) + out->ids.capacity() * 4);
    return out;
}

ContainerStats StateDecoder::Impl::containerStats(const Ref& r, const Query& q) const {
    const TypeX& tx = idx.x(r.type);
    switch (tx.cls) {
    case Cls::HashMap:
    case Cls::HashSet: return hashInfo(r, q).stats;
    case Cls::Collection: return collInfo(r, q)->stats;
    case Cls::LinkedList: return listInfo(r, q).stats;
    default: return {};
    }
}

} // namespace qstate::decode
