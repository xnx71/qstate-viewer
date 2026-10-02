// StateDecoder::reveal: the exact position of every node of a path inside its parent's child list.
// The property under test: for every step, state.children(parent, view, offset = index, limit = 1) returns exactly that
// node, and the parent's childTotal equals the total of that listing.
#include <doctest/doctest.h>

#include <chrono>
#include <functional>

#include "qstate/decode/json.h"
#include "real_helpers.h"
#include "test_helpers.h"

using namespace qstate::decode;
using namespace qstate::decode::testing;
using nlohmann::json;

namespace {

std::uint64_t fieldOff(const SB& sb, TypeId rec, const std::string& name) {
    for (const auto& f : sb.s.types[rec].fields)
        if (f.name == name) return f.offset;
    FAIL("no field " << name);
    return 0;
}

// Verifies reveal(id) against the children listing; returns the reveal for further assertions.
NodeReveal checkReveal(const StateDecoder& dec, const NodeId& id, bool hideEmpty) {
    const NodeReveal r = dec.reveal(id, hideEmpty);
    CHECK(r.id == id);
    REQUIRE(!r.path.empty());
    CHECK(r.path[0].id.empty());
    CHECK(r.path[0].index == 0);
    for (std::size_t i = 1; i < r.path.size(); ++i) {
        const RevealStep& parent = r.path[i - 1];
        const RevealStep& s = r.path[i];
        if (s.index < 0) {
            CHECK(!r.blocked.empty());
            CHECK(i + 1 == r.path.size());
            break;
        }
        const ChildrenPage pg = dec.children(parent.id, {s.raw ? ChildView::Raw : ChildView::Logical,
                                                          static_cast<std::uint64_t>(s.index), 1, hideEmpty});
        REQUIRE_MESSAGE(pg.items.size() == 1, "node " << s.id << " index " << s.index << " under " << parent.id);
        CHECK_MESSAGE(pg.items[0].id == s.id, "under " << parent.id << " index " << s.index);
        CHECK_MESSAGE(pg.total == parent.childTotal, "total of " << parent.id);
    }
    if (r.blocked.empty()) {
        const ChildrenPage own = dec.children(id, {ChildView::Logical, 0, 1, hideEmpty});
        CHECK(r.path.back().childTotal == own.total);
    }
    return r;
}

// Walks the tree (logical and raw views) and reveals every node found.
std::size_t walkAll(const StateDecoder& dec, const NodeId& id, bool hideEmpty, int depth, std::size_t budget) {
    std::size_t count = 0;
    for (ChildView view : {ChildView::Logical, ChildView::Raw}) {
        const ChildrenPage pg = dec.children(id, {view, 0, 1000, hideEmpty});
        for (const NodeInfo& c : pg.items) {
            if (count >= budget) return count;
            checkReveal(dec, c.id, hideEmpty);
            ++count;
            if (depth > 0 && c.childCount > 0 && c.size < (1u << 20))
                count += walkAll(dec, c.id, hideEmpty, depth - 1, budget - count);
        }
    }
    return count;
}

struct Synth {
    SB sb;
    TypeId root = 0;
    Img img{0};
    std::uint64_t idxA[6] = {};
    std::shared_ptr<StateDecoder> dec;
    Synth() {
        const TypeId arr = sb.qpiArray(sb.u64(), 10);
        const TypeId ba = sb.bitArray(70);
        const TypeId map = sb.hashMap(sb.id(), sb.u64(), 16);
        const TypeId set = sb.hashSet(sb.u32(), 8);
        const TypeId val = sb.record("V", {{"owner", sb.u64()}, {"tag", sb.u8()}});
        const TypeId coll = sb.collection(val, 16);
        const TypeId ll = sb.linkedList(sb.u64(), 8);
        const TypeId nested = sb.record("N", {{"a", sb.u64()}, {"b", sb.array(sb.u32(), 3)}});
        root = sb.record("S", {{"pre", sb.u64()}, {"arr", arr}, {"ba", ba}, {"map", map}, {"set", set}, {"coll", coll},
                               {"ll", ll}, {"nested", nested}, {"cl", sb.array(nested, 4)}});
        img = Img(sb.size(root));
        // array: elements 1, 4, 5, 9 non-zero
        const std::uint64_t arrOff = fieldOff(sb, root, "arr");
        for (std::uint64_t i : {1, 4, 5, 9}) img.put(arrOff + i * 8, 100 + i);
        // bit array: bits 0, 3, 8, 64, 69
        const std::uint64_t baOff = fieldOff(sb, root, "ba");
        img.put(baOff, (1u << 0) | (1u << 3) | (1u << 8));
        img.put(baOff + 8, (1u << 0) | (1u << 5));
        // hash map with collisions and a tombstone
        const std::uint64_t mapOff = fieldOff(sb, root, "map");
        HashW h(img, mapOff, 16, 32, 8, 8, 8);
        h.set(3, idBytes(3), le(300));
        h.set(4, idBytes(19), le(1900));
        h.set(5, idBytes(35), le(3500));
        h.set(9, idBytes(1), le(11));
        h.set(15, idBytes(2), le(22));
        h.tombstone(7);
        h.counters(5, 1);
        // hash set
        const std::uint64_t setOff = fieldOff(sb, root, "set");
        HashW hs(img, setOff, 8, 4, 0, 1, 4);
        hs.set(0, le(5, 4));
        hs.set(2, le(77, 4));
        hs.set(6, le(88, 4));
        hs.counters(3, 0);
        // collection
        const std::uint64_t cOff = fieldOff(sb, root, "coll");
        CollW w(img, cOff, 16, 16);
        auto V = [](std::uint64_t owner, std::uint8_t tag) {
            auto v = le(owner);
            v.resize(16);
            v[8] = tag;
            return v;
        };
        const std::int64_t prA[] = {5, 9, 5, -3, 9, 7};
        for (int i = 0; i < 6; ++i) idxA[i] = w.add(idBytes(1), V(100 + static_cast<std::uint64_t>(i), 1), prA[i]);
        w.add(idBytes(17, 5), V(200, 2), 1);
        w.add(idBytes(9), V(300, 3), 10);
        w.add(idBytes(17, 5), V(201, 2), 1);
        // linked list 2 -> 0 -> 3 -> 1
        const std::uint64_t llOff = fieldOff(sb, root, "ll");
        ListW u(img, llOff, 8, 8);
        u.initEmpty();
        for (int i = 0; i < 4; ++i) u.addTail(le(100 + static_cast<std::uint64_t>(i)));
        const std::uint64_t no = up(8, 8);
        auto setNext = [&](std::uint64_t node, std::int64_t nxt) { img.put(llOff + node * u.nodeSize + no, static_cast<std::uint64_t>(nxt)); };
        auto setPrev = [&](std::uint64_t node, std::int64_t prv) { img.put(llOff + node * u.nodeSize + no + 8, static_cast<std::uint64_t>(prv)); };
        setNext(2, 0); setPrev(2, -1);
        setNext(0, 3); setPrev(0, 2);
        setNext(3, 1); setPrev(3, 0);
        setNext(1, -1); setPrev(1, 3);
        img.put(llOff + u.headOff, 2);
        img.put(llOff + u.tailOff, 1);
        // nested struct array: element 2 non-zero
        img.put(fieldOff(sb, root, "cl") + 2 * sb.size(nested), 7);
        dec = std::make_shared<StateDecoder>(sb.finish(), root, mem(img), [] {
            DecoderConfig c;
            c.identity = testCodec();
            return c;
        }());
    }
};

} // namespace

