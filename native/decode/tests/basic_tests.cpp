#include <doctest/doctest.h>

#include <random>

#include "qstate/decode/json.h"
#include "../src/format.h"
#include "test_helpers.h"

using namespace qstate::decode;
using namespace qstate::decode::testing;
using nlohmann::json;

namespace {

struct Fx {
    SB sb;
    Img img;
    std::unique_ptr<StateDecoder> dec;
    TypeId root = 0;
    void make(TypeId r, std::shared_ptr<const ByteSource> src = nullptr, DecoderConfig cfg = {}) {
        root = r;
        if (!cfg.identity) cfg.identity = testCodec();
        if (!cfg.contractName)
            cfg.contractName = [](std::uint32_t i) { return i == 1 ? std::string("QX") : std::string(); };
        dec = std::make_unique<StateDecoder>(sb.finish(), r, src ? src : mem(img), cfg);
    }
    NodeInfo node(const std::string& id) const { return dec->node(id); }
    ChildrenPage kids(const std::string& id, ChildrenRequest r = {}) const { return dec->children(id, r); }
};

json J(const NodeInfo& n) { return json(n); }

} // namespace

TEST_CASE("leaf values: integers keep full precision, hex, bool, char, enum, float") {
    Fx f;
    auto e = f.sb.enumT("EState", f.sb.u8(), {{"NONE", 0}, {"LOCKED", 1}});
    auto r = f.sb.record("S", {{"u64max", f.sb.u64()}, {"s64min", f.sb.s64()}, {"u32", f.sb.u32()}, {"s16", f.sb.s16()},
                               {"flag", f.sb.boolean()}, {"ch", f.sb.chr()}, {"state", e}, {"fl", f.sb.f32()},
                               {"db", f.sb.f64()}, {"u8", f.sb.u8()}});
    f.img = Img(f.sb.size(r));
    f.img.put(0, ~std::uint64_t{0});
    f.img.put(8, std::uint64_t{1} << 63);
    f.img.put(16, 4000000000u, 4);
    f.img.put(20, 0xFFFE, 2); // -2
    f.img.put(22, 2, 1);      // bool raw 2
    f.img.put(23, 'Q', 1);
    f.img.put(24, 1, 1);
    float fl = 1.5f;
    std::uint32_t fb;
    std::memcpy(&fb, &fl, 4);
    f.img.put(28, fb, 4);
    double d = -0.1;
    std::uint64_t db;
    std::memcpy(&db, &d, 8);
    f.img.put(32, db);
    f.img.put(40, 7, 1);
    f.make(r);

    auto root = f.node("");
    CHECK(root.kind == NodeKind::Struct);
    CHECK(root.childCount == 10);
    CHECK(root.size == 48);
    auto page = f.kids("");
    REQUIRE(page.items.size() == 10);
    auto v = [&](int i) { return J(page.items[static_cast<std::size_t>(i)])["value"]; };
    CHECK(v(0)["v"] == "18446744073709551615");
    CHECK(v(0)["unsigned"] == true);
    CHECK(v(0)["bits"] == 64);
    CHECK(v(0)["hex"] == "0xffffffffffffffff");
    CHECK(v(1)["v"] == "-9223372036854775808");
    CHECK(v(1)["unsigned"] == false);
    CHECK(v(1)["hex"] == "0x8000000000000000");
    CHECK(v(2)["v"] == "4000000000");
    CHECK(v(3)["v"] == "-2");
    CHECK(v(3)["hex"] == "0xfffe");
    CHECK(v(4)["k"] == "bool");
    CHECK(v(4)["v"] == true);
    CHECK(v(4)["raw"] == 2);
    CHECK(v(5)["k"] == "char");
    CHECK(v(5)["v"] == "Q");
    CHECK(v(5)["code"] == 81);
    CHECK(v(6)["k"] == "enum");
    CHECK(v(6)["name"] == "LOCKED");
    CHECK(v(6)["v"] == "1");
    CHECK(v(7)["v"] == "1.5");
    CHECK(v(8)["v"] == "-0.1");
    CHECK(v(9)["v"] == "7");
    CHECK(page.items[0].id == "f:u64max");
    CHECK(page.items[0].inFile);
    // stable ids round trip
    CHECK(f.node("f:s16").label == "s16");
    CHECK_THROWS_AS(f.node("f:nope"), NotFoundError);
    CHECK_THROWS_AS(f.node("garbage"), NotFoundError);
}

