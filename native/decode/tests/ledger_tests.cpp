// Ledger tables: synthetic small-capacity spectrum / universe images with the exact record encodings (hash slot =
// publicKey.u32[0] & (capacity - 1), linear probing) and the real epoch-229 files.
#include <doctest/doctest.h>

#include <chrono>
#include <iostream>

#include "qstate/decode/decoder.h"
#include "qstate/decode/json.h"
#include "qstate/schema/extract.h"
#include "real_helpers.h"
#include "test_helpers.h"

using namespace qstate;
using namespace qstate::decode;
using namespace qstate::decode::testing;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

constexpr std::uint64_t kCap = 64;

struct LedgerSchemaIds {
    std::shared_ptr<const schema::Schema> schema;
    TypeId entityRecord = 0, spectrumTable = 0, assetRecord = 0, universeTable = 0;
};

// EntityRecord / AssetRecord exactly as src/network_messages/{entity,assets}.h lay them out, plus the synthetic
// recordType[kCap] table types the extraction adds.
LedgerSchemaIds ledgerSchema() {
    SB sb;
    LedgerSchemaIds ids;
    ids.entityRecord = sb.record("EntityRecord", {{"publicKey", sb.id()},
                                                  {"incomingAmount", sb.s64()},
                                                  {"outgoingAmount", sb.s64()},
                                                  {"numberOfIncomingTransfers", sb.u32()},
                                                  {"numberOfOutgoingTransfers", sb.u32()},
                                                  {"latestIncomingTransferTick", sb.u32()},
                                                  {"latestOutgoingTransferTick", sb.u32()}});
    sb.s.types[ids.entityRecord].role.kind = RoleKind::EntityRecord;
    REQUIRE(sb.size(ids.entityRecord) == 64);
    const TypeId issuance = sb.record("AssetRecord::varStruct::issuance",
                                      {{"publicKey", sb.id()},
                                       {"type", sb.u8()},
                                       {"name", sb.array(sb.chr(), 7)},
                                       {"numberOfDecimalPlaces", sb.chr()},
                                       {"unitOfMeasurement", sb.array(sb.chr(), 7)}});
    const TypeId ownership = sb.record("AssetRecord::varStruct::ownership",
                                       {{"publicKey", sb.id()},
                                        {"type", sb.u8()},
                                        {"padding", sb.array(sb.chr(), 1)},
                                        {"managingContractIndex", sb.u16()},
                                        {"issuanceIndex", sb.u32()},
                                        {"numberOfShares", sb.s64()}});
    const TypeId possession = sb.record("AssetRecord::varStruct::possession",
                                        {{"publicKey", sb.id()},
                                         {"type", sb.u8()},
                                         {"padding", sb.array(sb.chr(), 1)},
                                         {"managingContractIndex", sb.u16()},
                                         {"ownershipIndex", sb.u32()},
                                         {"numberOfShares", sb.s64()}});
    const TypeId varStruct = sb.record("AssetRecord::varStruct", {{"issuance", issuance}, {"ownership", ownership}, {"possession", possession}}, true);
    ids.assetRecord = sb.record("AssetRecord", {{"varStruct", varStruct}});
    sb.s.types[ids.assetRecord].role.kind = RoleKind::AssetRecord;
    REQUIRE(sb.size(ids.assetRecord) == 48);
    REQUIRE(sb.size(issuance) == 48);
    REQUIRE(sb.size(ownership) == 48);

    ids.spectrumTable = sb.array(ids.entityRecord, kCap);
    sb.s.types[ids.spectrumTable].role = {RoleKind::SpectrumTable, ids.entityRecord, schema::kNoType, schema::kNoType, kCap};
    ids.universeTable = sb.array(ids.assetRecord, kCap);
    sb.s.types[ids.universeTable].role = {RoleKind::UniverseTable, ids.assetRecord, schema::kNoType, schema::kNoType, kCap};
    ids.schema = sb.finish();
    return ids;
}

std::uint32_t homeSlot(const std::vector<std::uint8_t>& key) {
    std::uint32_t h = 0;
    for (int i = 3; i >= 0; --i) h = (h << 8) | key[static_cast<std::size_t>(i)];
    return h & (kCap - 1);
}

// Spectrum image writer: slot = key.u32[0] & (cap - 1), linear probing; returns the slot used.
struct SpectrumImg {
    Img img{kCap * 64};
    std::uint32_t add(const std::vector<std::uint8_t>& key, std::int64_t in, std::int64_t out, std::uint32_t nIn = 1,
                      std::uint32_t nOut = 0, std::uint32_t tickIn = 100, std::uint32_t tickOut = 0) {
        std::uint32_t slot = homeSlot(key);
        while (true) {
            bool empty = true;
            for (int i = 0; i < 32; ++i) empty = empty && img.b[slot * 64 + static_cast<std::size_t>(i)] == 0;
            if (empty) break;
            slot = (slot + 1) & (kCap - 1);
        }
        const std::uint64_t o = std::uint64_t{slot} * 64;
        img.putBytes(o, key);
        img.put(o + 32, static_cast<std::uint64_t>(in));
        img.put(o + 40, static_cast<std::uint64_t>(out));
        img.put(o + 48, nIn, 4);
        img.put(o + 52, nOut, 4);
        img.put(o + 56, tickIn, 4);
        img.put(o + 60, tickOut, 4);
        return slot;
    }
};

struct UniverseImg {
    Img img{kCap * 48};
    std::uint32_t freeSlot(const std::vector<std::uint8_t>& key) {
        std::uint32_t slot = homeSlot(key);
        while (img.b[slot * 48 + 32] != 0) slot = (slot + 1) & (kCap - 1);
        return slot;
    }
    std::uint32_t issuance(const std::vector<std::uint8_t>& key, const std::string& name, int decimals) {
        const std::uint32_t slot = freeSlot(key);
        const std::uint64_t o = std::uint64_t{slot} * 48;
        img.putBytes(o, key);
        img.b[o + 32] = 1;
        img.putText(o + 33, name);
        img.b[o + 40] = static_cast<std::uint8_t>(decimals);
        return slot;
    }
    std::uint32_t ownership(const std::vector<std::uint8_t>& key, std::uint16_t contract, std::uint32_t issuanceIndex, std::int64_t shares) {
        const std::uint32_t slot = freeSlot(key);
        const std::uint64_t o = std::uint64_t{slot} * 48;
        img.putBytes(o, key);
        img.b[o + 32] = 2;
        img.put(o + 34, contract, 2);
        img.put(o + 36, issuanceIndex, 4);
        img.put(o + 40, static_cast<std::uint64_t>(shares));
        return slot;
    }
    std::uint32_t possession(const std::vector<std::uint8_t>& key, std::uint16_t contract, std::uint32_t ownershipIndex, std::int64_t shares) {
        const std::uint32_t slot = ownership(key, contract, ownershipIndex, shares);
        img.b[std::uint64_t{slot} * 48 + 32] = 3;
        return slot;
    }
};

