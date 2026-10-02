#include <doctest/doctest.h>

#include <chrono>
#include <random>
#include <set>

#include "qstate/decode/json.h"
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

std::string idText(std::uint64_t w0) {
    // identity of (w0,0,0,0) using the test codec
    std::vector<std::uint8_t> b = idBytes(w0);
    auto codec = testCodec();
    return codec->encode(b.data());
}

TableRequest req(const std::string& id, std::uint64_t off = 0, std::uint64_t limit = 100) {
    TableRequest r;
    r.id = id;
    r.offset = off;
    r.limit = limit;
    return r;
}

} // namespace

TEST_CASE("HashMap<id,uint64,16>: live entries, collisions, tombstones, counters") {
    SB sb;
    const TypeId map = sb.hashMap(sb.id(), sb.u64(), 16);
    const TypeId root = sb.record("S", {{"pre", sb.u64()}, {"map", map}, {"post", sb.u64()}});
    const std::uint64_t base = fieldOff(sb, root, "map");
    CHECK(sb.size(map) == 16 * 40 + 8 + 16); // up(L*S(Element),8) + 8*W2(L) + 16
    Img img(sb.size(root));
    HashW h(img, base, 16, 32, 8, 8, 8);
    CHECK(h.total() == sb.size(map));
    // keys 3, 19, 35 collide on home slot 3 -> slots 3,4,5 ; key 9 -> slot 9, tombstone at 7
    h.set(3, idBytes(3), le(300));
    h.set(4, idBytes(19), le(1900));
    h.set(5, idBytes(35), le(3500));
    h.set(9, idBytes(1), le(11));
    img.putBytes(base + 7 * 40, idBytes(0));
    h.setState(7, 2);
    h.counters(4, 6);
    StateDecoder dec(sb.finish(), root, mem(img), [] {
        DecoderConfig c;
        c.identity = testCodec();
        return c;
    }());

    auto n = dec.node("f:map");
    CHECK(n.kind == NodeKind::HashMap);
    REQUIRE(n.container);
    CHECK(n.container->capacity == 16);
    CHECK(*n.container->population == 4);
    CHECK(*n.container->removed == 1);
    CHECK(n.container->warning.empty());
    CHECK(n.childCount == 4);
    CHECK(n.rawChildCount == 4);
    CHECK(n.tabular);

    auto page = dec.children("f:map");
    REQUIRE(page.items.size() == 4);
    CHECK(page.total == 4);
    CHECK(page.items[0].id == "f:map/e:3");
    CHECK(page.items[1].id == "f:map/e:4");
    CHECK(page.items[3].id == "f:map/e:9");
    CHECK(page.items[0].kind == NodeKind::Entry);
    CHECK(page.items[0].childCount == 2);
    CHECK(page.items[0].offset == base + 3 * 40);
    CHECK(page.items[0].label.find("contract 3") == 0);
    CHECK(page.items[3].label.find("contract 1") == 0);

    auto kids = dec.children("f:map/e:4").items;
    REQUIRE(kids.size() == 2);
    CHECK(kids[0].id == "f:map/e:4/f:key");
    CHECK(kids[1].id == "f:map/e:4/f:value");
    CHECK(json(kids[1])["value"]["v"] == "1900");
    CHECK(kids[1].offset == base + 4 * 40 + 32);
    // dead slots do not resolve as entries
    CHECK_THROWS_AS(dec.node("f:map/e:7"), NotFoundError);
    CHECK_THROWS_AS(dec.node("f:map/e:8"), NotFoundError);
    CHECK_THROWS_AS(dec.node("f:map/e:16"), NotFoundError);

    // paging
    auto p2 = dec.children("f:map", {ChildView::Logical, 2, 1, false});
    CHECK(p2.total == 4);
    REQUIRE(p2.items.size() == 1);
    CHECK(p2.items[0].id == "f:map/e:5");

    // raw view shows the C++ members
    auto raw = dec.children("f:map", {ChildView::Raw, 0, 10, false});
    REQUIRE(raw.items.size() == 4);
    CHECK(raw.items[0].label == "_elements");
    CHECK(raw.items[1].label == "_occupationFlags");
    CHECK(raw.items[2].label == "_population");
    CHECK(json(raw.items[2])["value"]["v"] == "4");
    CHECK(json(raw.items[3])["value"]["v"] == "6");
    auto rawEl = dec.children("f:map/f:_elements", {ChildView::Logical, 0, 100, true});
    CHECK(rawEl.total == 4); // tombstone slot is all zero -> skipped by hideEmpty (slot 7 has id 0 and value 0)
    CHECK(rawEl.items[0].id == "f:map/f:_elements/i:3");

    // locate
    CHECK(dec.locate(base + 4 * 40 + 33).id == "f:map/e:4/f:value");
    CHECK(dec.locate(base + 4 * 40 + 2).id == "f:map/e:4/f:key");
    CHECK(dec.locate(base + 7 * 40 + 2).id == "f:map/f:_elements/i:7/f:key");
    CHECK(dec.locate(base + 640).id == "f:map/f:_occupationFlags/i:0");
    CHECK(dec.locate(base + 648).id == "f:map/f:_population");
    CHECK(dec.locate(0).id == "f:pre");

    // table
    auto ti = dec.describeTable("f:map");
    CHECK(ti.view == "entries");
    REQUIRE(ti.columns.size() == 3);
    CHECK(ti.columns[0].id == "$index");
    CHECK(ti.columns[1].id == "key");
    CHECK(ti.columns[1].kind == "id");
    CHECK(ti.columns[1].group == "key");
    CHECK(ti.columns[2].id == "value");
    CHECK(ti.columns[2].kind == "int");
    CHECK(ti.totalRows == 4);
    auto tp = dec.tableRows(req("f:map"));
    CHECK(tp.total == 4);
    REQUIRE(tp.rows.size() == 4);
    CHECK(tp.rows[1].index == 4);
    CHECK(tp.rows[1].id == "f:map/e:4");
    CHECK(tp.rows[1].cells[2].v == "1900");
    CHECK(tp.rows[1].cells[0].v == "4");

    TableRequest sorted = req("f:map");
    sorted.sort = {{"value", true}};
    auto sp = dec.tableRows(sorted);
    REQUIRE(sp.rows.size() == 4);
    CHECK(sp.rows[0].cells[2].v == "3500");
    CHECK(sp.rows[3].cells[2].v == "11");
    sorted.sort = {{"value", false}};
    sorted.filters = {{"value", "gt", "300"}};
    sp = dec.tableRows(sorted);
    CHECK(sp.total == 2);
    CHECK(sp.rows[0].cells[2].v == "1900");
    sorted.filters = {{"key", "eq", idText(35)}};
    sp = dec.tableRows(sorted);
    CHECK(sp.total == 1);
    CHECK(sp.rows[0].index == 5);
    sorted.filters = {{"value", "ge", "0x76c"}}; // 1900
    CHECK(dec.tableRows(sorted).total == 2);
    sorted.filters = {{"value", "nope", "1"}};
    CHECK_THROWS_AS(dec.tableRows(sorted), InvalidArgumentError);
    sorted.filters = {{"nocol", "eq", "1"}};
    CHECK_THROWS_AS(dec.tableRows(sorted), InvalidArgumentError);
    sorted.filters = {{"value", "eq", "abc"}};
    CHECK_THROWS_AS(dec.tableRows(sorted), InvalidArgumentError);
    sorted.filters = {{"value", "zero", std::nullopt}};
    CHECK(dec.tableRows(sorted).total == 0);

    // wrong counters produce a warning, the flags win
    img.put(base + 648, 5);
    img.put(base + 656, 0);
    StateDecoder dec2(sb.finish(), root, mem(img), [] {
        DecoderConfig c;
        c.identity = testCodec();
        return c;
    }());
    auto n2 = dec2.node("f:map");
    CHECK(*n2.container->population == 4);
    CHECK(n2.container->warning.find("_population") != std::string::npos);
    CHECK(n2.container->warning.find("_markRemovalCounter") != std::string::npos);
}