TEST_CASE("ids: identity text, contract ids, zero, text-like ids; u128; datetime; asset names") {
    Fx f;
    auto r = f.sb.record("S", {{"a", f.sb.id()}, {"b", f.sb.id()}, {"c", f.sb.id()}, {"d", f.sb.id()},
                               {"wide", f.sb.u128()}, {"when", f.sb.dateTime()}, {"assetName", f.sb.u64()},
                               {"other", f.sb.u64()}, {"issuer", f.sb.id()}, {"name", f.sb.u64()}});
    f.img = Img(f.sb.size(r));
    f.img.putBytes(0, idBytes(1));               // contract 1
    // b stays zero
    f.img.putBytes(64, idBytes(0x12345, 77, 0, 0)); // arbitrary key
    f.img.putText(96, "HELLO_WORLD");               // text-like id
    f.img.put(128, 0xFFFFFFFFFFFFFFFFull);          // u128 low
    f.img.put(136, 1);                              // high
    // 2026-08-19 18:15:31.000'000
    std::uint64_t dt = (std::uint64_t{2026} << 46) | (std::uint64_t{8} << 42) | (std::uint64_t{19} << 37) |
                       (std::uint64_t{18} << 32) | (std::uint64_t{15} << 26) | (std::uint64_t{31} << 20);
    f.img.put(144, dt);
    f.img.putText(152, "QUTIL");   // assetName
    f.img.putText(160, "QUTIL");   // field name does not suggest an asset name -> plain int
    f.img.put(200, 0x4D4F4E5354ull); // 'name' field: "TSNOM"? bytes 54 53 4E 4F 4D -> 'T','S','N','O','M'
    f.make(r);
    auto p = f.kids("").items;
    auto val = [&](int i) { return J(p[static_cast<std::size_t>(i)])["value"]; };
    CHECK(val(0)["k"] == "id");
    CHECK(val(0)["identity"] == "B" + std::string(55, 'A') + "RMID");
    CHECK(val(0)["contract"]["index"] == 1);
    CHECK(val(0)["contract"]["name"] == "QX");
    CHECK(val(0)["zero"] == false);
    CHECK(val(1)["identity"] == std::string(56, 'A') + "FXIB");
    CHECK(val(1)["zero"] == true);
    CHECK(!val(1).contains("contract"));
    CHECK(val(2)["hex"].get<std::string>().size() == 64);
    CHECK(!val(2).contains("contract"));
    CHECK(val(3)["text"] == "HELLO_WORLD");
    CHECK(val(4)["k"] == "u128");
    CHECK(val(4)["v"] == "36893488147419103231"); // 2^65 - 1
    // hex formats pinned in contract.ts: u128 = "0x" + 32 digits (most significant first); id hex is bare, 64 chars
    CHECK(val(4)["hex"] == "0x0000000000000001ffffffffffffffff");
    CHECK(val(2)["hex"].get<std::string>().rfind("0x", 0) == std::string::npos);
    CHECK(val(5)["text"] == "2026-08-19 18:15:31.000'000");
    CHECK(val(5)["valid"] == true);
    CHECK(val(5)["raw"] == std::to_string(dt));
    CHECK(val(6)["text"] == "QUTIL");
    CHECK(!val(7).contains("text"));
    CHECK(val(9)["text"] == "TSNOM");
}

TEST_CASE("datetime edge cases") {
    bool valid = true;
    CHECK(fmt::dateTimeText(0, valid) == "unset");
    CHECK(!valid);
    fmt::dateTimeText(~std::uint64_t{0}, valid);
    CHECK(!valid);
    // Feb 30 is invalid
    std::uint64_t bad = (std::uint64_t{2026} << 46) | (std::uint64_t{2} << 42) | (std::uint64_t{30} << 37);
    fmt::dateTimeText(bad, valid);
    CHECK(!valid);
    std::uint64_t leap = (std::uint64_t{2024} << 46) | (std::uint64_t{2} << 42) | (std::uint64_t{29} << 37);
    CHECK(fmt::dateTimeText(leap, valid) == "2024-02-29 00:00:00.000'000");
    CHECK(valid);
}

