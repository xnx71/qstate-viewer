// Integration tests on real epoch-229 state files (skipped when QSTATE_TEST_STATE_DIR / QSTATE_SOURCE_DIR are unset).
#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <thread>

#include "qstate/decode/json.h"
#include "real_helpers.h"

using namespace qstate::decode;
using namespace qstate::decode::testing;
using nlohmann::json;

namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::uint64_t fieldOff(const qstate::schema::Schema& s, qstate::schema::TypeId rec, const char* name) {
    for (const auto& f : s.types[rec].fields)
        if (f.name == name) return f.offset;
    FAIL("no field " << name);
    return 0;
}

// independent flag counting straight from the file
struct FlagCount {
    std::uint64_t live = 0, tomb = 0, invalid = 0;
};
FlagCount countFlags(const ByteSource& src, std::uint64_t off, std::uint64_t L) {
    FlagCount fc;
    std::vector<std::uint8_t> buf((L * 2 + 7) / 8);
    REQUIRE(src.read(off, buf.size(), buf.data()) == buf.size());
    for (std::uint64_t i = 0; i < L; ++i) {
        const unsigned st = (buf[i / 4] >> ((i % 4) * 2)) & 3u;
        if (st == 1) ++fc.live;
        else if (st == 2) ++fc.tomb;
        else if (st == 3) ++fc.invalid;
    }
    return fc;
}

std::uint64_t u64At(const ByteSource& src, std::uint64_t off) {
    std::uint8_t b[8];
    REQUIRE(src.read(off, 8, b) == 8);
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

} // namespace

