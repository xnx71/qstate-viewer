// Opt-in stress tests (QSTATE_TEST_STRESS=1): big synthetic containers, ~600 MB of RAM.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <random>

#include "test_helpers.h"

using namespace qstate::decode;
using namespace qstate::decode::testing;

namespace {
double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

TEST_CASE("stress: Collection<AssetOrder,2^21> with 1.5M elements in 200 PoVs") {
    if (!std::getenv("QSTATE_TEST_STRESS")) {
        MESSAGE("skipped (set QSTATE_TEST_STRESS=1)");
        return;
    }
    SB sb;
    const std::uint64_t L = 1u << 21;
    const TypeId order = sb.record("AssetOrder", {{"entity", sb.id()}, {"numberOfShares", sb.s64()}});
    const TypeId coll = sb.collection(order, L);
    const TypeId root = sb.record("S", {{"c", coll}});
    CHECK(sb.size(coll) == 302514192);
    Img img(sb.size(root));
    CollW w(img, 0, L, 40);
    std::mt19937_64 rng(99);
    auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t N = 1500000;
    std::vector<std::vector<std::uint8_t>> povIds;
    for (int i = 0; i < 200; ++i) povIds.push_back(idBytes(static_cast<std::uint64_t>(i) * 7919 + 1, 5, 0, static_cast<std::uint64_t>(i)));
    for (std::uint64_t i = 0; i < N; ++i) {
        auto v = idBytes(i + 1);
        v.resize(40);
        for (int b = 0; b < 8; ++b) v[32 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>((i >> (8 * b)) & 0xFF);
        w.add(povIds[rng() % 200], v, static_cast<std::int64_t>(rng() % 1000000) - 500000);
    }
    MESSAGE("built image in " << msSince(t0) << " ms");
    DecoderConfig cfg;
    cfg.identity = testCodec();
    StateDecoder dec(sb.finish(), root, mem(img), cfg);

    t0 = std::chrono::steady_clock::now();
    auto n = dec.node("f:c");
    MESSAGE("collection node (flags + 200 PoVs): " << msSince(t0) << " ms");
    CHECK(*n.container->population == N);
    CHECK(*n.container->povs == 200);
    CHECK(n.container->warning.empty());

    const std::string pov = dec.children("f:c").items[0].id;
    t0 = std::chrono::steady_clock::now();
    auto q = dec.children(pov, {ChildView::Logical, 0, 10, false});
    MESSAGE("first page of a PoV queue (partial BST walk): " << msSince(t0) << " ms");
    t0 = std::chrono::steady_clock::now();
    q = dec.children(pov, {ChildView::Logical, 5000, 1000, false});
    MESSAGE("page 5000.. of the same queue (full walk, ~" << N / 200 << " elements): " << msSince(t0) << " ms");
    CHECK(q.total > 5000);
    t0 = std::chrono::steady_clock::now();
    q = dec.children(pov, {ChildView::Logical, 6000, 100, false});
    MESSAGE("same queue again (cached walk): " << msSince(t0) << " ms");
    std::int64_t last = INT64_MAX;
    auto page = dec.children(pov, {ChildView::Logical, 0, 1000, false});
    for (auto& e : page.items) {
        const std::int64_t p = std::stoll(dec.node(e.id + "/f:priority").value->v);
        CHECK(p <= last);
        last = p;
    }

    TableRequest tr;
    tr.id = "f:c";
    tr.limit = 10;
    t0 = std::chrono::steady_clock::now();
    auto plain = dec.tableRows(tr);
    MESSAGE("table first page, no sort (1.5M rows): " << msSince(t0) << " ms");
    CHECK(plain.total == N);
    tr.sort = {{"$priority", true}};
    t0 = std::chrono::steady_clock::now();
    auto sorted = dec.tableRows(tr);
    MESSAGE("table sorted by priority desc, first run (1.5M rows read + sorted): " << msSince(t0) << " ms");
    CHECK(sorted.total == N);
    CHECK(std::stoll(sorted.rows[0].cells[2].v) >= std::stoll(sorted.rows[9].cells[2].v));
    tr.offset = 1000000;
    t0 = std::chrono::steady_clock::now();
    sorted = dec.tableRows(tr);
    MESSAGE("page 1,000,000 of the sorted table (cached index): " << msSince(t0) << " ms");
    tr.offset = 0;
    tr.sort = {{"value.entity", false}};
    tr.filters = {{"$priority", "gt", "400000"}, {"value.numberOfShares", "lt", "1000000"}};
    t0 = std::chrono::steady_clock::now();
    auto filtered = dec.tableRows(tr);
    MESSAGE("filter on 2 columns + sort by id: " << msSince(t0) << " ms, " << filtered.total << " rows");
    CHECK(filtered.total > 0);
    tr.filters = {{"$pov", "eq", dec.node(pov).value->identity}};
    t0 = std::chrono::steady_clock::now();
    filtered = dec.tableRows(tr);
    MESSAGE("filter by PoV id: " << msSince(t0) << " ms, " << filtered.total << " rows");
    CHECK(filtered.total == dec.node(pov).childCount);
}