TEST_CASE("asset name heuristic is conservative") {
    CHECK(fmt::assetNameText(22609) == "QX");
    CHECK(fmt::assetNameText(327647778129ull) == "QUTIL");
    CHECK(!fmt::assetNameText(0));
    CHECK(!fmt::assetNameText(0x20202020));        // spaces
    CHECK(!fmt::assetNameText(0x6162));            // lower case
    CHECK(!fmt::assetNameText(0x4100000041ull));   // gap
    CHECK(!fmt::assetNameText(0x4141414141414141ull)); // 8 chars
    CHECK(fmt::assetNameValue("QX") == 22609);
    CHECK(!fmt::assetNameValue("TOOLONGNAME"));
    CHECK(assetNameLike("assetName"));
    CHECK(assetNameLike("_tradeAsset"));
    CHECK(assetNameLike("name"));
    CHECK(!assetNameLike("amount"));
}

TEST_CASE("byte arrays: text and hex; arrays and paging; hideEmpty; unions; nested; bit-fields") {
    Fx f;
    auto txt = f.sb.array(f.sb.u8(), 16);
    auto nums = f.sb.array(f.sb.u32(), 1000);
    auto un = f.sb.record("U", {{"a", f.sb.u64()}, {"b", f.sb.u32()}}, true);
    auto inner = f.sb.record("In", {{"x", f.sb.u16()}, {"y", f.sb.u16()}});
    auto r = f.sb.record("S", {{"text", txt}, {"nums", nums}, {"un", un}, {"inner", f.sb.array(inner, 3)}});
    f.img = Img(f.sb.size(r));
    f.img.putText(0, "hello");
    f.img.put(16 + 4 * 5, 55, 4);
    f.img.put(16 + 4 * 500, 66, 4);
    f.img.put(16 + 4 * 999, 77, 4);
    const std::uint64_t unOff = 16 + 4000;
    f.img.put(unOff, 0x1122334455667788ull);
    f.make(r);

    auto t = f.node("f:text");
    CHECK(t.kind == NodeKind::Array);
    CHECK(t.childCount == 16);
    REQUIRE(t.value);
    CHECK(J(t)["value"]["text"] == "hello");
    CHECK(J(t)["value"]["length"] == 16);

    auto n = f.node("f:nums");
    CHECK(n.childCount == 1000);
    CHECK(n.tabular);
    auto page = f.kids("f:nums", {ChildView::Logical, 998, 10, false});
    CHECK(page.total == 1000);
    CHECK(page.items.size() == 2);
    CHECK(page.items[1].label == "[999]");
    CHECK(J(page.items[1])["value"]["v"] == "77");
    CHECK(page.items[1].id == "f:nums/i:999");
    auto ne = f.kids("f:nums", {ChildView::Logical, 0, 100, true});
    CHECK(ne.total == 3);
    REQUIRE(ne.items.size() == 3);
    CHECK(ne.items[0].label == "[5]");
    CHECK(ne.items[1].label == "[500]");
    CHECK(ne.items[2].label == "[999]");
    auto ne2 = f.kids("f:nums", {ChildView::Logical, 1, 1, true});
    CHECK(ne2.total == 3);
    CHECK(ne2.items.at(0).label == "[500]");
    CHECK(f.kids("f:nums", {ChildView::Logical, 0, 1000000, false}).items.size() == 1000); // limit clamped to 1000

    auto u = f.node("f:un");
    CHECK(u.kind == NodeKind::Union);
    auto um = f.kids("f:un").items;
    REQUIRE(um.size() == 2);
    CHECK(um[0].offset == um[1].offset);
    CHECK(J(um[0])["value"]["v"] == std::to_string(0x1122334455667788ull));

    auto a = f.node("f:inner/i:2/f:y");
    CHECK(a.offset == 16 + 4000 + 8 + 8 + 2);
    CHECK(f.node("f:inner").preview.size() > 0);
}