std::vector<std::uint64_t> slotsOf(const ChildrenPage& page) {
    std::vector<std::uint64_t> v;
    for (const NodeInfo& n : page.items) v.push_back(std::stoull(n.id.substr(n.id.rfind(':') + 1)));
    return v;
}

std::vector<std::string> columnIds(const TableInfo& t) {
    std::vector<std::string> v;
    for (const TableColumn& c : t.columns) v.push_back(c.id);
    return v;
}

std::size_t col(const TableInfo& t, const std::string& id) {
    for (std::size_t i = 0; i < t.columns.size(); ++i)
        if (t.columns[i].id == id) return i;
    FAIL("no column " << id);
    return 0;
}

std::string hexOf(const std::vector<std::uint8_t>& key) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : key) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

// The short rendering of an identity in previews: first 8 letters, an ellipsis, last 8 letters.
std::string shortId(const std::vector<std::uint8_t>& key) {
    const std::string id = testCodec()->encode(key.data());
    return id.substr(0, 8) + "\xE2\x80\xA6" + id.substr(52);
}

DecoderConfig cfg(std::shared_ptr<DecodeCache> cache = nullptr) {
    DecoderConfig c;
    c.identity = testCodec();
    c.cache = cache;
    c.contractName = [](std::uint32_t i) { return i == 1 ? std::string("QX") : std::string(); };
    return c;
}

} // namespace

TEST_CASE("ledger: spectrum table tree, probing, previews, reveal, locate") {
    const LedgerSchemaIds ids = ledgerSchema();
    SpectrumImg s;
    // slot 1: contract 1 (QX); slot 5: key A; key B collides at 5 -> 6; key C has home 6 -> 7; slot 40: negative balance
    const auto kQx = idBytes(1), kA = idBytes(5, 11, 12, 13), kB = idBytes(5 + 64, 1), kC = idBytes(6, 2), kD = idBytes(40, 3);
    CHECK(s.add(kQx, 1000, 100) == 1);
    CHECK(s.add(kA, 500, 0) == 5);
    CHECK(s.add(kB, 70, 20) == 6);
    CHECK(s.add(kC, 9, 9) == 7);
    CHECK(s.add(kD, 10, 30) == 40);
    StateDecoder dec(ids.schema, ids.spectrumTable, mem(s.img), cfg());
    REQUIRE(dec.ledgerKind() == schema::LedgerKind::Spectrum);

    NodeInfo root = dec.node("");
    CHECK(root.kind == NodeKind::Array);
    CHECK(root.childCount == 5);
    CHECK(root.rawChildCount == kCap);
    CHECK(root.tabular);
    REQUIRE(root.container);
    CHECK(root.container->capacity == kCap);
    CHECK(root.container->population == 5);
    CHECK(root.preview == "5 / 64 entities");

    CHECK(slotsOf(dec.children("")) == std::vector<std::uint64_t>{1, 5, 6, 7, 40});
    ChildrenRequest raw;
    raw.view = ChildView::Raw;
    raw.limit = 1000;
    CHECK(dec.children("", raw).total == kCap);
    CHECK(dec.children("", raw).items.size() == kCap);
    raw.hideEmpty = true;
    CHECK(slotsOf(dec.children("", raw)) == std::vector<std::uint64_t>{1, 5, 6, 7, 40});
    ChildrenRequest page;
    page.offset = 2;
    page.limit = 2;
    CHECK(slotsOf(dec.children("", page)) == std::vector<std::uint64_t>{6, 7});

    NodeInfo qx = dec.node("i:1");
    CHECK(qx.kind == NodeKind::Struct);
    CHECK(qx.childCount == 7);
    CHECK(qx.preview == "contract 1 (QX), balance 900");
    CHECK(qx.offset == 64);
    CHECK(qx.size == 64);
    ChildrenPage fields = dec.children("i:1");
    REQUIRE(fields.items.size() == 7);
    CHECK(fields.items[0].id == "i:1/f:publicKey");
    REQUIRE(fields.items[0].value);
    CHECK(fields.items[0].value->contractIndex == 1u);
    CHECK(fields.items[2].label == "outgoingAmount");
    CHECK(fields.items[2].value->v == "100");
    CHECK(dec.node("i:40").preview.find("balance -20") != std::string::npos);
    CHECK(dec.node("i:9").preview == "empty slot");
    CHECK_THROWS_AS(dec.node("i:64"), NotFoundError);
    CHECK_THROWS_AS(dec.node("e:1"), NotFoundError);

    // reveal: live records are positioned among the live ones; an empty slot only exists in the raw view
    NodeReveal r = dec.reveal("i:7/f:incomingAmount");
    REQUIRE(r.path.size() == 3);
    CHECK(r.path[0].childTotal == 5);
    CHECK(r.path[1].index == 3);
    CHECK_FALSE(r.path[1].raw);
    CHECK(r.path[1].childTotal == 7);
    CHECK(r.path[2].index == 1);
    CHECK(r.blocked.empty());
    r = dec.reveal("i:9");
    REQUIRE(r.path.size() == 2);
    CHECK(r.path[1].index == 9);
    CHECK(r.path[1].raw);
    CHECK(r.path[0].childTotal == kCap);

    NodeLocation loc = dec.locate(6 * 64 + 40);
    CHECK(loc.id == "i:6/f:outgoingAmount");
    REQUIRE(loc.path.size() == 3);
    CHECK(loc.path[1].id == "i:6");
    CHECK_THROWS_AS(dec.locate(kCap * 64), NotFoundError);
}

