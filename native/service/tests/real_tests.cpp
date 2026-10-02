// Integration tests on the real Qubic core and the epoch-229 state files. Skipped unless
//   QSTATE_TEST_CORE_REPO     a git clone of the core with its tags (used as the repoUrl: no network)
//   QSTATE_TEST_STATE_DIR     directory with the epoch-229 contractNNNN.229 files
// are set; QSTATE_TEST_CORE_DIR_229 (plain snapshot of v1.303.2, made into a git repository here) lets the tests that
// only need the matching sources run without a clone, and QSTATE_SOURCE_DIR enables the digest comparison with
// docs/research/data/digest_229.txt.
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
    e.core = qstate::testing::coreRepo();
    e.state = qstate::testing::stateDir();
    e.core229 = qstate::testing::coreDir229();
    e.source = qstate::testing::sourceDir();
    return e;
}

// The plain snapshot of v1.303.2 as a git repository with the tag "snapshot" (made once per process).
struct SnapshotRepo {
    TempDir dir;
    qstate::testing::GitFixtureRepo git;
    explicit SnapshotRepo(const std::string& snapshot) : git(dir.path() / "snapshot") {
        for (const auto& entry : fs::directory_iterator(snapshot)) {
            if (entry.path().filename() == ".git") continue;
            fs::copy(entry.path(), git.dir() / entry.path().filename(), fs::copy_options::recursive);
        }
        git.commit("v1.303.2");
        git.tag("snapshot");
    }
};

// Sources that match the epoch-229 files: the real core with ref "auto", else the snapshot repository.
struct RealSource {
    std::string url, ref;
};

RealSource snapshotSource(const RealEnv& e) {
    static const SnapshotRepo* snapshot = nullptr;
    if (snapshot == nullptr) snapshot = new SnapshotRepo(e.core229);
    return {snapshot->git.url(), "snapshot"};
}

RealSource realSource(const RealEnv& e) {
    if (!e.core.empty()) return {e.core, "auto"};
    return snapshotSource(e);
}