TEST_CASE("empty and tiny hash containers") {
    SB sb;
    const TypeId m1 = sb.hashMap(sb.u8(), sb.u8(), 1);
    const TypeId s2 = sb.hashSet(sb.u16(), 2);
    const TypeId s64 = sb.hashSet(sb.u32(), 64);
    const TypeId root = sb.record("S", {{"m1", m1}, {"s2", s2}, {"s64", s64}});
    Img img(sb.size(root));
    StateDecoder dec(sb.finish(), root, mem(img));
    for (const char* id : {"f:m1", "f:s2", "f:s64"}) {
        auto n = dec.node(id);
        CHECK(n.childCount == 0);
        REQUIRE(n.container);
        CHECK(*n.container->population == 0);
        CHECK(n.container->warning.empty());
        CHECK(dec.children(id).items.empty());
        auto t = dec.tableRows(req(id));
        CHECK(t.total == 0);
        CHECK(t.rows.empty());
    }
    // L=1 map with its single slot live
    HashW h(img, 0, 1, 1, 1, 1, 1);
    h.set(0, {5}, {9});
    h.counters(1, 0);
    StateDecoder dec2(sb.finish(), root, mem(img));
    auto p = dec2.children("f:m1");
    REQUIRE(p.items.size() == 1);
    CHECK(json(dec2.children("f:m1/e:0").items[1])["value"]["v"] == "9");
}