TEST_CASE("ledger: spectrum summary, lookup and identity search") {
    const LedgerSchemaIds ids = ledgerSchema();
    SpectrumImg s;
    const auto kQx = idBytes(1), kA = idBytes(5, 11, 12, 13), kB = idBytes(5 + 64, 1), kC = idBytes(6, 2), kD = idBytes(40, 3);
    s.add(kQx, 1000, 100);
    s.add(kA, 500, 0);
    s.add(kB, 70, 20);
    s.add(kC, 9, 9);
    s.add(kD, 10, 30);
    StateDecoder dec(ids.schema, ids.spectrumTable, mem(s.img), cfg());

    LedgerSummary sum = dec.ledgerSummary();
    CHECK(sum.kind == schema::LedgerKind::Spectrum);
    CHECK(sum.capacity == kCap);
    CHECK(sum.population == 5);
    CHECK(sum.nonZeroBalances == 3);
    CHECK(sum.totalSupply == std::to_string(900 + 500 + 50 + 0 - 20));
    CHECK(sum.maxSlot == 1);
    CHECK(sum.maxBalance == "900");
    CHECK(sum.maxIdentity == testCodec()->encode(kQx.data()));
    nlohmann::json j = sum;
    CHECK(j["kind"] == "spectrum");
    CHECK(j["maxBalance"]["slot"] == 1);
    CHECK(j["totalSupply"] == "1430");
    CHECK_FALSE(j.contains("assets"));

    // lookup by hex key: probing walks past the colliding records
    LedgerLookup l = dec.ledgerLookup(hexOf(kB));
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].slot == 6);
    CHECK(l.hits[0].id == "i:6");
    CHECK(l.hits[0].preview.find("balance 50") != std::string::npos);
    CHECK_FALSE(l.hits[0].type);
    CHECK(l.probes == 2);
    CHECK(l.identity == testCodec()->encode(kB.data()));
    l = dec.ledgerLookup(hexOf(kC));
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].slot == 7);
    CHECK(l.probes == 2);
    // by identity text (checksum of the test codec)
    l = dec.ledgerLookup(testCodec()->encode(kA.data()));
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].slot == 5);
    CHECK(l.probes == 1);
    // absent key with home slot 5: probes 5, 6, 7 and stops at the empty slot 8
    l = dec.ledgerLookup(hexOf(idBytes(5, 99)));
    CHECK(l.hits.empty());
    CHECK(l.probes == 4);
    // absent key in an empty region
    l = dec.ledgerLookup(hexOf(idBytes(20)));
    CHECK(l.hits.empty());
    CHECK(l.probes == 1);
    CHECK_THROWS_AS(dec.ledgerLookup("not an identity"), InvalidArgumentError);
    CHECK_THROWS_AS(dec.ledgerLookup(std::string(60, 'B')), InvalidArgumentError); // checksum mismatch
    nlohmann::json lj = dec.ledgerLookup(hexOf(kB));
    CHECK(lj["hits"][0]["slot"] == 6);
    CHECK(lj["probes"] == 2);

    // search by identity uses the hash lookup; other patterns still scan the bytes
    SearchRequest sr;
    sr.query = testCodec()->encode(kB.data());
    SearchResult res = dec.search(sr);
    CHECK(res.mode == "id");
    CHECK(res.note.find("hash lookup") != std::string::npos);
    REQUIRE(res.matches.size() == 1);
    CHECK(res.matches[0].offset == 6 * 64);
    CHECK(res.matches[0].length == 32);
    CHECK(res.matches[0].location.id == "i:6/f:publicKey");
    sr.query = "500";
    res = dec.search(sr);
    CHECK(res.mode == "int");
    CHECK(res.note.find("hash lookup") == std::string::npos);
    REQUIRE(res.matches.size() == 1);
    CHECK(res.matches[0].location.id == "i:5/f:incomingAmount");
}

TEST_CASE("ledger: spectrum table view with the derived balance column") {
    const LedgerSchemaIds ids = ledgerSchema();
    SpectrumImg s;
    const auto kQx = idBytes(1), kA = idBytes(5, 11, 12, 13), kB = idBytes(5 + 64, 1), kC = idBytes(6, 2), kD = idBytes(40, 3);
    s.add(kQx, 1000, 100);
    s.add(kA, 500, 0);
    s.add(kB, 70, 20);
    s.add(kC, 9, 9);
    s.add(kD, 10, 30);
    // a balance beyond 64 bits: incoming = INT64_MAX, outgoing = INT64_MIN + 1
    const auto kBig = idBytes(50, 7);
    s.add(kBig, std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min() + 1);
    StateDecoder dec(ids.schema, ids.spectrumTable, mem(s.img), cfg());

    TableInfo t = dec.describeTable("");
    CHECK(t.view == "entities");
    REQUIRE(t.views.size() == 1);
    CHECK(t.totalRows == 6);
    CHECK(columnIds(t) == std::vector<std::string>{"$slot", "publicKey", "balance", "incomingAmount", "outgoingAmount",
                                                   "numberOfIncomingTransfers", "numberOfOutgoingTransfers",
                                                   "latestIncomingTransferTick", "latestOutgoingTransferTick"});
    CHECK(t.columns[col(t, "balance")].kind == "int");
    CHECK(t.columns[col(t, "balance")].sortable);
    CHECK(t.columns[col(t, "publicKey")].group == "key");
    CHECK(t.elementTypeId == ids.entityRecord);
    REQUIRE(t.container);
    CHECK(t.container->population == 6);
    CHECK_THROWS_AS(dec.describeTable("", "povs"), NotFoundError);

    TableRequest req;
    req.id = "";
    req.limit = 10;
    TablePage p = dec.tableRows(req);
    REQUIRE(p.rows.size() == 6);
    CHECK(p.rows[0].index == 1);
    CHECK(p.rows[0].id == "i:1");
    CHECK(p.rows[0].cells[col(t, "balance")].v == "900");
    CHECK(p.rows[0].cells[col(t, "balance")].bits == 64);
    CHECK(p.rows[0].cells[col(t, "publicKey")].contractIndex == 1u);
    CHECK(p.rows[4].cells[col(t, "balance")].v == "-20");
    CHECK(p.rows[4].cells[col(t, "balance")].hex == "0xffffffffffffffec");
    const LeafValue& big = p.rows[5].cells[col(t, "balance")];
    CHECK(big.v == "18446744073709551614");
    CHECK(big.bits == 128);
    CHECK(big.hex == "0x0000000000000000fffffffffffffffe");

    req.sort = {{"balance", true}};
    p = dec.tableRows(req);
    std::vector<std::uint64_t> order;
    for (const TableRow& r : p.rows) order.push_back(r.index);
    CHECK(order == std::vector<std::uint64_t>{50, 1, 5, 6, 7, 40});
    req.sort = {{"balance", false}};
    p = dec.tableRows(req);
    CHECK(p.rows[0].index == 40);
    CHECK(p.rows[1].index == 7);

    req.sort.clear();
    req.filters = {{"balance", "gt", std::string("60")}};
    p = dec.tableRows(req);
    CHECK(p.total == 3);
    req.filters = {{"balance", "zero", std::nullopt}};
    p = dec.tableRows(req);
    CHECK(p.total == 1);
    CHECK(p.rows[0].index == 7);
    req.filters = {{"balance", "lt", std::string("0")}};
    p = dec.tableRows(req);
    CHECK(p.total == 1);
    CHECK(p.rows[0].index == 40);
    req.filters = {{"publicKey", "eq", testCodec()->encode(kB.data())}};
    p = dec.tableRows(req);
    CHECK(p.total == 1);
    CHECK(p.rows[0].index == 6);
    req.filters = {{"incomingAmount", "ge", std::string("500")}};
    req.sort = {{"$slot", true}};
    p = dec.tableRows(req);
    REQUIRE(p.total == 3);
    CHECK(p.rows[0].index == 50);
    req.filters = {{"balance", "eq", std::string("abc")}};
    CHECK_THROWS_AS(dec.tableRows(req), InvalidArgumentError);
}

