// Integration tests on the real Qubic core and the epoch-229 state files. Skipped unless
//   QSTATE_TEST_CORE_DIR      core checkout (HEAD, epoch 233, a git clone with tags)
//   QSTATE_TEST_STATE_DIR     directory with the epoch-229 contractNNNN.229 files
// are set; QSTATE_TEST_CORE_DIR_229 (plain snapshot of v1.303.2) enables the snapshot tests and
// QSTATE_SOURCE_DIR the digest comparison with docs/research/k12-spike/digest_229.txt.
#include "fixtures.h"
#include "test_env.h"

#include <doctest/doctest.h>

#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct RealEnv {
    std::string core, core229, state, source;
    bool haveHead() const { return !core.empty() && !state.empty(); }
    bool haveSnapshot() const { return !core229.empty() && !state.empty(); }
};

RealEnv env() {
    RealEnv e;
    e.core = qstate::testing::coreDir();
    e.state = qstate::testing::stateDir();
    e.core229 = qstate::testing::envOr("QSTATE_TEST_CORE_DIR_229");
    e.source = qstate::testing::sourceDir();
    return e;
}

std::string statusTable(const json& ws) {
    std::ostringstream out;
    for (const json& c : ws["contracts"]) {
        out << "  " << c["index"] << " " << c["name"].get<std::string>() << " " << c["status"].get<std::string>();
        if (c.contains("file")) out << " file=" << c["file"]["size"];
        if (c.contains("expectedSize")) out << " expected=" << c["expectedSize"];
        out << "\n";
    }
    return out.str();
}

std::set<unsigned> indicesWith(const json& ws, const std::string& status) {
    std::set<unsigned> s;
    for (const json& c : ws["contracts"]) {
        if (c["status"] == status) s.insert(c["index"].get<unsigned>());
    }
    return s;
}

json firstDiagnostic(const json& ws, const std::string& severity, const std::string& needle) {
    for (const json& d : ws["diagnostics"]) {
        if (d["severity"] == severity && d["message"].get<std::string>().find(needle) != std::string::npos) return d;
    }
    return json();
}