TEST_CASE("HashSet<uint32,8>: bits above 2L are ignored, 0b11 reported") {
    SB sb;
    const TypeId set = sb.hashSet(sb.u32(), 8);
    const TypeId root = sb.record("S", {{"set", set}});
    CHECK(sb.size(set) == 56);
    Img img(sb.size(root));
    HashW h(img, 0, 8, 4, 0, 1, 4);
    h.set(2, le(77, 4));
    h.set(6, le(88, 4));
    h.setState(5, 3);                         // invalid
    img.put(h.flagsOff, img.get(h.flagsOff) | (std::uint64_t{1} << 40)); // slot 20: beyond L, must be ignored
    h.counters(2, 0);
    StateDecoder dec(sb.finish(), root, mem(img));
    auto n = dec.node("f:set");
    CHECK(*n.container->population == 2);
    CHECK(n.container->warning.find("0b11") != std::string::npos);
    auto p = dec.children("f:set");
    REQUIRE(p.items.size() == 2);
    CHECK(p.items[0].kind == NodeKind::Leaf);
    CHECK(json(p.items[1])["value"]["v"] == "88");
    CHECK(p.items[1].id == "f:set/e:6");
    auto t = dec.tableRows(req("f:set"));
    REQUIRE(t.rows.size() == 2);
    CHECK(t.rows[0].cells[1].v == "77");
}