TEST_CASE("ledger: universe records select the union alternative by type") {
    const LedgerSchemaIds ids = ledgerSchema();
    UniverseImg u;
    // NOST issued by key I at slot 3; I also owns and possesses it (same key: probing puts the records at 4 and 5);
    // key O (home 10) owns shares of NOST managed by contract 1; key P (home 10 -> probes to 11) possesses them.
    const auto kI = idBytes(3, 5), kO = idBytes(10, 6), kP = idBytes(10 + 64, 7), kX = idBytes(30, 8);
    const std::uint32_t sNost = u.issuance(kI, "NOST", 0);
    CHECK(sNost == 3);
    CHECK(u.ownership(kI, 1, sNost, 1'000'000) == 4);
    CHECK(u.possession(kI, 1, 4, 1'000'000) == 5);
    const std::uint32_t sO = u.ownership(kO, 1, sNost, 2500);
    CHECK(sO == 10);
    CHECK(u.possession(kP, 1, sO, 2500) == 11);
    const std::uint32_t sQft = u.issuance(kX, "QFT", 2);
    CHECK(sQft == 30);
    CHECK(u.ownership(kX, 1, sQft, 7) == 31);
    StateDecoder dec(ids.schema, ids.universeTable, mem(u.img), cfg());
    REQUIRE(dec.ledgerKind() == schema::LedgerKind::Universe);

    NodeInfo root = dec.node("");
    CHECK(root.childCount == 7);
    CHECK(root.preview == "7 / 64 assets: 2 issuances, 3 ownerships, 2 possessions");
    CHECK(slotsOf(dec.children("")) == std::vector<std::uint64_t>{3, 4, 5, 10, 11, 30, 31});

    // issuance
    NodeInfo is = dec.node("i:3");
    CHECK(is.kind == NodeKind::Struct);
    CHECK(is.preview.find("issuance NOST, 0 decimals") == 0);
    CHECK(is.childCount == 5);
    CHECK(is.rawChildCount == 1);
    ChildrenPage kids = dec.children("i:3");
    REQUIRE(kids.items.size() == 5);
    CHECK(kids.items[0].id == "i:3/a:publicKey");
    CHECK(kids.items[1].id == "i:3/a:type");
    CHECK(kids.items[1].value->k == LeafValue::Kind::Enum);
    CHECK(kids.items[1].value->v == "1");
    CHECK(kids.items[1].value->name == "issuance");
    CHECK(kids.items[2].label == "name");
    CHECK(kids.items[2].value->text == "NOST");
    CHECK(kids.items[2].offset == 3 * 48 + 33);
    CHECK(kids.items[4].label == "unitOfMeasurement");
    // ownership
    NodeInfo own = dec.node("i:10");
    CHECK(own.preview == "ownership of NOST: 2500 shares, held by " + shortId(kO) + ", managed by contract 1 (QX)");
    kids = dec.children("i:10");
    REQUIRE(kids.items.size() == 6);
    CHECK(kids.items[4].label == "issuanceIndex");
    CHECK(kids.items[4].value->v == "3");
    CHECK(kids.items[5].label == "numberOfShares");
    CHECK(kids.items[5].value->v == "2500");
    CHECK(dec.node("i:10/a:numberOfShares").offset == 10 * 48 + 40);
    // possession: the asset name is resolved through the ownership
    CHECK(dec.node("i:11").preview.find("possession of NOST: 2500 shares") == 0);
    CHECK(dec.children("i:11").items[4].label == "ownershipIndex");
    // raw view: the union as laid out
    ChildrenRequest raw;
    raw.view = ChildView::Raw;
    kids = dec.children("i:10", raw);
    REQUIRE(kids.items.size() == 1);
    CHECK(kids.items[0].id == "i:10/f:varStruct");
    CHECK(kids.items[0].kind == NodeKind::Union);
    kids = dec.children("i:10/f:varStruct");
    REQUIRE(kids.items.size() == 3);
    CHECK(kids.items[1].id == "i:10/f:varStruct/f:ownership");
    CHECK(dec.node("i:10/f:varStruct/f:ownership/f:numberOfShares").value->v == "2500");
    // empty slot: only the common prefix
    kids = dec.children("i:20");
    REQUIRE(kids.items.size() == 2);
    CHECK(kids.items[1].label == "type");
    CHECK(dec.node("i:20").preview == "empty slot");
    CHECK_THROWS_AS(dec.node("i:10/a:name"), NotFoundError); // not a member of the ownership alternative

    // locate / reveal follow the logical path
    NodeLocation loc = dec.locate(10 * 48 + 36);
    CHECK(loc.id == "i:10/a:issuanceIndex");
    NodeReveal r = dec.reveal("i:11/a:numberOfShares");
    REQUIRE(r.path.size() == 3);
    CHECK(r.path[1].index == 4); // rank among the live records
    CHECK(r.path[2].index == 5);
    CHECK_FALSE(r.path[2].raw);
    r = dec.reveal("i:10/f:varStruct/f:ownership");
    REQUIRE(r.path.size() == 4);
    CHECK(r.path[2].raw);
    CHECK(r.path[2].index == 0);
    CHECK(r.path[3].index == 1);

    // lookup: every record of a key (the issuer owns and possesses its own asset)
    LedgerLookup l = dec.ledgerLookup(hexOf(kI));
    REQUIRE(l.hits.size() == 3);
    CHECK(l.hits[0].slot == 3);
    CHECK(l.hits[0].type == 1u);
    CHECK(l.hits[1].slot == 4);
    CHECK(l.hits[1].type == 2u);
    CHECK(l.hits[2].slot == 5);
    CHECK(l.hits[2].type == 3u);
    CHECK(l.probes == 4); // 3, 4, 5 and the empty slot 6 that ends the sequence
    l = dec.ledgerLookup(hexOf(kP));
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].slot == 11);
    CHECK(l.probes == 3);
    SearchRequest sr;
    sr.query = testCodec()->encode(kI.data());
    SearchResult res = dec.search(sr);
    CHECK(res.matches.size() == 3);
    CHECK(res.matches[2].location.id == "i:5/a:publicKey");

    // summary
    LedgerSummary sum = dec.ledgerSummary();
    CHECK(sum.population == 7);
    CHECK(sum.issuances == 2);
    CHECK(sum.ownerships == 3);
    CHECK(sum.possessions == 2);
    REQUIRE(sum.assets.size() == 2);
    CHECK(sum.assets[0].slot == 3);
    CHECK(sum.assets[0].name == "NOST");
    CHECK(sum.assets[0].issuer == testCodec()->encode(kI.data()));
    CHECK(sum.assets[0].ownerships == 2);
    CHECK(sum.assets[0].possessions == 2);
    CHECK(sum.assets[0].totalShares == "1002500");
    CHECK(sum.assets[0].unitOfMeasurement == "00000000000000");
    CHECK(sum.assets[1].name == "QFT");
    CHECK(sum.assets[1].numberOfDecimalPlaces == 2);
    CHECK(sum.assets[1].ownerships == 1);
    CHECK(sum.assets[1].possessions == 0);
    nlohmann::json j = sum;
    CHECK(j["kind"] == "universe");
    CHECK(j["assets"].size() == 2);
    CHECK(j["assets"][0]["totalShares"] == "1002500");
    CHECK_FALSE(j.contains("totalSupply"));
}