TEST_CASE("reveal: exact positions for every node kind of a synthetic state") {
    Synth s;
    const StateDecoder& dec = *s.dec;

    // struct field
    auto r = checkReveal(dec, "f:arr", false);
    REQUIRE(r.path.size() == 2);
    CHECK(r.path[1].index == 1);
    CHECK_FALSE(r.path[1].raw);
    CHECK(r.path[0].childTotal == dec.children("").total);
    CHECK(r.typeName == dec.node("f:arr").typeName);
    CHECK(r.offset == dec.node("f:arr").offset);
    CHECK(r.size == dec.node("f:arr").size);

    // QPI array: raw member of the array role vs logical element; hide-empty ranks
    auto e = checkReveal(dec, "f:arr/i:5", false);
    CHECK(e.path.back().index == 5);
    auto he = checkReveal(dec, "f:arr/i:5", true);
    CHECK(he.path.back().index == 2); // non-zero elements 1, 4, 5, 9 -> rank 2
    CHECK(he.path.back().childTotal == 0);
    auto hidden = dec.reveal("f:arr/i:2", true);
    CHECK(hidden.blocked == "hideEmpty");
    CHECK(hidden.path.back().index == -1);
    CHECK(dec.reveal("f:arr/i:2", false).blocked.empty());
    auto members = checkReveal(dec, "f:arr/f:_values", false);
    CHECK(members.path.back().raw);
    CHECK(members.path.back().index == 0);

    // bit array
    CHECK(checkReveal(dec, "f:ba/b:69", false).path.back().index == 69);
    CHECK(checkReveal(dec, "f:ba/b:64", true).path.back().index == 3); // set bits: 0, 3, 8, 64, 69
    CHECK(checkReveal(dec, "f:ba/b:69", true).path.back().index == 4);
    CHECK(dec.reveal("f:ba/b:1", true).blocked == "hideEmpty");

    // hash map / set: rank among live slots (slots 3, 4, 5, 9, 15)
    CHECK(checkReveal(dec, "f:map/e:3", false).path.back().index == 0);
    CHECK(checkReveal(dec, "f:map/e:9", false).path.back().index == 3);
    auto m15 = checkReveal(dec, "f:map/e:15/f:value", false);
    REQUIRE(m15.path.size() == 4);
    CHECK(m15.path[2].index == 4);
    CHECK(m15.path[2].childTotal == 2);
    CHECK(m15.path[3].index == 1);
    CHECK_THROWS_AS(dec.reveal("f:map/e:7", false), NotFoundError); // tombstone
    CHECK(checkReveal(dec, "f:set/e:6", false).path.back().index == 2);

    // collection: PoV slot rank, element position inside the PoV queue (priority order, FIFO among equals)
    // PoV A (slot 1) queue: idx1(9) idx4(9) idx5(7) idx0(5) idx2(5) idx3(-3)
    const std::uint64_t expectPos[6] = {3, 0, 4, 5, 1, 2};
    for (int i = 0; i < 6; ++i) {
        auto c = checkReveal(dec, "f:coll/e:" + std::to_string(s.idxA[i]), false);
        REQUIRE(c.path.size() == 4);
        CHECK(c.path[2].id == "f:coll/p:1");
        CHECK(c.path[2].index == 0);
        CHECK(c.path[2].childTotal == 6);
        CHECK(c.path[3].index == static_cast<std::int64_t>(expectPos[i]));
        CHECK(c.path[3].id == "f:coll/e:" + std::to_string(s.idxA[i]));
    }
    // the PoV spelled in the id gives the same result
    auto viaPov = checkReveal(dec, "f:coll/p:2", false);
    CHECK(viaPov.path.back().index == 1);
    CHECK(viaPov.path.back().childTotal == 2);
    CHECK(checkReveal(dec, "f:coll/p:9", false).path.back().index == 2);
    auto deep = checkReveal(dec, "f:coll/e:" + std::to_string(s.idxA[1]) + "/f:priority", false);
    REQUIRE(deep.path.size() == 5);
    CHECK(deep.path[4].index == 1);
    auto rawMember = checkReveal(dec, "f:coll/e:" + std::to_string(s.idxA[1]) + "/f:povIndex", false);
    CHECK(rawMember.path.back().raw);
    CHECK(rawMember.path.back().index == 2);
    // raw members of a container
    auto rawPovs = checkReveal(dec, "f:coll/f:_povs/i:4/f:value", false);
    CHECK(rawPovs.path[2].raw);

    // linked list order 2 -> 0 -> 3 -> 1
    CHECK(checkReveal(dec, "f:ll/e:2", false).path.back().index == 0);
    CHECK(checkReveal(dec, "f:ll/e:3", false).path.back().index == 2);
    CHECK(checkReveal(dec, "f:ll/e:1", false).path.back().index == 3);

    // nested struct arrays
    CHECK(checkReveal(dec, "f:cl/i:2/f:a", false).path.back().index == 0);
    CHECK(dec.reveal("f:cl/i:2/f:b/i:1", true).blocked == "hideEmpty");
    CHECK(checkReveal(dec, "f:cl/i:2", true).path.back().index == 0); // only element 2 of cl is non-zero
}