TEST_CASE("Collection: PoVs, priority order (FIFO among equals), tombstoned PoVs, element table") {
    SB sb;
    const TypeId val = sb.record("V", {{"owner", sb.u64()}, {"tag", sb.u8()}}); // size 16
    const TypeId coll = sb.collection(val, 16);
    const TypeId root = sb.record("S", {{"pre", sb.u64()}, {"c", coll}});
    const std::uint64_t base = fieldOff(sb, root, "c");
    Img img(sb.size(root));
    CollW w(img, base, 16, 16);
    CHECK(w.total() == sb.size(coll)); // 64L + 8*W2(L) + L*(up(S(T),8)+40) + 16
    auto V = [](std::uint64_t owner, std::uint8_t tag) {
        auto v = le(owner);
        v.resize(16);
        v[8] = tag;
        return v;
    };
    const auto povA = idBytes(1, 0, 0, 0); // home slot 1
    const auto povB = idBytes(17, 5, 0, 0); // collides with A -> slot 2
    const auto povC = idBytes(9, 0, 0, 0);
    const auto povDead = idBytes(4, 0, 0, 0);
    // A: priorities 5, 9, 5, -3, 9, 7
    const std::int64_t prA[] = {5, 9, 5, -3, 9, 7};
    std::vector<std::uint64_t> idxA;
    for (int i = 0; i < 6; ++i) idxA.push_back(w.add(povA, V(100 + static_cast<std::uint64_t>(i), 1), prA[i]));
    w.add(povB, V(200, 2), 1);
    w.add(povC, V(300, 3), 10);
    w.add(povB, V(201, 2), 1);
    w.add(povDead, V(999, 4), 0);
    w.removeSoleLast(); // tombstoned PoV with stale id
    CHECK(w.population() == 9);

    StateDecoder dec(sb.finish(), root, mem(img), [] {
        DecoderConfig c;
        c.identity = testCodec();
        return c;
    }());
    auto n = dec.node("f:c");
    CHECK(n.kind == NodeKind::Collection);
    REQUIRE(n.container);
    CHECK(*n.container->population == 9);
    CHECK(*n.container->povs == 3);
    CHECK(*n.container->removed == 1);
    CHECK(n.container->warning.empty());
    CHECK(n.childCount == 3);
    CHECK(n.tabular);

    auto povs = dec.children("f:c");
    REQUIRE(povs.items.size() == 3);
    CHECK(povs.items[0].kind == NodeKind::Pov);
    CHECK(povs.items[0].id == "f:c/p:1");
    CHECK(povs.items[1].id == "f:c/p:2");
    CHECK(povs.items[2].id == "f:c/p:9");
    CHECK(povs.items[0].childCount == 6);
    CHECK(povs.items[1].childCount == 2);
    CHECK(json(povs.items[0])["value"]["k"] == "id");
    CHECK(povs.items[0].preview == "population 6");

    // PoV A queue: highest priority first, FIFO among equals: 9(idx1) 9(idx4) 7(idx5) 5(idx0) 5(idx2) -3(idx3)
    auto q = dec.children("f:c/p:1");
    REQUIRE(q.items.size() == 6);
    const std::uint64_t expectOrder[] = {idxA[1], idxA[4], idxA[5], idxA[0], idxA[2], idxA[3]};
    for (int i = 0; i < 6; ++i) CHECK(q.items[static_cast<std::size_t>(i)].id == "f:c/e:" + std::to_string(expectOrder[i]));
    CHECK(q.total == 6);
    auto q2 = dec.children("f:c/p:1", {ChildView::Logical, 4, 10, false});
    REQUIRE(q2.items.size() == 2);
    CHECK(q2.items[0].id == "f:c/e:" + std::to_string(idxA[2]));

    // an element
    auto e = dec.node("f:c/e:" + std::to_string(idxA[1]));
    CHECK(e.kind == NodeKind::Entry);
    CHECK(e.childCount == 3);
    CHECK(e.rawChildCount == 6);
    auto ek = dec.children(e.id).items;
    REQUIRE(ek.size() == 3);
    CHECK(ek[0].label == "value");
    CHECK(ek[1].label == "priority");
    CHECK(json(ek[1])["value"]["v"] == "9");
    CHECK(ek[2].label == "pov");
    CHECK(json(ek[2])["value"]["k"] == "id");
    CHECK(ek[2].offset == base + 1 * 64);
    auto ekRaw = dec.children(e.id, {ChildView::Raw, 0, 10, false}).items;
    CHECK(ekRaw.size() == 6);
    CHECK(ekRaw[2].label == "povIndex");
    // reachable through the PoV path as well
    CHECK(dec.node("f:c/p:1/e:" + std::to_string(idxA[1])).id == "f:c/e:" + std::to_string(idxA[1]));

    // table: elements
    auto ti = dec.describeTable("f:c");
    REQUIRE(ti.views.size() == 2);
    CHECK(ti.view == "elements");
    CHECK(ti.totalRows == 9);
    std::vector<std::string> ids;
    for (auto& c : ti.columns) ids.push_back(c.id);
    CHECK(ids == std::vector<std::string>{"$index", "$pov", "$priority", "value.owner", "value.tag"});
    auto tp = dec.tableRows(req("f:c"));
    CHECK(tp.total == 9);
    REQUIRE(tp.rows.size() == 9);
    CHECK(tp.rows[0].cells[1].k == LeafValue::Kind::Id);
    CHECK(tp.rows[0].cells[1].hex == json(dec.node("f:c/p:1"))["value"].value("hex", ""));
    TableRequest sr = req("f:c");
    sr.sort = {{"$priority", true}, {"value.owner", false}};
    auto sp = dec.tableRows(sr);
    CHECK(sp.rows[0].cells[2].v == "10");
    CHECK(sp.rows[1].cells[2].v == "9");
    CHECK(sp.rows[1].cells[3].v == "101");
    CHECK(sp.rows[2].cells[3].v == "104");
    CHECK(sp.rows[8].cells[2].v == "-3");
    sr.sort = {{"$priority", false}};
    sr.filters = {{"$pov", "eq", json(dec.node("f:c/p:2"))["value"].value("identity", std::string())}};
    sp = dec.tableRows(sr);
    CHECK(sp.total == 2);
    CHECK(sp.rows[0].cells[3].v == "200");
    sr.filters = {{"$priority", "lt", "0"}};
    CHECK(dec.tableRows(sr).total == 1);
    sr.filters = {{"value.tag", "eq", "1"}};
    CHECK(dec.tableRows(sr).total == 6);
    // povs view
    auto tpv = dec.describeTable("f:c", "povs");
    CHECK(tpv.totalRows == 3);
    auto pr = req("f:c");
    pr.view = "povs";
    auto pp = dec.tableRows(pr);
    REQUIRE(pp.rows.size() == 3);
    CHECK(pp.rows[0].id == "f:c/p:1");
    std::vector<std::string> pids;
    for (auto& c : tpv.columns) pids.push_back(c.id);
    CHECK(pids == std::vector<std::string>{"$index", "$pov", "$population", "$head", "$tail", "$root"});
    CHECK(pp.rows[0].cells[2].v == "6");
    CHECK_THROWS_AS(dec.describeTable("f:c", "nope"), NotFoundError);

    // locate
    CHECK(dec.locate(w.el(idxA[1])).id == "f:c/e:" + std::to_string(idxA[1]) + "/f:value/f:owner");
    CHECK(dec.locate(w.prio(idxA[1])).id == "f:c/e:" + std::to_string(idxA[1]) + "/f:priority");
    CHECK(dec.locate(base + 64 * 2 + 3).id == "f:c/p:2");
    // tombstoned PoV slot 4 and the dead element area -> raw path
    CHECK(dec.locate(base + 64 * 4 + 3).id == "f:c/f:_povs/i:4/f:value");
    CHECK(dec.locate(w.el(12)).id == "f:c/f:_elements/i:12/f:value/f:owner");
}