json request(const std::string& url, const std::string& ref, const std::string& statePath) {
    return {{"core", {{"repoUrl", url}, {"ref", ref}}}, {"statePath", statePath}};
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

TEST_CASE("real: core.sync of the real repository lists tags with version and epoch") {
    RealEnv e = env();
    if (e.core.empty()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_REPO not set");
        return;
    }
    Harness h;
    const auto t0 = Clock::now();
    json repo = h.ok("core.sync", {{"repoUrl", e.core}});
    const double coldMs = msSince(t0);
    const auto t1 = Clock::now();
    json again = h.ok("core.sync", {{"repoUrl", e.core}, {"offline", true}});
    const double warmMs = msSince(t1);
    std::cerr << "[service] core.sync of the local core clone: first (clone + " << repo["tags"].size() << " tags) " << coldMs
              << " ms, listing from the mirror " << warmMs << " ms\n";
    CHECK(again == repo);
    CHECK(warmMs < 1000);
    REQUIRE(repo["tags"].size() > 100);
    bool found = false;
    for (const json& tag : repo["tags"]) {
        CHECK(tag["kind"] == "tag");
        CHECK(tag["sha"].get<std::string>().size() == 40);
        CHECK(tag["date"].is_string());
        if (tag["ref"] == "v1.303.2") {
            found = true;
            CHECK(tag["version"] == "1.303.2");
            CHECK(tag["epoch"] == 229);
        }
    }
    CHECK(found);
    CHECK(repo["defaultBranch"].is_string());
    CHECK(repo["branches"][0]["ref"] == repo["defaultBranch"]);
    CHECK(repo["branches"][0]["epoch"].get<int>() >= 233);

    json commits = h.ok("core.commits", {{"repoUrl", e.core}, {"ref", "v1.303.2"}, {"limit", 10}});
    REQUIRE(commits["commits"].size() == 10);
    CHECK(commits["commits"][0]["epoch"] == 229);
    CHECK(commits["total"].get<int>() > 100);
    json searched = h.ok("core.commits", {{"repoUrl", e.core}, {"ref", repo["defaultBranch"]}, {"search", "fix"}, {"limit", 5}});
    CHECK(searched["commits"].size() == 5);
}

TEST_CASE("real: epoch-229 files with core ref auto pick v1.303.2") {
    RealEnv e = env();
    if (!e.haveHead()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_REPO / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    Harness h;
    const auto t0 = Clock::now();
    json ws = h.ok("workspace.open", request(e.core, "auto", e.state));
    std::cerr << "[service] open (auto, cold mirror and export): " << msSince(t0) << " ms, parse " << ws["core"]["parseMs"] << " ms\n";
    CHECK(ws["core"]["repoUrl"] == e.core);
    CHECK(ws["core"]["ref"] == "v1.303.2");
    CHECK(ws["core"]["kind"] == "tag");
    CHECK(ws["core"]["epoch"] == 229);
    CHECK(ws["core"]["version"] == "1.303.2");
    CHECK(ws["core"]["sha"].get<std::string>().size() == 40);
    CHECK(ws["state"]["epoch"] == 229);
    CHECK(ws["state"]["scope"] == "dir");
    REQUIRE(ws["contracts"].size() == 29);
    INFO(statusTable(ws));
    CHECK(indicesWith(ws, "ok").size() == 29);
    CHECK(firstDiagnostic(ws, "error", "").is_null());
    CHECK(firstDiagnostic(ws, "warning", "epoch").is_null());

    // second open: mirror and export are cached
    const auto t1 = Clock::now();
    json again = h.ok("workspace.open", request(e.core, "auto", e.state));
    std::cerr << "[service] open (auto, warm mirror and export): " << msSince(t1) << " ms\n";
    CHECK(again["core"]["sha"] == ws["core"]["sha"]);

    // explicit tag, and the commit sha: same result
    json tag = h.ok("workspace.open", request(e.core, "v1.303.2", e.state));
    CHECK(tag["core"]["ref"] == "v1.303.2");
    CHECK(indicesWith(tag, "ok").size() == 29);
    json sha = h.ok("workspace.open", request(e.core, ws["core"]["sha"], e.state));
    CHECK(sha["core"]["kind"] == "commit");
    CHECK(indicesWith(sha, "ok").size() == 29);
    CHECK(h.errorCode("workspace.open", request(e.core, "no-such-tag", e.state)) == "invalid_params");
    h.ok("workspace.close");
}

TEST_CASE("real: epoch-229 files with the head of the default branch: only NOST changed its layout") {
    RealEnv e = env();
    if (!e.haveHead()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_REPO / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    Harness h;
    const std::string branch = h.ok("core.sync", {{"repoUrl", e.core}})["defaultBranch"];
    json ws = h.ok("workspace.open", request(e.core, branch, e.state));
    CHECK(ws["core"]["ref"] == branch);
    CHECK(ws["core"]["kind"] == "branch");
    CHECK(ws["core"]["epoch"].get<int>() >= 233);
    REQUIRE(ws["contracts"].size() >= 31);
    INFO(statusTable(ws));
    const auto mismatch = indicesWith(ws, "size-mismatch");
    std::cerr << "[service] head of " << branch << " vs epoch-229 files, size-mismatch:";
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

TEST_CASE("real: a single real state file") {
    RealEnv e = env();
    if (!e.haveHead() && !e.haveSnapshot()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    const RealSource src = realSource(e);
    Harness h;
    const std::string file = e.state + "/contract0001.229";
    json ws = h.ok("workspace.open", request(src.url, src.ref, file));
    CHECK(ws["state"]["scope"] == "file");
    CHECK(ws["state"]["epoch"] == 229);
    REQUIRE(ws["contracts"].size() == 1);
    CHECK(ws["contracts"][0]["name"] == "QX");
    CHECK(ws["contracts"][0]["status"] == "ok");
    CHECK(ws["core"]["epoch"] == 229);
    CHECK(h.ok("state.children", {{"contract", 1}, {"id", ""}})["total"].get<int>() > 0);
    CHECK(h.errorCode("state.node", {{"contract", 2}, {"id", ""}}) == "not_found");
}

TEST_CASE("real: a plain snapshot of v1.303.2 as a repository") {
    RealEnv e = env();
    if (!e.haveSnapshot()) {
        MESSAGE("skipped: QSTATE_TEST_CORE_DIR_229 / QSTATE_TEST_STATE_DIR not set");
        return;
    }
    Harness h;
    const RealSource snap = snapshotSource(e);
    const auto t0 = Clock::now();
    json ws = h.ok("workspace.open", request(snap.url, snap.ref, e.state));
    std::cerr << "[service] open (snapshot repository): " << msSince(t0) << " ms, parse " << ws["core"]["parseMs"] << " ms\n";
    REQUIRE(ws["contracts"].size() == 29);
    INFO(statusTable(ws));
    CHECK(indicesWith(ws, "ok").size() == 29);
    CHECK(ws["core"]["version"] == "1.303.2");
    CHECK(ws["core"]["epoch"] == 229);
    CHECK(ws["core"]["ref"] == "snapshot");
    // auto finds no tag of epoch 229 ("snapshot" is not a release tag but its EPOCH does match: it is picked)
    json autoWs = h.ok("workspace.open", request(snap.url, "auto", e.state));
    CHECK(autoWs["core"]["ref"] == "snapshot");
    CHECK(indicesWith(autoWs, "ok").size() == 29);
    h.ok("workspace.close");
}

TEST_CASE("real: pipeline timing, browsing, tables, search") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    Harness h;
    const RealSource src = realSource(e);
    // Cold mirror + export + schema + the first node views of several contracts.
    const auto t0 = Clock::now();
    json ws = h.ok("workspace.open", request(src.url, src.ref, e.state));
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
    std::cerr << "[service] pipeline (cold mirror, export and schema + first views of 6 contracts): open " << openMs << " ms, views "
              << viewsMs << " ms, total " << openMs + viewsMs << " ms\n";
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(openMs + viewsMs < 2500); // sanitizer builds are several times slower
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
    std::ifstream in(e.source + "/docs/research/data/digest_229.txt");
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
    const RealSource src = realSource(e);
    h.ok("workspace.open", request(src.url, src.ref, e.state));
    int checked = 0;
    for (const auto& [index, info] : recorded) {
        if (info.first > (64u << 20)) continue; // the big files take seconds; the support tests cover them
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
    const RealSource src = realSource(e);
    h.ok("workspace.open", request(src.url, src.ref, e.state));
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
    Fixture small;
    const RealSource src = realSource(e);
    json big = request(src.url, src.ref, e.state);
    json bigResult;
    std::thread t([&] { bigResult = h.call("workspace.open", big); });
    std::this_thread::sleep_for(milliseconds(25));
    json smallResult = h.call("workspace.open", small.request());
    t.join();
    REQUIRE(smallResult.contains("result"));
    // the big one was either cancelled, or (if it happened to finish first) superseded by the small one
    if (bigResult.contains("error")) CHECK(bigResult["error"]["message"] == "cancelled");
    json cur = h.ok("workspace.get");
    CHECK(cur["request"] == small.request());
    CHECK(cur["id"] == smallResult["result"]["id"]);
}

TEST_CASE("real: watching small copies of real state files") {
    RealEnv e = env();
    if (!e.haveSnapshot() && !e.haveHead()) {
        MESSAGE("skipped: real data not configured");
        return;
    }
    TempDir state;
    for (const char* name : {"contract0000.229", "contract0015.229", "contract0016.229"}) {
        fs::copy_file(fs::path(e.state) / name, state / name);
    }
    const RealSource src = realSource(e);
    Harness h;
    EventCollector events(h.bus);
    json ws = h.ok("workspace.open", request(src.url, src.ref, state.str()));
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
    const RealSource src = realSource(e);
    h.ok("workspace.open", request(src.url, src.ref, e.state));
    for (const char* method : {"state.search", "state.digest"}) {
        std::promise<json> done;
        json params = {{"contract", 14}, {"query", "0xdeadbeefcafebabe0123456789abcdef"}};
        const auto t0 = Clock::now();
        auto token = h.dispatcher.dispatchAsync(method, params, [&](json r) { done.set_value(std::move(r)); });
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