TEST_CASE("reveal: errors and the empty path") {
    Synth s;
    const StateDecoder& dec = *s.dec;
    auto root = dec.reveal("", false);
    REQUIRE(root.path.size() == 1);
    CHECK(root.path[0].childTotal == dec.children("").total);
    CHECK_THROWS_AS(dec.reveal("f:nope", false), NotFoundError);
    CHECK_THROWS_AS(dec.reveal("f:arr/i:10", false), NotFoundError);
    CHECK_THROWS_AS(dec.reveal("f:coll/e:15", false), NotFoundError); // beyond the population
    CHECK_THROWS_AS(dec.reveal("f:map/e:0", false), NotFoundError);
    CHECK_THROWS_AS(dec.reveal("garbage", false), NotFoundError);
}

TEST_CASE("reveal: every node of the synthetic state, both child views, with and without hide-empty") {
    Synth s;
    for (bool hideEmpty : {false, true}) {
        const std::size_t n = walkAll(*s.dec, "", hideEmpty, 6, 100000);
        CHECK(n > 150);
    }
}

TEST_CASE("locate: collection elements are listed below their PoV") {
    Synth s;
    const StateDecoder& dec = *s.dec;
    const std::uint64_t cOff = fieldOff(s.sb, s.root, "coll");
    CollW w(s.img, cOff, 16, 16);
    const NodeLocation loc = dec.locate(w.el(s.idxA[1]));
    REQUIRE(loc.path.size() >= 4);
    CHECK(loc.path[1].id == "f:coll");
    CHECK(loc.path[2].id == "f:coll/p:1");
    CHECK(loc.path[3].id == "f:coll/e:" + std::to_string(s.idxA[1]));
    CHECK(loc.id == "f:coll/e:" + std::to_string(s.idxA[1]) + "/f:value/f:owner");
}