TEST_CASE("BitArray children and value") {
    Fx f;
    auto r = f.sb.record("S", {{"bits", f.sb.bitArray(128)}, {"tail", f.sb.u64()}});
    f.img = Img(f.sb.size(r));
    f.img.put(0, 0b101, 1);
    f.img.put(9, 0x80, 1); // bit 79
    f.make(r);
    auto n = f.node("f:bits");
    CHECK(n.kind == NodeKind::BitArray);
    CHECK(n.childCount == 128);
    CHECK(J(n)["value"]["set"] == 3);
    CHECK(J(n)["value"]["count"] == 128);
    auto page = f.kids("f:bits", {ChildView::Logical, 0, 5, false});
    CHECK(page.items.size() == 5);
    CHECK(J(page.items[2])["value"]["v"] == true);
    CHECK(J(page.items[1])["value"]["v"] == false);
    auto set = f.kids("f:bits", {ChildView::Logical, 0, 10, true});
    CHECK(set.total == 3);
    CHECK(set.items[2].label == "[79]");
    CHECK(f.node("f:bits/b:79").offset == 9);
    auto raw = f.kids("f:bits", {ChildView::Raw, 0, 10, false});
    CHECK(raw.total == 1);
    CHECK(raw.items[0].label == "_values");
    CHECK(f.dec->locate(9).id == "f:bits");
}

TEST_CASE("truncated file: nodes beyond the end are unavailable, nothing crashes") {
    Fx f;
    auto r = f.sb.record("S", {{"a", f.sb.u64()}, {"b", f.sb.id()}, {"c", f.sb.u64()}});
    f.img = Img(44); // cuts c (40..48)
    f.img.put(0, 5);
    f.make(r);
    auto p = f.kids("").items;
    REQUIRE(p.size() == 3);
    CHECK(p[0].inFile);
    CHECK(p[1].inFile);
    CHECK(!p[2].inFile);
    CHECK(J(p[2])["value"]["k"] == "unavailable");
    CHECK(f.dec->fileSize() == 44);
    CHECK(f.dec->stateSize() == 48);
}

TEST_CASE("locate by offset descends to the deepest node") {
    Fx f;
    auto inner = f.sb.record("In", {{"x", f.sb.u16()}, {"y", f.sb.u32()}});
    auto r = f.sb.record("S", {{"pad", f.sb.u64()}, {"arr", f.sb.array(inner, 4)}, {"tail", f.sb.u64()}});
    f.img = Img(f.sb.size(r));
    f.make(r);
    // arr at 8, each element 8 bytes: [x@0 pad@2 y@4]
    auto loc = f.dec->locate(8 + 2 * 8 + 4 + 1);
    CHECK(loc.id == "f:arr/i:2/f:y");
    CHECK(loc.path.size() == 4);
    CHECK(loc.path[0].id == "");
    CHECK(loc.path[2].label == "[2]");
    CHECK(loc.offset == 8 + 16 + 4);
    CHECK(loc.size == 4);
    // padding byte inside element 2 -> element
    CHECK(f.dec->locate(8 + 16 + 2).id == "f:arr/i:2");
    CHECK(f.dec->locate(0).id == "f:pad");
    CHECK_THROWS_AS(f.dec->locate(1000), NotFoundError);
    // located ids resolve
    CHECK(f.node(loc.id).offset == loc.offset);
}

TEST_CASE("search: hex, int, text, id; limits; offsets map to nodes") {
    Fx f;
    auto r = f.sb.record("S", {{"a", f.sb.u64()}, {"b", f.sb.id()}, {"c", f.sb.array(f.sb.u8(), 16)}, {"d", f.sb.u64()}});
    f.img = Img(f.sb.size(r));
    f.img.put(0, 123456789);
    f.img.putBytes(8, idBytes(1));
    f.img.putText(40, "needle");
    f.img.put(56, 123456789);
    f.make(r);
    auto s = f.dec->search({"123456789", "auto", 10});
    CHECK(s.mode == "int");
    REQUIRE(s.matches.size() == 2);
    CHECK(s.matches[0].location.id == "f:a");
    CHECK(s.matches[1].location.id == "f:d");
    CHECK(!s.truncated);
    s = f.dec->search({"needle", "auto", 10});
    CHECK(s.mode == "text");
    REQUIRE(s.matches.size() == 1);
    CHECK(s.matches[0].offset == 40);
    CHECK(s.matches[0].location.id == "f:c/i:0");
    s = f.dec->search({"0x6e6565646c65", "auto", 10});
    CHECK(s.mode == "hex");
    CHECK(s.matches.size() == 1);
    s = f.dec->search({"B" + std::string(55, 'A') + "RMID", "auto", 10});
    CHECK(s.mode == "id");
    REQUIRE(s.matches.size() == 1);
    CHECK(s.matches[0].location.id == "f:b");
    s = f.dec->search({"00", "hex", 3});
    CHECK(s.truncated);
    CHECK(s.matches.size() == 3);
    CHECK_THROWS_AS(f.dec->search({"zz", "hex", 3}), InvalidArgumentError);
    CHECK_THROWS_AS(f.dec->search({"", "auto", 3}), InvalidArgumentError);
    auto js = json(s);
    CHECK(js["pattern"]["mode"] == "hex");
}