TEST_CASE("real QX (contract 1): collections against the python reference numbers") {
    RealContract c = openReal(1);
    if (!c.ok) {
        MESSAGE("skipped: QSTATE_TEST_STATE_DIR / QSTATE_SOURCE_DIR not set");
        return;
    }
    REQUIRE(c.src->size() == c.lc.stateSize);
    CHECK(c.lc.stateSize == 621806120);
    const auto& sch = *c.lc.schema;
    StateDecoder& dec = *c.dec;

    auto t0 = std::chrono::steady_clock::now();
    auto root = dec.node("");
    CHECK(root.childCount == 22);
    MESSAGE("QX root node: " << msSince(t0) << " ms");

    // ---- _assetOrders
    t0 = std::chrono::steady_clock::now();
    auto ao = dec.node("f:_assetOrders");
    MESSAGE("QX _assetOrders node (flag scan of 2^21 PoV slots + PoV table): " << msSince(t0) << " ms");
    REQUIRE(ao.container);
    CHECK(ao.kind == NodeKind::Collection);
    CHECK(ao.container->capacity == 2097152);
    CHECK(*ao.container->population == 3382);
    CHECK(*ao.container->povs == 92);
    CHECK(*ao.container->removed == 6);
    CHECK(ao.container->warning.empty());
    CHECK(ao.offset == 40);
    // independent count straight from the flag array
    const std::uint64_t aoFlags = 40 + fieldOff(sch, sch.types[c.lc.root].fields[6].type, "_povOccupationFlags");
    const FlagCount fc = countFlags(*c.src, aoFlags, 2097152);
    CHECK(fc.live == 92);
    CHECK(fc.tomb == 6);
    CHECK(fc.invalid == 0);
    CHECK(u64At(*c.src, 40 + 302514176 - 0) == 3382); // _population

    // element 0: priority 10, pov slot 1, value.entity / numberOfShares 1
    auto e0 = dec.children("f:_assetOrders/e:0").items;
    REQUIRE(e0.size() == 3);
    CHECK(json(e0[1])["value"]["v"] == "10");
    auto e0value = dec.children("f:_assetOrders/e:0/f:value").items;
    REQUIRE(e0value.size() == 2);
    CHECK(e0value[0].label == "entity");
    CHECK(json(e0value[1])["value"]["v"] == "1");
    auto raw0 = dec.children("f:_assetOrders/e:0", {ChildView::Raw, 0, 10, false}).items;
    REQUIRE(raw0.size() == 6);
    CHECK(json(raw0[2])["value"]["v"] == "1"); // povIndex
    if (kRealIdentities) {
        const std::string id0 = json(e0value[0])["value"]["identity"];
        CHECK(id0.rfind("NPGCDSLR", 0) == 0);
        CHECK(id0.substr(id0.size() - 4) == "NZSJ");
    }
    // the element's PoV is slot 1: issuer = QX contract id, asset 'QWALLET'
    auto pov1 = json(dec.node("f:_assetOrders/p:1"))["value"];
    CHECK(pov1["hex"].get<std::string>().substr(0, 64 - 16) == "01" + std::string(46, '0'));

    // PoV table: slot 0 = (issuer NULL, 'QUTIL'): population 38, head 3380, tail 3237, bst root 77
    auto povTable = dec.describeTable("f:_assetOrders", "povs");
    CHECK(povTable.totalRows == 92);
    TableRequest pr;
    pr.id = "f:_assetOrders";
    pr.view = "povs";
    pr.limit = 200;
    auto povRows = dec.tableRows(pr);
    REQUIRE(povRows.rows.size() == 92);
    const auto& r0 = povRows.rows[0];
    CHECK(r0.index == 0);
    CHECK(r0.cells[2].v == "38");
    CHECK(r0.cells[3].v == "3380");
    CHECK(r0.cells[4].v == "3237");
    CHECK(r0.cells[5].v == "77");
    // queue of PoV 0: elem 3380 prio 25000001, 3095 prio 25000000, 365 prio 21100000 (bids, descending price)
    auto queue = dec.children("f:_assetOrders/p:0", {ChildView::Logical, 0, 100, false});
    CHECK(queue.total == 38);
    REQUIRE(queue.items.size() == 38);
    CHECK(queue.items[0].id == "f:_assetOrders/e:3380");
    CHECK(queue.items[1].id == "f:_assetOrders/e:3095");
    CHECK(queue.items[2].id == "f:_assetOrders/e:365");
    auto prio = [&](const std::string& eid) {
        return std::stoll(json(dec.node(eid + "/f:priority"))["value"]["v"].get<std::string>());
    };
    CHECK(prio(queue.items[0].id) == 25000001);
    CHECK(prio(queue.items[1].id) == 25000000);
    CHECK(prio(queue.items[2].id) == 21100000);
    CHECK(queue.items.back().id == "f:_assetOrders/e:3237");

    // every PoV queue: right length, non-increasing priorities; the union of all queues is every element exactly once
    std::vector<char> seen(3382, 0);
    std::uint64_t total = 0;
    for (const auto& pv : dec.children("f:_assetOrders", {ChildView::Logical, 0, 1000, false}).items) {
        auto q = dec.children(pv.id, {ChildView::Logical, 0, 1000, false});
        CHECK(q.items.size() == pv.childCount);
        std::int64_t last = INT64_MAX;
        for (auto& it : q.items) {
            const std::int64_t p = prio(it.id);
            CHECK(p <= last);
            last = p;
            const std::uint64_t idx = std::stoull(it.id.substr(it.id.rfind(':') + 1));
            REQUIRE(idx < 3382);
            CHECK(seen[idx] == 0);
            seen[idx] = 1;
            ++total;
        }
    }
    CHECK(total == 3382);

    // element table: sort the 3382 elements by priority (desc) and by owner id
    TableRequest tr;
    tr.id = "f:_assetOrders";
    tr.limit = 5;
    tr.sort = {{"$priority", true}};
    t0 = std::chrono::steady_clock::now();
    auto sorted = dec.tableRows(tr);
    MESSAGE("QX _assetOrders table sorted by priority (3382 rows): " << msSince(t0) << " ms");
    CHECK(sorted.total == 3382);
    CHECK(sorted.rows[0].cells[2].v == "7500000000"); // max priority, verified with python on the file
    CHECK(sorted.rows[1].cells[2].v == "6250000000");
    CHECK(sorted.rows[2].cells[2].v == "6000000001");
    tr.sort = {{"value.entity", false}};
    tr.filters = {{"$priority", "gt", "100000"}};
    auto f2 = dec.tableRows(tr);
    CHECK(f2.total > 0);
    CHECK(f2.total < 3382);

    // ---- _entityOrders (second collection)
    auto eo = dec.node("f:_entityOrders");
    CHECK(*eo.container->population == 3382);
    CHECK(*eo.container->povs == 790);
    CHECK(*eo.container->removed == 359);
    CHECK(eo.container->warning.empty());
    const std::uint64_t eoOff = eo.offset;
    CHECK(eoOff == 302514232);
    CHECK(u64At(*c.src, eoOff + fieldOff(sch, sch.types[c.lc.root].fields[7].type, "_markRemovalCounter")) == 149034);
    // the collection's own element-array scan with hideEmpty over the raw layout
    t0 = std::chrono::steady_clock::now();
    auto rawEls = dec.children("f:_entityOrders/f:_elements", {ChildView::Logical, 0, 3, true});
    MESSAGE("QX _entityOrders raw _elements hideEmpty scan (2^21 x 88 B = 176 MB): " << msSince(t0) << " ms");
    CHECK(rawEls.total == 3382);

    // locate round trip on the real file
    auto loc = dec.locate(40 + 134742016 + 5 * 80 + 1); // inside _assetOrders._elements[5]
    CHECK(loc.id.rfind("f:_assetOrders/e:5", 0) == 0);
    CHECK(dec.node(loc.id).offset == loc.offset);

    // search: the QX contract id (u64 1) as 8-byte aligned integer finds nodes
    t0 = std::chrono::steady_clock::now();
    auto sr = dec.search({"22609", "int", 5});   // 'QX' as assetName value
    MESSAGE("QX search for an 8 byte integer over 621 MB: " << msSince(t0) << " ms, " << sr.matches.size() << " matches");
    CHECK(sr.mode == "int");
}