// Depth-first search for the first leaf satisfying `pred`, descending at most `depth` levels and looking at
// at most `fanout` children per node.
bool findLeaf(Harness& h, unsigned contract, const std::string& id, int depth, const std::function<bool(const json&)>& pred,
              json& out) {
    json page = h.ok("state.children", {{"contract", contract}, {"id", id}, {"limit", 40}});
    for (const json& item : page["items"]) {
        if (item.contains("value") && pred(item)) {
            out = item;
            return true;
        }
    }
    if (depth <= 0) return false;
    for (const json& item : page["items"]) {
        if (item["childCount"].get<std::uint64_t>() > 0 && item["size"].get<std::uint64_t>() < (1u << 20) &&
            findLeaf(h, contract, item["id"], depth - 1, pred, out)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("real: epoch-229 files with core HEAD and coreRef auto pick v1.303.2") {
    RealEnv e = env();
    if (!e.haveHead()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_DIR / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    Harness h;
    const auto t0 = Clock::now();
    json ws = h.ok("workspace.open", {{"coreDir", e.core}, {"stateDir", e.state}, {"coreRef", "auto"}});
    std::cerr << "[service] open (auto, cold export cache): " << msSince(t0) << " ms, parse " << ws["core"]["parseMs"] << " ms\n";
    CHECK(ws["core"]["ref"] == "v1.303.2");
    CHECK(ws["core"]["epoch"] == 229);
    CHECK(ws["core"]["version"] == "1.303.2");
    CHECK(ws["core"]["sha"].get<std::string>().size() == 40);
    CHECK(ws["core"]["sourceDir"].get<std::string>().find(h.scratch.str()) == 0);
    CHECK(ws["state"]["epoch"] == 229);
    REQUIRE(ws["contracts"].size() == 29);
    INFO(statusTable(ws));
    CHECK(indicesWith(ws, "ok").size() == 29);
    CHECK(firstDiagnostic(ws, "error", "").is_null());
    CHECK(firstDiagnostic(ws, "warning", "epoch").is_null());

    // second open: the export is cached
    const auto t1 = Clock::now();
    json again = h.ok("workspace.open", {{"coreDir", e.core}, {"stateDir", e.state}, {"coreRef", "auto"}});
    std::cerr << "[service] open (auto, warm export cache): " << msSince(t1) << " ms\n";
    CHECK(again["core"]["sourceDir"] == ws["core"]["sourceDir"]);

    // explicit tag: same result
    json tag = h.ok("workspace.open", {{"coreDir", e.core}, {"stateDir", e.state}, {"coreRef", "v1.303.2"}});
    CHECK(tag["core"]["ref"] == "v1.303.2");
    CHECK(indicesWith(tag, "ok").size() == 29);
    CHECK(h.errorCode("workspace.open", {{"coreDir", e.core}, {"stateDir", e.state}, {"coreRef", "no-such-tag"}}) == "invalid_params");

    // the exported tree is a cache copy: no watcher on it (editing events cannot happen), state files are watched
    h.ok("workspace.close");
}

TEST_CASE("real: epoch-229 files with the HEAD working tree: only NOST changed its layout") {
    RealEnv e = env();
    if (!e.haveHead()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_DIR / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    Harness h;
    json ws = h.ok("workspace.open", {{"coreDir", e.core}, {"stateDir", e.state}});
    CHECK(ws["core"]["ref"] == "");
    CHECK(ws["core"]["epoch"].get<int>() >= 233);
    REQUIRE(ws["contracts"].size() >= 31);
    INFO(statusTable(ws));
    const auto mismatch = indicesWith(ws, "size-mismatch");
    std::cerr << "[service] HEAD working tree vs epoch-229 files, size-mismatch:";
    for (unsigned i : mismatch) std::cerr << " " << i << "(" << contractOf(ws, i)["name"].get<std::string>() << ")";
    std::cerr << "; ok: " << indicesWith(ws, "ok").size() << ", missing-file: " << indicesWith(ws, "missing-file").size() << "\n";
    CHECK(mismatch == std::set<unsigned>{14});
    CHECK(contractOf(ws, 14)["name"] == "NOST");
    const std::string msg = contractOf(ws, 14)["statusMessage"];
    CHECK(msg.find("epoch 229") != std::string::npos);
    CHECK(msg.find("1030098088") != std::string::npos);
    CHECK_FALSE(firstDiagnostic(ws, "warning", "epoch 229").is_null());
    CHECK(indicesWith(ws, "missing-file") == std::set<unsigned>{29, 30});
    CHECK(indicesWith(ws, "ok").size() == 28);
    // the mismatching contract can still be browsed
    json node = h.ok("state.node", {{"contract", 14}, {"id", ""}});
    CHECK(node["inFile"] == true);
    h.ok("workspace.close");
}

TEST_CASE("real: plain snapshot directory of v1.303.2 (not a git repository)") {
    RealEnv e = env();
    if (!e.haveSnapshot()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_DIR_229 / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    Harness h;
    const auto t0 = Clock::now();
    json ws = h.ok("workspace.open", {{"coreDir", e.core229}, {"stateDir", e.state}});
    std::cerr << "[service] open (snapshot dir, no git): " << msSince(t0) << " ms, parse " << ws["core"]["parseMs"] << " ms\n";
    REQUIRE(ws["contracts"].size() == 29);
    INFO(statusTable(ws));
    CHECK(indicesWith(ws, "ok").size() == 29);
    CHECK(ws["core"]["version"] == "1.303.2");
    CHECK(ws["core"]["epoch"] == 229);
    CHECK(ws["core"]["ref"] == "");
    // auto on a non-git directory: working tree + warning
    json autoWs = h.ok("workspace.open", {{"coreDir", e.core229}, {"stateDir", e.state}, {"coreRef", "auto"}});
    CHECK(indicesWith(autoWs, "ok").size() == 29);
    CHECK_FALSE(firstDiagnostic(autoWs, "warning", "auto").is_null());
    h.ok("workspace.close");
}

TEST_CASE("real: pipeline timing, browsing, tables, search") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    Harness h;
    // Cold schema from a snapshot directory (no git involved) + the first node views of several contracts.
    const std::string coreDir = e.haveSnapshot() ? e.core229 : e.core;
    const std::string ref = e.haveSnapshot() ? "" : "auto";
    const auto t0 = Clock::now();
    json req = {{"coreDir", coreDir}, {"stateDir", e.state}};
    if (!ref.empty()) req["coreRef"] = ref;
    json ws = h.ok("workspace.open", req);
    const double openMs = msSince(t0);
    const auto t1 = Clock::now();
    for (unsigned c : {1u, 2u, 4u, 9u, 14u, 26u}) {
        json root = h.ok("state.node", {{"contract", c}, {"id", ""}});
        json kids = h.ok("state.children", {{"contract", c}, {"id", ""}});
        CHECK(kids["total"].get<int>() > 0);
        CHECK(root["inFile"] == true);
        for (const json& item : kids["items"]) {
            if (item["tabular"] == true) {
                json info = h.ok("table.describe", {{"contract", c}, {"id", item["id"]}});
                json rows = h.ok("table.rows", {{"contract", c}, {"id", item["id"]}, {"offset", 0}, {"limit", 20}});
                CHECK(rows["rows"].size() <= 20);
                CHECK(rows["total"].get<std::uint64_t>() <= info["totalRows"].get<std::uint64_t>());
                break;
            }
        }
    }
    const double viewsMs = msSince(t1);
    std::cerr << "[service] pipeline (cold schema from " << (ref.empty() ? "snapshot dir" : "git tag cache") << " + first views of 6 contracts): open "
              << openMs << " ms, views " << viewsMs << " ms, total " << openMs + viewsMs << " ms\n";
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(openMs + viewsMs < 1500); // the target of the task; sanitizer builds are several times slower
#endif

    // QX (contract 1): collections are browsable as tables; search finds an identity found in the tree
    json qxRoot = h.ok("state.children", {{"contract", 1}, {"id", ""}});
    bool sawTable = false;
    for (const json& item : qxRoot["items"]) {
        if (item["tabular"] != true) continue;
        sawTable = true;
        json info = h.ok("table.describe", {{"contract", 1}, {"id", item["id"]}});
        CHECK(info["columns"].size() > 1);
        json rows = h.ok("table.rows", {{"contract", 1}, {"id", item["id"]}, {"offset", 0}, {"limit", 50}});
        CHECK(rows["total"].get<std::uint64_t>() >= rows["rows"].size());
        for (const json& row : rows["rows"]) CHECK(row["cells"].size() == info["columns"].size());
    }
    CHECK(sawTable);

    // reveal of a deep collection element: PoV and queue position are exact (checked against the listing)
    {
        json povs = h.ok("state.children", {{"contract", 1}, {"id", "f:_assetOrders"}, {"limit", 5}});
        REQUIRE(povs["items"].size() == 5);
        json els = h.ok("state.children", {{"contract", 1}, {"id", povs["items"][3]["id"]}, {"limit", 3}});
        REQUIRE(!els["items"].empty());
        json rv = h.ok("state.reveal", {{"contract", 1}, {"id", els["items"][0]["id"]}});
        REQUIRE(rv["path"].size() == 4);
        CHECK(rv["path"][2]["id"] == povs["items"][3]["id"]);
        CHECK(rv["path"][2]["index"] == 3);
        CHECK(rv["path"][3]["index"] == 0);
        CHECK(rv["path"][2]["childTotal"] == els["total"]);
    }

    // an id leaf somewhere in a small contract
    json leaf;
    bool found = false;
    for (unsigned c : {15u, 16u, 22u, 23u, 24u, 25u, 3u, 6u}) {
        if (findLeaf(h, c, "", 3, [](const json& item) { return item["value"]["k"] == "id" && item["value"]["zero"] == false; }, leaf)) {
            json res = h.ok("state.search", {{"contract", c}, {"query", leaf["value"]["identity"]}, {"limit", 50}});
            CHECK(res["pattern"]["mode"] == "id");
            bool hit = false;
            for (const json& m : res["matches"]) hit = hit || m["offset"] == leaf["offset"];
            CHECK(hit);
            // locate the match: the deepest node starts at the match
            json loc = h.ok("state.locate", {{"contract", c}, {"offset", leaf["offset"]}});
            CHECK(loc["offset"] == leaf["offset"]);
            json rv = h.ok("state.reveal", {{"contract", c}, {"id", loc["id"]}});
            CHECK(rv["id"] == loc["id"]);
            // and the digest of this contract is a 32 byte hex string
            json d = h.ok("state.digest", {{"contract", c}});
            CHECK(d["k12"].get<std::string>().size() == 64);
            found = true;
            break;
        }
    }
    CHECK(found);
    h.ok("workspace.close");
}

TEST_CASE("real: state.digest of small files equals the recorded K12 digests") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    if (e.source.empty()) {
        MESSAGE("skipped: QSTATE_SOURCE_DIR not set");
        return;
    }
    std::ifstream in(e.source + "/docs/research/k12-spike/digest_229.txt");
    if (!in) {
        MESSAGE("skipped: digest_229.txt not found");
        return;
    }
    std::map<unsigned, std::pair<std::uint64_t, std::string>> recorded;
    std::string line;
    std::regex re(R"(^contract(\d{4})\.229\s+(\d+) bytes\s+([0-9a-f]{64}))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) recorded[std::stoul(m[1])] = {std::stoull(m[2]), m[3]};
    }
    REQUIRE(recorded.size() == 29);
    Harness h;
    json req = {{"coreDir", e.haveSnapshot() ? e.core229 : e.core}, {"stateDir", e.state}};
    if (!e.haveSnapshot()) req["coreRef"] = "auto";
    h.ok("workspace.open", req);
    int checked = 0;
    for (const auto& [index, info] : recorded) {
        if (info.first > (64u << 20)) continue; // big files are covered by the CLI smoke test
        json d = h.ok("state.digest", {{"contract", index}});
        CHECK_MESSAGE(d["k12"] == info.second, "contract " << index);
        ++checked;
    }
    CHECK(checked >= 15);
}

TEST_CASE("real: concurrent reads while the real workspace is reloaded") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    Harness h;
    json req = {{"coreDir", e.haveSnapshot() ? e.core229 : e.core}, {"stateDir", e.state}};
    if (!e.haveSnapshot()) req["coreRef"] = "auto";
    h.ok("workspace.open", req);
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<long> calls{0};
    auto reader = [&](int seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        const unsigned contracts[] = {1, 3, 6, 15, 16, 24};
        while (!stop.load()) {
            const unsigned c = contracts[rng() % 6];
            json r;
            switch (rng() % 4) {
            case 0: r = h.call("state.node", {{"contract", c}, {"id", ""}}); break;
            case 1: r = h.call("state.children", {{"contract", c}, {"id", ""}, {"limit", 50}}); break;
            case 2: r = h.call("state.bytes", {{"contract", c}, {"offset", 0}, {"length", 4096}}); break;
            default: r = h.call("state.locate", {{"contract", c}, {"offset", 8}}); break;
            }
            ++calls;
            if (r.contains("error")) ++failures;
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) threads.emplace_back(reader, i);
    for (int i = 0; i < 4; ++i) h.ok("workspace.reload");
    stop = true;
    for (auto& t : threads) t.join();
    CHECK(failures.load() == 0);
    CHECK(calls.load() > 20);
}

TEST_CASE("real: a newer open cancels the extraction that is still running") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    Harness h;

    TempDir core, state;
    writeFakeCore(core.path());
    writeFakeState(state.path());
    json big = {{"coreDir", e.haveSnapshot() ? e.core229 : e.core}, {"stateDir", e.state}};
    if (!e.haveSnapshot()) big["coreRef"] = "auto";
    json bigResult;
    std::thread t([&] { bigResult = h.call("workspace.open", big); });
    std::this_thread::sleep_for(milliseconds(25));
    json small = h.call("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}});
    t.join();
    REQUIRE(small.contains("result"));
    // the big one was either cancelled, or (if it happened to finish first) superseded by the small one
    if (bigResult.contains("error")) CHECK(bigResult["error"]["message"] == "cancelled");
    json cur = h.ok("workspace.get");
    CHECK(cur["request"]["coreDir"] == core.str());
    CHECK(cur["id"] == small["result"]["id"]);
}

TEST_CASE("real: core.versions lists the newest tags with version and epoch") {
    RealEnv e = env();
    if (e.core.empty()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_DIR not set");
        return;
    }
    Harness h;
    const auto t0 = Clock::now();
    json r = h.ok("core.versions", {{"coreDir", e.core}});
    std::cerr << "[service] core.versions (30 tags, cold): " << msSince(t0) << " ms\n";
    CHECK(r["worktree"]["kind"] == "worktree");
    CHECK(r["worktree"]["version"].is_string());
    CHECK(r["worktree"]["epoch"].get<int>() >= 233);
    CHECK(r["worktree"]["sha"].get<std::string>().size() == 40);
    REQUIRE(r["refs"].size() == 30);
    bool found = false;
    std::string previousDate;
    for (const json& ref : r["refs"]) {
        CHECK(ref["kind"] == "tag");
        CHECK(ref["version"].is_string());
        CHECK(ref["epoch"].is_number());
        CHECK(ref["sha"].get<std::string>().size() == 40);
        const std::string date = ref["date"];
        if (!previousDate.empty()) CHECK(date <= previousDate); // newest first
        previousDate = date;
        if (ref["ref"] == "v1.303.2") {
            found = true;
            CHECK(ref["version"] == "1.303.2");
            CHECK(ref["epoch"] == 229);
        }
    }
    CHECK(found);
    const auto t1 = Clock::now();
    json small = h.ok("core.versions", {{"coreDir", e.core}, {"limit", 5}});
    CHECK(small["refs"].size() == 5);
    CHECK(small["refs"][0] == r["refs"][0]);
    std::cerr << "[service] core.versions (5 tags, warm memo): " << msSince(t1) << " ms\n";
}

TEST_CASE("real: watching small copies of real state files") {
    RealEnv e = env();
    if (!e.haveSnapshot()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_DIR_229 / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    TempDir state;
    for (const char* name : {"contract0000.229", "contract0015.229", "contract0016.229"}) {
        fs::copy_file(fs::path(e.state) / name, state / name);
    }
    Harness h;
    EventCollector events(h.bus);
    json ws = h.ok("workspace.open", {{"coreDir", e.core229}, {"stateDir", state.str()}});
    CHECK(contractOf(ws, 15)["status"] == "ok");
    CHECK(contractOf(ws, 1)["status"] == "missing-file");
    const int gen = contractOf(ws, 15)["generation"];
    appendBytes(state / "contract0015.229", std::vector<std::uint8_t>(5, 1));
    json ev = events.waitFor("contracts.changed");
    REQUIRE_FALSE(ev.is_null());
    CHECK(ev["contracts"][0]["index"] == 15);
    CHECK(ev["contracts"][0]["status"] == "size-mismatch");
    CHECK(ev["contracts"][0]["generation"].get<int>() > gen);
    // restore the original size: ok again
    const std::size_t seen = events.size();
    fs::resize_file(state / "contract0015.229", 32864);
    json ev2 = events.waitFor("contracts.changed", milliseconds(8000), {}, seen);
    REQUIRE_FALSE(ev2.is_null());
    CHECK(ev2["contracts"][0]["status"] == "ok");
    CHECK(events.count("workspace.updated") == 0);
}

TEST_CASE("real: a search and a digest of a large file can be cancelled") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    Harness h;
    json req = {{"coreDir", e.haveSnapshot() ? e.core229 : e.core}, {"stateDir", e.state}};
    if (!e.haveSnapshot()) req["coreRef"] = "auto";
    h.ok("workspace.open", req);
    for (const char* method : {"state.search", "state.digest"}) {
        std::promise<json> done;
        json params = {{"contract", 14}, {"query", "0xdeadbeefcafebabe0123456789abcdef"}};
        const auto t0 = Clock::now();
        auto token = h.dispatcher.dispatchAsync(method, params, [&](json r) { done.set_value(std::move(r)); }, "http");
        std::this_thread::sleep_for(milliseconds(15));
        token->cancel();
        json r = done.get_future().get();
        const double ms = msSince(t0);
        std::cerr << "[service] " << method << " on a 1 GB file, cancelled after 15 ms: returned after " << ms << " ms ("
                  << (r.contains("error") ? r["error"]["message"].get<std::string>() : std::string("finished")) << ")\n";
        if (r.contains("error")) CHECK(r["error"]["message"] == "cancelled");
        CHECK(ms < 3000);
    }
}