TEST_CASE("search across block boundaries") {
    SB sb;
    auto r = sb.array(sb.u8(), 9u << 20);
    auto src = std::make_shared<SparseSource>(sb.size(r));
    const std::uint64_t boundary = 4u << 20;
    src->write(boundary - 3, {'a', 'b', 'c', 'd', 'e', 'f'});
    src->write(8u << 20, {'a', 'b', 'c', 'd', 'e', 'f'});
    StateDecoder dec(sb.finish(), r, src);
    auto res = dec.search({"abcdef", "text", 10});
    REQUIRE(res.matches.size() == 2);
    CHECK(res.matches[0].offset == boundary - 3);
    CHECK(res.matches[1].offset == (8u << 20));
}

TEST_CASE("cancellation") {
    SB sb;
    auto r = sb.array(sb.u8(), 64u << 20);
    auto src = std::make_shared<SparseSource>(sb.size(r));
    StateDecoder dec(sb.finish(), r, src);
    std::atomic<bool> cancel{true};
    Query q;
    q.cancel = &cancel;
    CHECK_THROWS_AS(dec.search({"zzzz", "text", 10}, q), CancelledError);
    CHECK_THROWS_AS(dec.children("", {ChildView::Logical, 0, 10, true}, q), CancelledError);
}

TEST_CASE("fuzz: random bytes never crash any query") {
    SB sb;
    auto tx = sb.collection(sb.record("V", {{"a", sb.u64()}, {"b", sb.u8()}}), 16);
    auto hm = sb.hashMap(sb.id(), sb.u64(), 16);
    auto hs = sb.hashSet(sb.u32(), 8);
    auto ll = sb.linkedList(sb.u64(), 8);
    auto root = sb.record("Root", {{"coll", tx}, {"map", hm}, {"set", hs}, {"list", ll}, {"bits", sb.bitArray(100)},
                                   {"arr", sb.qpiArray(sb.u32(), 10)}, {"dt", sb.dateTime()}, {"f", sb.f64()}});
    const std::uint64_t size = sb.size(root);
    std::mt19937_64 rng(12345);
    auto schemaPtr = sb.finish();
    for (int iter = 0; iter < 60; ++iter) {
        Img img(size);
        for (auto& b : img.b) {
            const auto r = rng();
            // mix of zero / 0xFF / random bytes to hit sentinels and wild indices
            b = (r & 3) == 0 ? 0xFF : ((r & 3) == 1 ? 0 : static_cast<std::uint8_t>(r >> 8));
        }
        if (iter % 3 == 0) img.b.resize(static_cast<std::size_t>(rng() % size)); // truncated
        StateDecoder dec(schemaPtr, root, mem(img));
        Query q;
        q.generation = static_cast<std::uint64_t>(iter);
        std::function<void(const std::string&, int)> walk = [&](const std::string& id, int depth) {
            NodeInfo n = dec.node(id, q);
            (void)json(n);
            if (depth > 4 || n.childCount == 0) return;
            for (ChildView v : {ChildView::Logical, ChildView::Raw}) {
                ChildrenPage page = dec.children(id, {v, 0, 6, v == ChildView::Logical}, q);
                for (auto& c : page.items) walk(c.id, depth + 1);
            }
        };
        walk("", 0);
        for (const char* t : {"f:coll", "f:map", "f:set", "f:list", "f:arr"}) {
            TableInfo ti = dec.describeTable(t, "", q);
            TableRequest tr;
            tr.id = t;
            tr.limit = 20;
            for (const TableColumn& c : ti.columns)
                if (c.sortable) {
                    tr.sort = {{c.id, true}};
                    break;
                }
            (void)json(dec.tableRows(tr, q));
        }
        for (int k = 0; k < 20; ++k) (void)dec.locate(rng() % size, q);
        (void)dec.search({"abc", "text", 5}, q);
    }
}
