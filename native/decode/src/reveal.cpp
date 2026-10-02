// Exact child positions for "reveal this node in the tree": for every node on the path, its index inside the parent's
// child list as state.children would return it. Everything is computed from cached scans (flag scans, PoV tables,
// PoV queue order, list order, non-empty indexes), never by paging through children.
#include <bit>
#include <charconv>

#include "impl.h"

namespace qstate::decode {

using Impl = StateDecoder::Impl;

namespace {

bool parseU64(std::string_view s, std::uint64_t& v) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// Rank of `x` inside the ascending vector (number of smaller entries), or nullopt when x is not an entry.
std::optional<std::uint64_t> rankAscending(const std::vector<std::uint32_t>& v, std::uint64_t x) {
    auto it = std::lower_bound(v.begin(), v.end(), x, [](std::uint32_t a, std::uint64_t b) { return a < b; });
    if (it == v.end() || *it != x) return std::nullopt;
    return static_cast<std::uint64_t>(it - v.begin());
}

std::optional<std::uint64_t> memberIndex(const std::vector<Member>& members, std::string_view name) {
    for (std::size_t i = 0; i < members.size(); ++i)
        if (members[i].name == name) return i;
    return std::nullopt;
}

} // namespace

std::optional<Ref> Impl::povOfElement(const Ref& c, const Ref& element, const Query& q) const {
    const CollectionLayout& l = idx.x(c.type).coll;
    const auto slot = readU64(element.offset + l.povIdxOff);
    if (!slot || *slot >= l.capacity) return std::nullopt;
    if (!collInfo(c, q)->findPov(*slot)) return std::nullopt;
    return povRef(c, *slot);
}

Impl::ChildPos Impl::childPosition(const Ref& p, char kind, std::string_view arg, bool hideEmpty, const Query& q) const {
    ChildPos pos;
    auto notListed = [&](const char* why) {
        pos.listed = false;
        pos.blocked = why;
        return pos;
    };
    auto fail = [&]() -> ChildPos {
        throw NotFoundError("no child '" + std::string(1, kind) + ":" + std::string(arg) + "' under node '" + p.id + "'");
    };
    std::uint64_t n = 0;

    switch (p.rk) {
    case RK::Bit: return fail();
    case RK::MapEntry:
        if (kind == 'f' && arg == "key") {
            pos.index = 0;
            return pos;
        }
        if (kind == 'f' && arg == "value") {
            pos.index = 1;
            return pos;
        }
        return fail();
    case RK::CollEntry: {
        if (kind != 'f') return fail();
        if (arg == "value" || arg == "priority" || arg == "pov") {
            pos.index = arg == "value" ? 0 : arg == "priority" ? 1 : 2;
            return pos;
        }
        const auto mi = memberIndex(idx.x(idx.x(p.type).coll.elementType).members, arg);
        if (!mi) return fail();
        pos.index = *mi;
        pos.raw = true;
        return pos;
    }
    case RK::Pov: {
        if (kind != 'e' || !parseU64(arg, n)) return fail();
        const Ref c = containerOf(p);
        const auto ci = collInfo(c, q);
        const PovRec* pov = ci->findPov(p.index);
        if (!pov || n >= ci->elemCount) return notListed("orphan");
        const auto order = povOrder(c, *ci, *pov, q);
        const auto it = std::find(order->ids.begin(), order->ids.end(), static_cast<std::uint32_t>(n));
        if (it == order->ids.end()) return notListed("orphan");
        pos.index = static_cast<std::uint64_t>(it - order->ids.begin());
        return pos;
    }
    case RK::Plain: break;
    }

    const TypeX& tx = idx.x(p.type);
    switch (kind) {
    case 'f': {
        const auto mi = memberIndex(tx.members, arg);
        if (!mi) return fail();
        pos.index = *mi;
        pos.raw = tx.cls != Cls::Record && tx.cls != Cls::Union; // container members only exist in the raw view
        return pos;
    }
    case 'i': {
        if (!parseU64(arg, n) || n >= tx.count) return fail();
        if (hideEmpty) {
            const auto ne = nonEmptyIndex(p.offset, tx.elemSize, tx.count, q);
            const auto r = rankAscending(ne->ids, n);
            if (!r) return notListed("hideEmpty");
            pos.index = *r;
        } else {
            pos.index = n;
        }
        return pos;
    }
    case 'b': {
        if (!parseU64(arg, n) || n >= tx.count) return fail();
        if (!hideEmpty) {
            pos.index = n;
            return pos;
        }
        // rank among the set bits
        std::vector<std::uint8_t> buf(static_cast<std::size_t>((tx.count + 7) / 8));
        buf.resize(read(p.offset, buf.size(), buf.data()));
        if (n / 8 >= buf.size() || !((buf[n / 8] >> (n % 8)) & 1u)) return notListed("hideEmpty");
        std::uint64_t rank = 0;
        for (std::uint64_t b = 0; b < n / 8; ++b) rank += static_cast<std::uint64_t>(std::popcount(buf[b]));
        rank += static_cast<std::uint64_t>(std::popcount(static_cast<unsigned>(buf[n / 8] & ((1u << (n % 8)) - 1u))));
        pos.index = rank;
        return pos;
    }
    case 'e': {
        if (!parseU64(arg, n)) return fail();
        switch (tx.cls) {
        case Cls::HashMap:
        case Cls::HashSet: {
            const auto r = rankAscending(hashInfo(p, q).flags->live, n);
            if (!r) return fail();
            pos.index = *r;
            return pos;
        }
        case Cls::LinkedList: {
            const auto order = listOrder(p, q);
            const auto it = std::find(order->ids.begin(), order->ids.end(), static_cast<std::uint32_t>(n));
            if (it == order->ids.end()) return fail();
            pos.index = static_cast<std::uint64_t>(it - order->ids.begin());
            return pos;
        }
        default: return fail(); // collection elements are positioned below their PoV (see reveal)
        }
    }
    case 'p': {
        if (tx.cls != Cls::Collection || !parseU64(arg, n)) return fail();
        const auto ci = collInfo(p, q);
        auto it = std::lower_bound(ci->povs.begin(), ci->povs.end(), n,
                                   [](const PovRec& a, std::uint64_t s) { return a.slot < s; });
        if (it == ci->povs.end() || it->slot != n) return fail();
        pos.index = static_cast<std::uint64_t>(it - ci->povs.begin());
        return pos;
    }
    default: return fail();
    }
}

NodeReveal Impl::reveal(const NodeId& id, bool hideEmpty, const Query& q) const {
    NodeReveal out;
    std::vector<Ref> refs;
    auto push = [&](Ref r, const ChildPos* pos) {
        finalize(r);
        RevealStep s;
        s.id = r.id;
        s.label = r.label;
        if (pos) {
            s.index = pos->listed ? static_cast<std::int64_t>(pos->index) : -1;
            s.raw = pos->raw;
        }
        out.path.push_back(std::move(s));
        refs.push_back(std::move(r));
    };
    push(rootRef(), nullptr);

    std::size_t at = 0;
    while (at < id.size() && out.blocked.empty()) {
        checkCancel(q);
        std::size_t end = id.find('/', at);
        if (end == std::string::npos) end = id.size();
        const std::string_view seg = std::string_view(id).substr(at, end - at);
        at = end + 1;
        const Ref cur = refs.back();
        Ref child = childBySegment(cur, seg, q); // validates the segment (NotFoundError)
        const char kind = seg[0];
        const std::string_view arg = seg.substr(2);

        if (cur.rk == RK::Plain && idx.x(cur.type).cls == Cls::Collection && kind == 'e') {
            // elements are children of their PoV in the logical view
            const auto pov = povOfElement(cur, child, q);
            if (!pov) {
                ChildPos np;
                np.listed = false;
                np.blocked = "orphan";
                push(std::move(child), &np);
                out.blocked = np.blocked;
                break;
            }
            const ChildPos pp = childPosition(cur, 'p', std::to_string(pov->index), hideEmpty, q);
            push(*pov, &pp);
            const Ref povRefCopy = refs.back();
            const ChildPos ep = childPosition(povRefCopy, 'e', arg, hideEmpty, q);
            push(std::move(child), &ep);
            if (!ep.listed) out.blocked = ep.blocked;
            continue;
        }
        const ChildPos pos = childPosition(cur, kind, arg, hideEmpty, q);
        push(std::move(child), &pos);
        if (!pos.listed) out.blocked = pos.blocked;
    }

    // child totals: the view in which the next step lives
    for (std::size_t i = 0; i < out.path.size(); ++i) {
        if (!out.blocked.empty() && i + 1 == out.path.size()) break; // the unlisted node itself
        const bool raw = i + 1 < out.path.size() && out.path[i + 1].raw;
        out.path[i].childTotal =
            listChildren(refs[i], raw ? ChildView::Raw : ChildView::Logical, 0, 1, hideEmpty, q).total;
    }

    const Ref& last = refs.back();
    out.id = last.id;
    out.offset = last.offset;
    out.size = last.size;
    out.typeName = typeNameOfRef(last);
    return out;
}

} // namespace qstate::decode