TEST_CASE("real QBOND (contract 17): hash maps and sets against the python reference") {
    RealContract c = openReal(17);
    if (!c.ok) {
        MESSAGE("skipped");
        return;
    }
    StateDecoder& dec = *c.dec;
    auto m = dec.node("f:_epochMbondInfoMap");
    REQUIRE(m.container);
    CHECK(*m.container->population == 47);
    CHECK(m.container->warning.empty());
    // slot 5: key 207 value {name 'MBND16', stakersAmount 5, totalStaked 11261}
    auto e5 = dec.children("f:_epochMbondInfoMap/e:5").items;
    REQUIRE(e5.size() == 2);
    CHECK(json(e5[0])["value"]["v"] == "207");
    auto v5 = dec.children("f:_epochMbondInfoMap/e:5/f:value").items;
    REQUIRE(v5.size() >= 3);
    CHECK(json(v5[0])["value"]["text"] == "MBND16");
    CHECK(json(v5[1])["value"]["v"] == "5");
    CHECK(json(v5[2])["value"]["v"] == "11261");
    // table: columns key / value.name etc., sort by key
    auto ti = dec.describeTable("f:_epochMbondInfoMap");
    CHECK(ti.columns[1].id == "key");
    CHECK(ti.columns[2].id == "value.name");
    TableRequest tr;
    tr.id = "f:_epochMbondInfoMap";
    tr.limit = 100;
    tr.sort = {{"key", false}};
    auto sorted = dec.tableRows(tr);
    CHECK(sorted.total == 47);
    std::uint64_t last = 0;
    for (auto& r : sorted.rows) {
        const std::uint64_t k = std::stoull(r.cells[1].v);
        CHECK(k >= last);
        last = k;
    }
    // names filter via asset-name text
    tr.sort.clear();
    tr.filters = {{"value.name", "eq", "MBND16"}};
    CHECK(dec.tableRows(tr).total == 1);

    auto big = dec.node("f:_userTotalStakedMap");
    CHECK(*big.container->population == 175);
    CHECK(big.container->capacity == 524288);
    CHECK(big.container->warning.empty());
    auto cf = dec.node("f:_commissionFreeAddresses");
    CHECK(*cf.container->population == 1);
    CHECK(*cf.container->removed == 4);
    CHECK(cf.container->warning.empty());
    auto ask = dec.node("f:_askOrders");
    CHECK(*ask.container->population == 6);
    CHECK(ask.container->warning.empty());
}