TEST_CASE("Collection<sint64>: scalar values, empty collection, zero state") {
    SB sb;
    const TypeId coll = sb.collection(sb.s64(), 8);
    const TypeId root = sb.record("S", {{"c", coll}});
    Img img(sb.size(root));
    {
        StateDecoder dec(sb.finish(), root, mem(img));
        auto n = dec.node("f:c");
        CHECK(n.childCount == 0);
        CHECK(*n.container->population == 0);
        CHECK(dec.children("f:c").items.empty());
        CHECK(dec.tableRows(req("f:c")).total == 0);
        CHECK(dec.tableRows([] { auto r = req("f:c"); r.view = "povs"; return r; }()).total == 0);
    }
    CollW w(img, 0, 8, 8);
    w.add(idBytes(2), le(33), 5);
    StateDecoder dec(sb.finish(), root, mem(img));
    auto t = dec.tableRows(req("f:c"));
    REQUIRE(t.rows.size() == 1);
    CHECK(dec.describeTable("f:c").columns.back().id == "value");
    CHECK(t.rows[0].cells.back().v == "33");
    // counter mismatch: elements claim 3 but the PoV sums say 1
    img.put(w.popOff, 3);
    StateDecoder dec2(sb.finish(), root, mem(img));
    CHECK(dec2.node("f:c").container->warning.find("PoV populations") != std::string::npos);
}