TEST_CASE("reveal: huge sparse HashMap (2^24 slots) uses the cached scan, no page walking") {
    SB sb;
    const TypeId map = sb.hashMap(sb.u64(), sb.u64(), 1u << 24);
    const TypeId root = sb.record("S", {{"map", map}});
    const std::uint64_t L = 1u << 24;
    const std::uint64_t elemSize = 16;
    const std::uint64_t flagsOff = up(L * elemSize, 8);
    auto src = std::make_shared<SparseSource>(sb.size(root));
    std::vector<std::uint64_t> slots;
    for (std::uint64_t i = 0; i < 5000; ++i) slots.push_back(i * 3301 + 17);
    std::vector<std::uint64_t> words((L * 2 + 63) / 64, 0);
    std::map<std::uint64_t, std::uint64_t> flagWords;
    for (std::uint64_t slot : slots) flagWords[slot >> 5] |= std::uint64_t{1} << ((slot & 31) * 2);
    for (auto& [w, v] : flagWords) src->write64(flagsOff + w * 8, v);
    src->write64(flagsOff + 8 * ((L * 2 + 63) / 64), slots.size());
    StateDecoder dec(sb.finish(), root, src);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t k : {0ull, 1ull, 2499ull, 4999ull}) {
        const NodeReveal r = dec.reveal("f:map/e:" + std::to_string(slots[k]), false);
        REQUIRE(r.path.size() == 3);
        CHECK(r.path[2].index == static_cast<std::int64_t>(k));
        CHECK(r.path[1].childTotal == slots.size());
    }
    const auto page = dec.children("f:map", {ChildView::Logical, 4999, 1, false});
    REQUIRE(page.items.size() == 1);
    CHECK(page.items[0].id == "f:map/e:" + std::to_string(slots[4999]));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 3000);
}

TEST_CASE("real QX: reveal of collection elements / PoVs against the children listing") {
    RealContract c = openReal(1);
    if (!c.ok) {
        MESSAGE("skipped: QSTATE_TEST_STATE_DIR / QSTATE_SOURCE_DIR not set");
        return;
    }
    const StateDecoder& dec = *c.dec;
    for (const char* coll : {"f:_assetOrders", "f:_entityOrders"}) {
        const ChildrenPage povs = dec.children(coll, {ChildView::Logical, 0, 1000, false});
        REQUIRE(povs.total > 0);
        std::size_t checked = 0;
        for (std::size_t pi = 0; pi < povs.items.size(); pi += 7) {
            const NodeInfo& pov = povs.items[pi];
            const NodeReveal pr = checkReveal(dec, pov.id, false);
            CHECK(pr.path.back().index == static_cast<std::int64_t>(pi));
            const ChildrenPage els = dec.children(pov.id, {ChildView::Logical, 0, 1000, false});
            for (std::size_t ei = 0; ei < els.items.size(); ei += (ei < 3 ? 1 : 29)) {
                const NodeReveal er = checkReveal(dec, els.items[ei].id, false);
                REQUIRE(er.path.size() >= 3);
                CHECK(er.path.back().index == static_cast<std::int64_t>(ei));
                CHECK(er.path[er.path.size() - 2].id == pov.id);
                ++checked;
            }
        }
        CHECK(checked > 50);
    }
    // the deepest leaf of a located byte
    const NodeLocation loc = dec.locate(dec.node("f:_assetOrders").offset + 302514192 / 2);
    checkReveal(dec, loc.id, false);
}

TEST_CASE("real QBOND: reveal of hash map entries (rank among live slots)") {
    RealContract c = openReal(17);
    if (!c.ok) {
        MESSAGE("skipped: QSTATE_TEST_STATE_DIR / QSTATE_SOURCE_DIR not set");
        return;
    }
    const StateDecoder& dec = *c.dec;
    const ChildrenPage root = dec.children("", {ChildView::Logical, 0, 1000, false});
    std::size_t maps = 0;
    for (const NodeInfo& n : root.items) {
        if (n.kind != NodeKind::HashMap && n.kind != NodeKind::HashSet) continue;
        const ChildrenPage entries = dec.children(n.id, {ChildView::Logical, 0, 1000, false});
        for (std::size_t i = 0; i < entries.items.size(); i += 3) {
            const NodeReveal r = checkReveal(dec, entries.items[i].id, false);
            CHECK(r.path.back().index == static_cast<std::int64_t>(i));
        }
        if (!entries.items.empty()) ++maps;
    }
    CHECK(maps > 0);
}