TEST_CASE("real QUOTTERY (contract 2): map of structs with DateAndTime") {
    RealContract c = openReal(2);
    if (!c.ok) {
        MESSAGE("skipped");
        return;
    }
    StateDecoder& dec = *c.dec;
    auto m = dec.node("f:mEventInfo");
    CHECK(*m.container->population == 114);
    CHECK(m.container->warning.empty());
    auto e2 = dec.children("f:mEventInfo/e:2").items;
    CHECK(json(e2[0])["value"]["v"] == "406");
    auto fields = dec.children("f:mEventInfo/e:2/f:value").items;
    bool sawOpen = false, sawEnd = false;
    for (auto& f : fields) {
        if (f.label == "openDate") {
            CHECK(json(f)["value"]["text"] == "2026-08-19 18:15:31.000'000");
            sawOpen = true;
        }
        if (f.label == "endDate") {
            CHECK(json(f)["value"]["text"] == "2026-10-17 14:00:00.000'000");
            sawEnd = true;
        }
    }
    CHECK(sawOpen);
    CHECK(sawEnd);
    auto pos = dec.node("f:mPositionInfo");
    CHECK(*pos.container->population == 39);
    CHECK(pos.container->capacity == 8388608);
    auto ab = dec.node("f:mABOrders");
    CHECK(*ab.container->population == 121);
    CHECK(*ab.container->povs == 28);
    CHECK(*ab.container->removed == 46);
    CHECK(ab.container->warning.empty());
    // timing of the big raw scan: 606 MB mPositionInfo element array
    auto t0 = std::chrono::steady_clock::now();
    auto ne = dec.children("f:mPositionInfo/f:_elements", {ChildView::Logical, 0, 10, true});
    MESSAGE("QUOTTERY mPositionInfo raw _elements hideEmpty scan (8M slots, 606 MB): " << msSince(t0) << " ms");
    CHECK(ne.total >= 39);
}

TEST_CASE("real ESCROW (27) and GGWP (28): tombstones, FIFO queues, counters") {
    RealContract c = openReal(27);
    if (!c.ok) {
        MESSAGE("skipped");
        return;
    }
    {
        StateDecoder& dec = *c.dec;
        auto deals = dec.node("f:_deals");
        CHECK(*deals.container->population == 5);
        CHECK(*deals.container->removed == 35);
        CHECK(deals.container->warning.empty());
        auto earned = dec.node("f:_earnedTokens");
        CHECK(*earned.container->population == 6);
        CHECK(*earned.container->removed == 5);
        // the ownerDealIndexes collection: PoV queues with priority 0 are FIFO
        auto odi = dec.node("f:_ownerDealIndexes");
        CHECK(odi.container->warning.empty());
        bool foundFifo = false;
        for (auto& pv : dec.children("f:_ownerDealIndexes").items) {
            auto q = dec.children(pv.id).items;
            if (q.size() == 3) {
                std::vector<std::string> v;
                for (auto& e : q) v.push_back(json(dec.node(e.id + "/f:value"))["value"]["v"]);
                if (v == std::vector<std::string>{"33", "34", "36"}) foundFifo = true;
            }
        }
        CHECK(foundFifo);
    }
    RealContract g = openReal(28);
    REQUIRE(g.ok);
    auto hb = g.dec->node("f:holderBalances");
    CHECK(*hb.container->population == 264);
    CHECK(hb.container->warning.empty());
    auto sb = g.dec->node("f:stakedBalances");
    CHECK(*sb.container->population == 61);
}