TEST_CASE("LinkedList: list order, zero-initialised, emptied") {
    SB sb;
    const TypeId ll = sb.linkedList(sb.u64(), 8);
    const TypeId root = sb.record("S", {{"never", ll}, {"used", ll}, {"emptied", ll}});
    const std::uint64_t used = fieldOff(sb, root, "used"), emptied = fieldOff(sb, root, "emptied");
    Img img(sb.size(root));
    ListW u(img, used, 8, 8);
    CHECK(u.total() == sb.size(ll));
    u.initEmpty();
    for (int i = 0; i < 4; ++i) u.addTail(le(100 + static_cast<std::uint64_t>(i)));
    // re-link as 2 -> 0 -> 3 -> 1 to prove the list order is followed, not the node order
    const std::uint64_t no = up(8, 8);
    auto setNext = [&](std::uint64_t node, std::int64_t nxt) { img.put(used + node * u.nodeSize + no, static_cast<std::uint64_t>(nxt)); };
    auto setPrev = [&](std::uint64_t node, std::int64_t prv) { img.put(used + node * u.nodeSize + no + 8, static_cast<std::uint64_t>(prv)); };
    setNext(2, 0); setPrev(2, -1);
    setNext(0, 3); setPrev(0, 2);
    setNext(3, 1); setPrev(3, 0);
    setNext(1, -1); setPrev(1, 3);
    img.put(used + u.headOff, 2);
    img.put(used + u.tailOff, 1);
    ListW e(img, emptied, 8, 8);
    e.initEmpty();
    img.put(emptied + e.nextUnusedOff, 3); // nodes were used, all freed again
    img.put(emptied + e.freeOff, 2);
    StateDecoder dec(sb.finish(), root, mem(img));

    auto never = dec.node("f:never");
    CHECK(never.childCount == 0);
    CHECK(never.container->warning.empty());
    CHECK(dec.children("f:never").items.empty());
    CHECK(dec.node("f:emptied").childCount == 0);
    CHECK(dec.children("f:emptied").items.empty());

    auto n = dec.node("f:used");
    CHECK(n.kind == NodeKind::LinkedList);
    CHECK(n.childCount == 4);
    CHECK(n.container->warning.empty());
    auto p = dec.children("f:used");
    REQUIRE(p.items.size() == 4);
    CHECK(json(p.items[0])["value"]["v"] == "102");
    CHECK(json(p.items[1])["value"]["v"] == "100");
    CHECK(json(p.items[2])["value"]["v"] == "103");
    CHECK(json(p.items[3])["value"]["v"] == "101");
    CHECK(p.items[0].id == "f:used/e:2");
    auto t = dec.tableRows(req("f:used"));
    REQUIRE(t.rows.size() == 4);
    CHECK(t.rows[0].cells[0].v == "2"); // $index = node
    CHECK(t.rows[1].cells[1].v == "1"); // $position
    CHECK(dec.describeTable("f:used").columns[1].id == "$position");
    TableRequest sr = req("f:used");
    sr.sort = {{"value", true}};
    CHECK(dec.tableRows(sr).rows[0].cells[2].v == "103");
    // raw view
    auto raw = dec.children("f:used", {ChildView::Raw, 0, 20, false});
    CHECK(raw.total == 7);
    // locate: value vs link bytes of node 1
    CHECK(dec.locate(used + 1 * u.nodeSize + 2).id == "f:used/e:1");
    CHECK(dec.locate(used + 1 * u.nodeSize + 9).id == "f:used/f:_nodes/i:1/f:nextIndex");
    // stored population disagreeing with flags is reported
    img.put(used + u.popOff, 3);
    StateDecoder dec2(sb.finish(), root, mem(img));
    CHECK(dec2.node("f:used").container->warning.find("occupied") != std::string::npos);
}