TEST_CASE("ledger: universe table views, derived asset / $type columns, sorting and filtering") {
    const LedgerSchemaIds ids = ledgerSchema();
    UniverseImg u;
    const auto kI = idBytes(3, 5), kO = idBytes(10, 6), kP = idBytes(10 + 64, 7), kX = idBytes(30, 8);
    const std::uint32_t sNost = u.issuance(kI, "NOST", 0);
    u.ownership(kI, 1, sNost, 1'000'000);
    u.possession(kI, 1, 4, 1'000'000);
    const std::uint32_t sO = u.ownership(kO, 2, sNost, 2500);
    u.possession(kP, 2, sO, 2500);
    const std::uint32_t sQft = u.issuance(kX, "QFT", 2);
    u.ownership(kX, 1, sQft, 7);
    StateDecoder dec(ids.schema, ids.universeTable, mem(u.img), cfg());

    TableInfo all = dec.describeTable("");
    CHECK(all.view == "all");
    REQUIRE(all.views.size() == 4);
    CHECK(all.totalRows == 7);
    CHECK(columnIds(all) == std::vector<std::string>{"$slot", "$type", "publicKey", "asset", "name", "numberOfDecimalPlaces",
                                                     "unitOfMeasurement", "managingContractIndex", "issuanceIndex",
                                                     "numberOfShares", "ownershipIndex"});
    CHECK(all.columns[col(all, "$type")].kind == "enum");
    CHECK(all.columns[col(all, "asset")].kind == "int");
    CHECK(all.columns[col(all, "numberOfDecimalPlaces")].kind == "int");
    TableInfo iss = dec.describeTable("", "issuances");
    CHECK(iss.totalRows == 2);
    CHECK(columnIds(iss) == std::vector<std::string>{"$slot", "publicKey", "asset", "name", "numberOfDecimalPlaces", "unitOfMeasurement"});
    TableInfo own = dec.describeTable("", "ownerships");
    CHECK(own.totalRows == 3);
    CHECK(columnIds(own) == std::vector<std::string>{"$slot", "publicKey", "asset", "managingContractIndex", "issuanceIndex", "numberOfShares"});
    TableInfo pos = dec.describeTable("", "possessions");
    CHECK(pos.totalRows == 2);
    CHECK(columnIds(pos) == std::vector<std::string>{"$slot", "publicKey", "asset", "managingContractIndex", "issuanceIndex",
                                                     "ownershipIndex", "numberOfShares"});
    CHECK(pos.columns[col(pos, "issuanceIndex")].kind == "int");
    CHECK(pos.columns[col(pos, "issuanceIndex")].sortable);
    CHECK(pos.columns[col(pos, "issuanceIndex")].filterable);
    CHECK(all.columns[col(all, "issuanceIndex")].kind == "int");
    CHECK(own.columns[col(own, "issuanceIndex")].kind == "int");
    CHECK_THROWS_AS(dec.describeTable("", "entities"), NotFoundError);

    TableRequest req;
    req.id = "";
    req.limit = 20;
    TablePage p = dec.tableRows(req);
    REQUIRE(p.rows.size() == 7);
    // row 0: the NOST issuance
    CHECK(p.rows[0].index == 3);
    CHECK(p.rows[0].cells[col(all, "$type")].k == LeafValue::Kind::Enum);
    CHECK(p.rows[0].cells[col(all, "$type")].name == "issuance");
    CHECK(p.rows[0].cells[col(all, "asset")].text == "NOST");
    CHECK(p.rows[0].cells[col(all, "name")].text == "NOST");
    CHECK(p.rows[0].cells[col(all, "numberOfDecimalPlaces")].v == "0");
    CHECK(p.rows[0].cells[col(all, "numberOfShares")].k == LeafValue::Kind::Unavailable);
    CHECK(p.rows[0].cells[col(all, "numberOfShares")].v == "not in an issuance record");
    CHECK(p.rows[0].cells[col(all, "issuanceIndex")].v == "3"); // an issuance belongs to itself
    // row 3: ownership of O, contract 2 (no name) ; row 4: possession of P
    CHECK(p.rows[3].index == 10);
    CHECK(p.rows[3].cells[col(all, "$type")].name == "ownership");
    CHECK(p.rows[3].cells[col(all, "asset")].text == "NOST");
    CHECK(p.rows[3].cells[col(all, "issuanceIndex")].v == "3");
    CHECK(p.rows[3].cells[col(all, "managingContractIndex")].v == "2");
    CHECK_FALSE(p.rows[3].cells[col(all, "managingContractIndex")].text);
    CHECK(p.rows[3].cells[col(all, "name")].k == LeafValue::Kind::Unavailable);
    CHECK(p.rows[4].index == 11);
    CHECK(p.rows[4].cells[col(all, "asset")].text == "NOST");
    CHECK(p.rows[4].cells[col(all, "ownershipIndex")].v == "10");
    CHECK(p.rows[4].cells[col(all, "issuanceIndex")].v == "3"); // derived: ownership 10 -> issuance 3
    CHECK(p.rows[4].cells[col(all, "issuanceIndex")].isUnsigned);
    CHECK(p.rows[4].cells[col(all, "issuanceIndex")].bits == 32);
    // contract name chip: managingContractIndex 1 -> "QX"
    CHECK(p.rows[1].cells[col(all, "managingContractIndex")].text == "QX");
    CHECK(p.rows[5].cells[col(all, "numberOfDecimalPlaces")].v == "2");

    // sort by asset (text order), then by shares desc
    req.sort = {{"asset", false}, {"numberOfShares", true}};
    p = dec.tableRows(req);
    std::vector<std::uint64_t> order;
    for (const TableRow& r : p.rows) order.push_back(r.index);
    CHECK(order == std::vector<std::uint64_t>{4, 5, 10, 11, 3, 31, 30});
    req.sort = {{"$type", false}, {"$slot", true}};
    p = dec.tableRows(req);
    order.clear();
    for (const TableRow& r : p.rows) order.push_back(r.index);
    CHECK(order == std::vector<std::uint64_t>{30, 3, 31, 10, 4, 11, 5});

    // filters
    req.sort.clear();
    req.filters = {{"asset", "eq", std::string("QFT")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2);
    req.filters = {{"asset", "contains", std::string("no")}};
    p = dec.tableRows(req);
    CHECK(p.total == 5);
    req.filters = {{"$type", "eq", std::string("possession")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2);
    req.filters = {{"$type", "ne", std::string("1")}};
    p = dec.tableRows(req);
    CHECK(p.total == 5);
    req.filters = {{"numberOfShares", "gt", std::string("1000")}};
    p = dec.tableRows(req);
    CHECK(p.total == 4); // rows of other types never match
    req.filters = {{"managingContractIndex", "eq", std::string("2")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2);
    // issuanceIndex in `all` selects everything that belongs to the asset: the issuance, its ownerships, its possessions
    req.filters = {{"issuanceIndex", "eq", std::string("3")}};
    p = dec.tableRows(req);
    CHECK(p.total == 5); // NOST: slots 3, 4, 5, 10, 11
    req.filters = {{"issuanceIndex", "eq", std::string("30")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2); // QFT: slots 30, 31
    req.filters = {{"issuanceIndex", "ne", std::string("3")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2);
    req.filters = {{"issuanceIndex", "eq", std::string("x")}};
    CHECK_THROWS_AS(dec.tableRows(req), InvalidArgumentError);
    req.filters = {{"$type", "eq", std::string("nothing")}};
    CHECK_THROWS_AS(dec.tableRows(req), InvalidArgumentError);

    // the per-type views
    req.filters.clear();
    req.view = "possessions";
    req.sort = {{"numberOfShares", true}};
    p = dec.tableRows(req);
    REQUIRE(p.rows.size() == 2);
    CHECK(p.rows[0].index == 5);
    CHECK(p.rows[0].cells[col(pos, "asset")].text == "NOST");
    CHECK(p.rows[0].cells[col(pos, "issuanceIndex")].v == "3");
    CHECK(p.rows[1].cells[col(pos, "ownershipIndex")].v == "10");
    CHECK(p.rows[1].cells[col(pos, "issuanceIndex")].v == "3");
    // the filter the UI carries from the ownerships view to the possessions view
    req.sort.clear();
    req.filters = {{"issuanceIndex", "eq", std::string("3")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2);
    req.filters = {{"issuanceIndex", "eq", std::string("30")}};
    p = dec.tableRows(req);
    CHECK(p.total == 0);
    req.filters.clear();
    req.view = "ownerships";
    req.filters = {{"issuanceIndex", "eq", std::string("3")}};
    p = dec.tableRows(req);
    CHECK(p.total == 2);
    req.filters.clear();
    req.view = "issuances";
    req.sort = {{"name", false}};
    p = dec.tableRows(req);
    REQUIRE(p.rows.size() == 2);
    CHECK(p.rows[0].index == 3);
    CHECK(p.rows[1].cells[col(iss, "name")].text == "QFT");
}

TEST_CASE("ledger: the derived issuanceIndex of a possession whose ownership slot is empty is unavailable") {
    const LedgerSchemaIds ids = ledgerSchema();
    UniverseImg u;
    const auto kI = idBytes(3, 5), kO = idBytes(10, 6), kP = idBytes(12, 7), kD = idBytes(20, 9);
    const std::uint32_t sNost = u.issuance(kI, "NOST", 0);
    const std::uint32_t sO = u.ownership(kO, 1, sNost, 2500);
    u.possession(kP, 1, sO, 2500);
    u.possession(kD, 1, 40, 7);     // slot 40 is empty
    u.possession(kD, 1, sNost, 7);  // slot 3 is an issuance, not an ownership (lands in slot 21)
    StateDecoder dec(ids.schema, ids.universeTable, mem(u.img), cfg());

    TableInfo pos = dec.describeTable("", "possessions");
    CHECK(pos.totalRows == 3);
    TableRequest req;
    req.id = "";
    req.view = "possessions";
    req.limit = 10;
    req.sort = {{"issuanceIndex", false}};
    TablePage p = dec.tableRows(req);
    REQUIRE(p.rows.size() == 3);
    // unresolved rows sort first (empty key), the resolved one last
    CHECK(p.rows[2].index == 12);
    CHECK(p.rows[2].cells[col(pos, "issuanceIndex")].v == "3");
    for (std::size_t i = 0; i < 2; ++i) {
        const LeafValue& v = p.rows[i].cells[col(pos, "issuanceIndex")];
        CHECK(v.k == LeafValue::Kind::Unavailable);
        CHECK(v.v.find("ownership #") == 0);
        CHECK(v.v.find("not found") != std::string::npos);
        CHECK(p.rows[i].cells[col(pos, "asset")].k == LeafValue::Kind::Unavailable);
    }
    req.sort.clear();
    req.filters = {{"issuanceIndex", "eq", std::string("3")}};
    p = dec.tableRows(req);
    CHECK(p.total == 1);
    req.filters = {{"issuanceIndex", "ne", std::string("3")}}; // unresolved never matches
    p = dec.tableRows(req);
    CHECK(p.total == 0);
    req.filters = {{"issuanceIndex", "zero", std::nullopt}};
    p = dec.tableRows(req);
    CHECK(p.total == 0);
    req.filters = {{"issuanceIndex", "nonzero", std::nullopt}};
    p = dec.tableRows(req);
    CHECK(p.total == 1);
    req.view = "all";
    req.filters = {{"issuanceIndex", "eq", std::string("3")}};
    p = dec.tableRows(req);
    CHECK(p.total == 3); // the issuance, its ownership, the one resolved possession
}

TEST_CASE("ledger: a truncated file decodes as far as it exists") {
    const LedgerSchemaIds ids = ledgerSchema();
    SpectrumImg s;
    s.add(idBytes(1), 10, 0);
    s.add(idBytes(60), 20, 0);
    Img cut(40 * 64 + 10);
    std::copy(s.img.b.begin(), s.img.b.begin() + static_cast<std::ptrdiff_t>(cut.b.size()), cut.b.begin());
    StateDecoder dec(ids.schema, ids.spectrumTable, mem(cut), cfg());
    NodeInfo root = dec.node("");
    CHECK_FALSE(root.inFile);
    CHECK(root.childCount == 1);
    REQUIRE(root.container);
    CHECK(root.container->warning.find("beyond the end") != std::string::npos);
    CHECK(dec.ledgerSummary().population == 1);
    CHECK(dec.ledgerLookup(hexOf(idBytes(60))).hits.empty());
    NodeInfo beyond = dec.node("i:60");
    CHECK_FALSE(beyond.inFile);
    CHECK(dec.describeTable("").totalRows == 1);
}

TEST_CASE("ledger: the live scan is cached per generation and shared with the tables") {
    const LedgerSchemaIds ids = ledgerSchema();
    SpectrumImg s;
    s.add(idBytes(1), 10, 0);
    auto cache = std::make_shared<DecodeCache>();
    StateDecoder dec(ids.schema, ids.spectrumTable, mem(s.img), cfg(cache));
    Query q;
    q.generation = 1;
    dec.node("", q);
    const std::size_t after = cache->entryCount();
    dec.children("", {}, q);
    dec.ledgerSummary(q);
    dec.describeTable("", "", q);
    CHECK(cache->entryCount() == after); // one scan entry serves all of them
    q.generation = 2;
    dec.node("", q);
    CHECK(cache->entryCount() == after); // the old generation was dropped, the new one scanned
}

// ---- real files ----------------------------------------------------------------------------------------------------

namespace {

struct RealLedger {
    std::shared_ptr<const schema::Schema> schema;
    std::shared_ptr<PreadSource> src;
    std::unique_ptr<StateDecoder> dec;
    bool ok = false;
};

std::shared_ptr<const schema::Schema> realSchema() {
    static std::shared_ptr<const schema::Schema> cached;
    if (cached) return cached;
    const std::string core = qstate::testing::coreDir229();
    if (core.empty()) return nullptr;
    cpp::DiskSource disk(core);
    cached = schema::extractSchema(disk).schema;
    return cached;
}

RealLedger openRealLedger(schema::LedgerKind kind, std::shared_ptr<DecodeCache> cache = nullptr) {
    RealLedger r;
    const std::string stateDir = qstate::testing::stateDir();
    r.schema = realSchema();
    if (stateDir.empty() || !r.schema) return r;
    const schema::LedgerSchema* ls = r.schema->ledger(kind);
    if (!ls || ls->rootType == schema::kNoType) return r;
    r.src = std::make_shared<PreadSource>(stateDir + "/" + schema::ledgerKindName(kind) + ".229");
    if (!r.src->ok()) return r;
    DecoderConfig c;
    c.identity = realCodec();
    c.cache = cache;
    c.scope = kind == schema::LedgerKind::Spectrum ? 2000u : 2001u;
    c.contractName = [](std::uint32_t i) { return i == 1 ? std::string("QX") : std::string(); };
    r.dec = std::make_unique<StateDecoder>(r.schema, ls->rootType, r.src, c);
    r.ok = true;
    return r;
}

} // namespace

TEST_CASE("ledger: real spectrum.229") {
    RealLedger r = openRealLedger(schema::LedgerKind::Spectrum);
    if (!r.ok) {
        MESSAGE("QSTATE_TEST_STATE_DIR / QSTATE_TEST_CORE_DIR_229 not set: skipped");
        return;
    }
    CHECK(r.src->size() == 1073741824ull);
    auto t0 = Clock::now();
    NodeInfo root = r.dec->node("");
    const double scanMs = msSince(t0);
    CHECK(root.childCount == 670379);
    CHECK(root.rawChildCount == 16777216);
    CHECK(root.inFile);
    t0 = Clock::now();
    LedgerSummary sum = r.dec->ledgerSummary();
    const double summaryMs = msSince(t0);
    CHECK(sum.population == 670379);
    CHECK(sum.totalSupply == "176923511873269");
    CHECK(sum.nonZeroBalances <= sum.population);
    CHECK(sum.nonZeroBalances > 0);
    // slots 1..3 hold the contract ids of contracts 1..3
    for (unsigned i = 1; i <= 3; ++i) {
        ChildrenPage f = r.dec->children("i:" + std::to_string(i));
        REQUIRE(f.items.size() == 7);
        REQUIRE(f.items[0].value);
        CHECK(f.items[0].value->contractIndex == i);
    }
    // lookup of the QX contract identity (contract 1): slot 1
    std::uint8_t qxKey[32] = {1};
    const std::string qxIdentity = realCodec()->encode(qxKey);
    t0 = Clock::now();
    LedgerLookup l = r.dec->ledgerLookup(qxIdentity);
    const double lookupMs = msSince(t0);
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].slot == 1);
    CHECK(l.probes == 1);
    CHECK(l.hits[0].preview.find("contract 1 (QX)") == 0);
    // an absent key probes its cluster up to the first empty slot
    std::uint8_t absent[32] = {1, 0, 0, 0, 0xAB};
    l = r.dec->ledgerLookup(realCodec()->encode(absent));
    CHECK(l.hits.empty());
    CHECK(l.probes > 1);
    // the richest entity by the table equals the summary
    TableRequest req;
    req.id = "";
    req.limit = 1;
    req.sort = {{"balance", true}};
    t0 = Clock::now();
    TablePage p = r.dec->tableRows(req);
    const double sortMs = msSince(t0);
    REQUIRE(p.rows.size() == 1);
    CHECK(p.total == 670379);
    CHECK(p.rows[0].index == sum.maxSlot);
    TableInfo ti = r.dec->describeTable("");
    CHECK(p.rows[0].cells[col(ti, "balance")].v == sum.maxBalance);
    CHECK(p.rows[0].cells[col(ti, "publicKey")].identity == sum.maxIdentity);
    t0 = Clock::now();
    p = r.dec->tableRows(req); // cached order
    const double sortAgainMs = msSince(t0);
    // identity search = hash lookup
    SearchRequest sr;
    sr.query = sum.maxIdentity;
    SearchResult res = r.dec->search(sr);
    REQUIRE(res.matches.size() == 1);
    CHECK(res.matches[0].offset == sum.maxSlot * 64);
    CHECK(res.note.find("hash lookup") != std::string::npos);
    std::cerr << "[ledger] spectrum.229: live scan " << scanMs << " ms, summary " << summaryMs << " ms (cached scan), lookup "
              << lookupMs << " ms, sort by balance " << sortMs << " ms (cached: " << sortAgainMs << " ms), supply "
              << sum.totalSupply << ", max " << sum.maxBalance << " at slot " << sum.maxSlot << "\n";
}

TEST_CASE("ledger: real universe.229") {
    RealLedger r = openRealLedger(schema::LedgerKind::Universe);
    if (!r.ok) {
        MESSAGE("QSTATE_TEST_STATE_DIR / QSTATE_TEST_CORE_DIR_229 not set: skipped");
        return;
    }
    CHECK(r.src->size() == 805306368ull);
    auto t0 = Clock::now();
    LedgerSummary sum = r.dec->ledgerSummary();
    const double scanMs = msSince(t0);
    CHECK(sum.population == 147 + 40495 + 40495);
    CHECK(sum.issuances == 147);
    CHECK(sum.ownerships == 40495);
    CHECK(sum.possessions == 40495);
    REQUIRE(sum.assets.size() == 147);
    CHECK(sum.assets[0].slot == 0);
    CHECK(sum.assets[0].name == "NOST");
    std::uint64_t ownerships = 0, possessions = 0;
    for (const auto& a : sum.assets) {
        ownerships += a.ownerships;
        possessions += a.possessions;
    }
    CHECK(ownerships == 40495);
    CHECK(possessions == 40495);
    // slot 0 NOST issuance, slot 1 ownership (contract 1, issuance 1775022, 1.61e9 shares), slot 2 its possession
    CHECK(r.dec->node("i:0").preview.find("issuance NOST") == 0);
    ChildrenPage own = r.dec->children("i:1");
    REQUIRE(own.items.size() == 6);
    CHECK(own.items[1].value->v == "2");
    CHECK(own.items[1].value->name == "ownership");
    CHECK(own.items[3].value->v == "1");
    CHECK(own.items[4].value->v == "1775022");
    CHECK(own.items[5].value->v == "1610000000");
    ChildrenPage pos = r.dec->children("i:2");
    CHECK(pos.items[1].value->v == "3");
    CHECK(pos.items[1].value->name == "possession");
    CHECK(pos.items[4].value->v == "1");
    // the name of the asset at slot 1775022 is what the derived column shows for slots 1 and 2
    const std::string nameAt = r.dec->children("i:1775022").items[2].value->text.value_or("");
    CHECK_FALSE(nameAt.empty());
    TableInfo all = r.dec->describeTable("");
    TableRequest req;
    req.id = "";
    req.limit = 3;
    TablePage p = r.dec->tableRows(req);
    REQUIRE(p.rows.size() == 3);
    CHECK(p.rows[1].cells[col(all, "asset")].text == nameAt);
    CHECK(p.rows[2].cells[col(all, "asset")].text == nameAt);
    CHECK(p.rows[1].cells[col(all, "managingContractIndex")].text == "QX");
    // QX contract identity: every universe record with that key
    std::uint8_t qxKey[32] = {1};
    LedgerLookup l = r.dec->ledgerLookup(realCodec()->encode(qxKey));
    CHECK_FALSE(l.hits.empty());
    for (const auto& h : l.hits) {
        REQUIRE(h.type);
        CHECK(*h.type >= 1);
        CHECK(*h.type <= 3);
        CHECK(r.dec->children(h.id).items[0].value->contractIndex == 1u);
    }
    // sort the ownerships by asset name and shares
    req.view = "ownerships";
    req.sort = {{"asset", false}, {"numberOfShares", true}};
    req.limit = 5;
    t0 = Clock::now();
    p = r.dec->tableRows(req);
    const double sortMs = msSince(t0);
    CHECK(p.total == 40495);
    REQUIRE(p.rows.size() == 5);
    TableInfo ownInfo = r.dec->describeTable("", "ownerships");
    const std::string first = p.rows[0].cells[col(ownInfo, "asset")].text.value_or("");
    CHECK_FALSE(first.empty());
    CHECK(first <= p.rows[4].cells[col(ownInfo, "asset")].text.value_or(""));
    // NOST (issuance slot 0): the derived issuanceIndex selects its 109 ownerships, its 109 possessions (through the
    // ownership map) and, in `all`, the issuance row itself: 1 + 109 + 109
    CHECK(sum.assets[0].ownerships == 109);
    CHECK(sum.assets[0].possessions == 109);
    req.sort.clear();
    req.limit = 1;
    req.filters = {{"issuanceIndex", "eq", std::string("0")}};
    req.view = "ownerships";
    p = r.dec->tableRows(req);
    CHECK(p.total == sum.assets[0].ownerships);
    req.view = "possessions";
    t0 = Clock::now();
    p = r.dec->tableRows(req);
    const double possFilterMs = msSince(t0);
    CHECK(p.total == sum.assets[0].possessions);
    TableInfo posInfo = r.dec->describeTable("", "possessions");
    REQUIRE(p.rows.size() == 1);
    CHECK(p.rows[0].cells[col(posInfo, "issuanceIndex")].v == "0");
    CHECK(p.rows[0].cells[col(posInfo, "asset")].text == "NOST");
    req.view = "all";
    p = r.dec->tableRows(req);
    CHECK(p.total == 1 + sum.assets[0].ownerships + sum.assets[0].possessions);
    CHECK(p.total == 219);
    std::cerr << "[ledger] universe.229: live scan + summary " << scanMs << " ms, possessions by issuanceIndex "
              << possFilterMs << " ms, sort ownerships by asset " << sortMs
              << " ms, " << l.hits.size() << " records of the QX identity, slot 1 asset " << nameAt << "\n";
}