TEST_CASE("real files: every container of every contract decodes without warnings; timings") {
    if (qstate::testing::stateDir().empty() || qstate::testing::sourceDir().empty()) {
        MESSAGE("skipped");
        return;
    }
    auto cache = std::make_shared<DecodeCache>(256u << 20);
    double totalMs = 0;
    int containers = 0;
    for (int idx = 1; idx <= 28; ++idx) {
        RealContract c = openReal(idx, cache);
        if (!c.ok) {
            MESSAGE("contract " << idx << ": not available");
            continue;
        }
        StateDecoder& dec = *c.dec;
        const std::uint64_t fileSize = dec.fileSize();
        // QRP's file is bigger than its StateData: tolerated
        CHECK(fileSize >= dec.stateSize());
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::string> containerIds;
        std::function<void(const std::string&, int)> visit = [&](const std::string& id, int depth) {
            NodeInfo n = dec.node(id);
            switch (n.kind) {
            case NodeKind::HashMap: case NodeKind::HashSet: case NodeKind::Collection: case NodeKind::LinkedList:
                containerIds.push_back(id);
                return;
            case NodeKind::Struct: case NodeKind::Union: case NodeKind::Array:
                if (depth > 6 || n.childCount == 0) return;
                for (auto& ch : dec.children(id, {ChildView::Logical, 0, n.kind == NodeKind::Array ? 2u : 200u, false}).items)
                    visit(ch.id, depth + 1);
                return;
            default: return;
            }
        };
        visit("", 0);
        for (const std::string& id : containerIds) {
            ++containers;
            NodeInfo n = dec.node(id);
            INFO("contract " << idx << " (" << c.lc.name << ") " << id << ": " << (n.container ? n.container->warning : ""));
            REQUIRE(n.container);
            CHECK(n.container->warning.empty());
            CHECK(n.tabular);
            // table machinery: describe + first page, sorted by the first sortable column
            TableInfo ti = dec.describeTable(id);
            TableRequest tr;
            tr.id = id;
            tr.limit = 50;
            for (auto& col : ti.columns)
                if (col.sortable && col.id[0] != '$') {
                    tr.sort = {{col.id, true}};
                    break;
                }
            TablePage tp = dec.tableRows(tr);
            CHECK(tp.total == ti.totalRows);
            if (n.kind == NodeKind::Collection) {
                std::uint64_t sum = 0;
                for (auto& pv : dec.children(id, {ChildView::Logical, 0, 1000, false}).items) sum += pv.childCount;
                if (*n.container->povs <= 1000) CHECK(sum == *n.container->population);
            } else {
                CHECK(n.childCount == *n.container->population);
            }
            (void)json(tp);
        }
        const double ms = msSince(t0);
        totalMs += ms;
        MESSAGE("contract " << idx << " " << c.lc.name << ": " << containerIds.size() << " containers decoded in " << ms << " ms");
    }
    MESSAGE(containers << " containers, " << totalMs << " ms total");
    CHECK(containers > 40);
}

TEST_CASE("real state: concurrent queries are safe") {
    RealContract c = openReal(17);
    if (!c.ok) {
        MESSAGE("skipped");
        return;
    }
    StateDecoder& dec = *c.dec;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t)
        threads.emplace_back([&, t] {
            try {
                for (int i = 0; i < 20; ++i) {
                    Query q;
                    q.generation = 1;
                    TableRequest tr;
                    tr.id = "f:_epochMbondInfoMap";
                    tr.limit = 10;
                    tr.sort = {{"key", (i + t) % 2 == 0}};
                    if (dec.tableRows(tr, q).total != 47) ++failures;
                    if (*dec.node("f:_userTotalStakedMap", q).container->population != 175) ++failures;
                    (void)dec.children("f:_userTotalStakedMap", {ChildView::Logical, 0, 20, false}, q);
                    (void)dec.locate(1000 + static_cast<std::uint64_t>(i), q);
                }
            } catch (...) {
                ++failures;
            }
        });
    for (auto& t : threads) t.join();
    CHECK(failures == 0);
}

TEST_CASE("real proposal voting slots (GQMPROP 6, CCF 8, QUTIL 4): decoded slot summaries") {
    int found = 0;
    for (int idx : {4, 6, 8}) {
        RealContract c = openReal(idx);
        if (!c.ok) {
            MESSAGE("skipped");
            return;
        }
        StateDecoder& dec = *c.dec;
        std::function<void(const std::string&, int)> visit = [&](const std::string& id, int depth) {
            NodeInfo n = dec.node(id);
            if (n.kind == NodeKind::Struct && n.typeName.rfind("ProposalWithAllVoteData<", 0) == 0) {
                ++found;
                MESSAGE("contract " << idx << " " << id << ": " << n.preview);
                CHECK(!n.preview.empty());
                CHECK((n.preview == "free slot" || n.preview.rfind("epoch ", 0) == 0));
                // base class members (url, epoch, type, tick, data) come first, then votes
                auto kids = dec.children(id).items;
                REQUIRE(kids.size() >= 6);
                CHECK(kids[1].label == "epoch");
                CHECK(kids.back().label == "votes");
                return;
            }
            if (depth > 4 || !(n.kind == NodeKind::Struct || n.kind == NodeKind::Array) || n.childCount == 0) return;
            for (auto& ch : dec.children(id, {ChildView::Logical, 0, n.kind == NodeKind::Array ? 3u : 100u, false}).items)
                visit(ch.id, depth + 1);
        };
        visit("", 0);
    }
    CHECK(found >= 3);
}