TEST_CASE("table columns of struct elements are flattened; many fields collapse; arrays composite") {
    SB sb;
    const TypeId asset = sb.record("Asset", {{"issuer", sb.id()}, {"assetName", sb.u64()}});
    const TypeId order = sb.record("Order", {{"entity", sb.id()}, {"asset", asset}, {"shares", sb.s64()},
                                             {"label", sb.array(sb.u8(), 8)}, {"vals", sb.array(sb.u32(), 4)}});
    const TypeId arr = sb.array(order, 5);
    const TypeId root = sb.record("S", {{"orders", arr}});
    Img img(sb.size(root));
    const std::uint64_t es = sb.size(order);
    img.putBytes(2 * es, idBytes(1));
    img.putBytes(2 * es + 32, idBytes(2));
    img.putText(2 * es + 64, "QUTIL");
    img.put(2 * es + 72, static_cast<std::uint64_t>(-5));
    img.putText(2 * es + 80, "abc");
    StateDecoder dec(sb.finish(), root, mem(img));
    auto ti = dec.describeTable("f:orders");
    std::vector<std::string> ids;
    for (auto& c : ti.columns) ids.push_back(c.id);
    CHECK(ids == std::vector<std::string>{"$index", "value.entity", "value.asset.issuer", "value.asset.assetName",
                                          "value.shares", "value.label", "value.vals"});
    CHECK(ti.columns[3].kind == "int");
    CHECK(ti.columns[5].kind == "bytes");
    CHECK(ti.columns[6].kind == "composite");
    CHECK(!ti.columns[6].sortable);
    auto rows = dec.tableRows([] { TableRequest r; r.id = "f:orders"; r.hideEmpty = true; return r; }());
    CHECK(rows.total == 1);
    REQUIRE(rows.rows.size() == 1);
    CHECK(rows.rows[0].index == 2);
    CHECK(rows.rows[0].id == "f:orders/i:2");
    CHECK(rows.rows[0].cells[3].text == "QUTIL");
    CHECK(rows.rows[0].cells[4].v == "-5");
    CHECK(rows.rows[0].cells[5].text == "abc");
    CHECK(rows.rows[0].cells[6].k == LeafValue::Kind::Composite);
    // asset names filter by text
    TableRequest fr;
    fr.id = "f:orders";
    fr.filters = {{"value.asset.assetName", "eq", "QUTIL"}};
    CHECK(dec.tableRows(fr).total == 1);
    fr.filters = {{"value.asset.assetName", "contains", "uti"}};
    CHECK(dec.tableRows(fr).total == 1);
    fr.filters = {{"value.shares", "lt", "0"}};
    CHECK(dec.tableRows(fr).total == 1);
    fr.filters = {{"value.shares", "zero", std::nullopt}};
    CHECK(dec.tableRows(fr).total == 4);
    fr.filters = {{"value.vals", "eq", "1"}};
    CHECK_THROWS_AS(dec.tableRows(fr), InvalidArgumentError);
    // full table without hideEmpty
    TableRequest all;
    all.id = "f:orders";
    CHECK(dec.tableRows(all).total == 5);
    all.sort = {{"value.vals", false}};
    CHECK_THROWS_AS(dec.tableRows(all), InvalidArgumentError);
    // generations and caches
    Query q1;
    q1.generation = 1;
    Query q2;
    q2.generation = 2;
    dec.tableRows(fr = [] { TableRequest r; r.id = "f:orders"; r.sort = {{"value.shares", true}}; return r; }(), q1);
    const auto entries = dec.cache().entryCount();
    CHECK(entries > 0);
    dec.tableRows(fr, q2);
    CHECK(dec.cache().entryCount() <= entries + 1);
}

TEST_CASE("JSON shapes follow contract.ts") {
    SB sb;
    const TypeId map = sb.hashMap(sb.u64(), sb.u32(), 8);
    const TypeId root = sb.record("S", {{"m", map}});
    Img img(sb.size(root));
    HashW h(img, 0, 8, 8, 4, 4, 8);
    h.set(1, le(11), le(22, 4));
    h.counters(1, 0);
    StateDecoder dec(sb.finish(), root, mem(img));
    json j = dec.children("f:m");
    CHECK(j["total"] == 1);
    CHECK(j["offset"] == 0);
    CHECK(j["items"][0]["kind"] == "entry");
    CHECK(j["items"][0]["tabular"] == false);
    CHECK(j["items"][0]["inFile"] == true);
    json n = dec.node("f:m");
    CHECK(n["kind"] == "hashMap");
    CHECK(n["container"]["capacity"] == 8);
    CHECK(n["container"]["population"] == 1);
    CHECK(n["rawChildCount"] == 4);
    json t = dec.describeTable("f:m");
    CHECK(t["columns"][1]["id"] == "key");
    CHECK(t["totalRows"] == 1);
    json rq = json::parse(R"({"id":"f:m","offset":0,"limit":10,"sort":[{"column":"value","desc":true}],"filters":[{"column":"key","op":"eq","value":"11"}]})");
    auto r = rq.get<TableRequest>();
    CHECK(r.sort.size() == 1);
    CHECK(r.sort[0].desc);
    CHECK(r.filters[0].value == "11");
    json page = dec.tableRows(r);
    CHECK(page["total"] == 1);
    CHECK(page["rows"][0]["cells"][1]["k"] == "int");
}

TEST_CASE("huge sparse HashMap (2^24 slots, ~700 MB logical): fast flag scan, paging, cache") {
    SB sb;
    const std::uint64_t L = 1u << 24;
    const TypeId map = sb.hashMap(sb.id(), sb.u64(), L);
    const TypeId root = sb.record("S", {{"map", map}});
    HashW dummyLayout(*std::make_unique<Img>(0), 0, L, 32, 8, 8, 8);
    CHECK(sb.size(map) == dummyLayout.total());
    auto src = std::make_shared<SparseSource>(sb.size(root));
    std::mt19937_64 rng(7);
    std::vector<std::uint32_t> slots;
    {
        std::set<std::uint32_t> uniq;
        while (uniq.size() < 50000) uniq.insert(static_cast<std::uint32_t>(rng() % L));
        slots.assign(uniq.begin(), uniq.end());
    }
    std::uint64_t flagWords = L * 2 / 64;
    std::vector<std::uint64_t> words(flagWords, 0);
    for (std::uint32_t s : slots) {
        words[s >> 5] |= std::uint64_t{1} << ((s & 31) * 2);
        src->write(std::uint64_t{s} * 40, idBytes(std::uint64_t{s} + 1));
        src->write64(std::uint64_t{s} * 40 + 32, std::uint64_t{s} * 3);
    }
    for (std::uint64_t i = 0; i < flagWords; ++i)
        if (words[i]) src->write64(L * 40 + i * 8, words[i]);
    src->write64(L * 40 + flagWords * 8, slots.size());
    StateDecoder dec(sb.finish(), root, src);

    auto t0 = std::chrono::steady_clock::now();
    auto n = dec.node("f:map");
    const double scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(*n.container->population == 50000);
    CHECK(n.container->warning.empty());
    CHECK(n.childCount == 50000);
    MESSAGE("flag scan of 2^24 slots: " << scanMs << " ms");
    t0 = std::chrono::steady_clock::now();
    auto page = dec.children("f:map", {ChildView::Logical, 49990, 20, false});
    const double pageMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(page.items.size() == 10);
    CHECK(page.items[0].offset == std::uint64_t{slots[49990]} * 40);
    MESSAGE("cached page: " << pageMs << " ms");
    CHECK(pageMs < 200);
    // sorted table by value descending over 50k sparse rows
    TableRequest r = req("f:map", 0, 5);
    r.sort = {{"value", true}};
    t0 = std::chrono::steady_clock::now();
    auto tp = dec.tableRows(r);
    MESSAGE("sorted 50k sparse rows: " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() << " ms");
    CHECK(tp.total == 50000);
    CHECK(tp.rows[0].cells[2].v == std::to_string(std::uint64_t{slots.back()} * 3));
    t0 = std::chrono::steady_clock::now();
    r.offset = 40000;
    tp = dec.tableRows(r);
    const double cachedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("second page of the sorted table (cached index): " << cachedMs << " ms");
    CHECK(tp.rows.size() == 5);
    // raw hideEmpty over the 671 MB element array (sparse source: zeros are generated)
    t0 = std::chrono::steady_clock::now();
    auto ne = dec.children("f:map/f:_elements", {ChildView::Logical, 0, 3, true});
    MESSAGE("hideEmpty scan of the raw element array: " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() << " ms");
    CHECK(ne.total == 50000);
    // locate in the middle
    CHECK(dec.locate(std::uint64_t{slots[100]} * 40 + 33).id == "f:map/e:" + std::to_string(slots[100]) + "/f:value");
    // a tiny cache keeps working
    dec.cache().setCapacity(1024);
    CHECK(dec.node("f:map").container->population == 50000);
    CHECK(dec.cache().usedBytes() <= 1024);
}
