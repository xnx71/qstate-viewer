// Ledger files (spectrum.EEE / universe.EEE) through the service: Workspace.ledgers, routing of the state.* / table.*
// methods by "spectrum" / "universe", ledger.summary / ledger.lookup, single ledger file scope, fs.list markers, events.
#include "fixtures.h"
#include "test_env.h"

#include <doctest/doctest.h>

#include <iostream>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;
using std::chrono::milliseconds;

namespace {

std::vector<std::string> columnIds(const json& table) {
    std::vector<std::string> v;
    for (const json& c : table["columns"]) v.push_back(c["id"].get<std::string>());
    return v;
}

} // namespace

TEST_CASE("ledgers: a directory shows both ledgers with their status") {
    Harness h;
    Fixture f;
    SUBCASE("missing files") {
        json ws = h.ok("workspace.open", f.request());
        REQUIRE(ws["ledgers"].size() == 2);
        CHECK(ws["ledgers"][0]["kind"] == "spectrum");
        CHECK(ws["ledgers"][1]["kind"] == "universe");
        for (const json& l : ws["ledgers"]) {
            CHECK(l["status"] == "missing-file");
            CHECK(l["statusMessage"].get<std::string>().find("no file") == 0);
            CHECK(l["capacity"] == kFakeLedgerCapacity);
            CHECK(l["generation"] == 1);
            CHECK_FALSE(l.contains("file"));
        }
        CHECK(ws["ledgers"][0]["recordTypeName"] == "EntityRecord");
        CHECK(ws["ledgers"][0]["recordSize"] == 64);
        CHECK(ws["ledgers"][0]["expectedSize"] == 64 * kFakeLedgerCapacity);
        CHECK(ws["ledgers"][1]["recordTypeName"] == "AssetRecord");
        CHECK(ws["ledgers"][1]["recordSize"] == 48);
        CHECK(ws["ledgers"][1]["expectedSize"] == 48 * kFakeLedgerCapacity);
        CHECK(ws["ledgers"][0]["recordTypeId"].is_number());
        // the record type is a plain struct to schema.types
        json types = h.ok("schema.types", {{"typeIds", {ws["ledgers"][0]["recordTypeId"]}}});
        CHECK(types[0]["name"] == "EntityRecord");
        CHECK(types[0]["fields"].size() == 7);
        CHECK(h.errorCode("state.node", {{"contract", "spectrum"}, {"id", ""}}) == "not_found");
        CHECK(h.errorCode("ledger.summary", {{"kind", "universe"}}) == "not_found");
    }
    SUBCASE("ok and size-mismatch") {
        writeFakeLedgers(f.state.path());
        writeFile(f.state / "universe.005", "short");
        json ws = h.ok("workspace.open", f.request());
        CHECK(ledgerOf(ws, "spectrum")["status"] == "ok");
        CHECK(ledgerOf(ws, "spectrum")["file"]["name"] == "spectrum.005");
        CHECK(ledgerOf(ws, "spectrum")["file"]["size"] == 4096);
        CHECK(ledgerOf(ws, "universe")["status"] == "size-mismatch");
        CHECK(ledgerOf(ws, "universe")["statusMessage"].get<std::string>().find("64 * sizeof(AssetRecord)") != std::string::npos);
        // the ledger files stay listed among the other files of the epoch
        CHECK(ws["state"]["otherFiles"].size() == 2);
        // a size-mismatch ledger is still decodable as far as the file goes
        json node = h.ok("state.node", {{"contract", "universe"}, {"id", ""}});
        CHECK(node["childCount"] == 0);
        CHECK(node["inFile"] == false);
    }
}

TEST_CASE("ledgers: state.* / table.* / ledger.* routed by \"spectrum\" and \"universe\"") {
    Harness h;
    Fixture f;
    writeFakeLedgers(f.state.path());
    json ws = h.ok("workspace.open", f.request());
    CHECK(ledgerOf(ws, "spectrum")["status"] == "ok");
    CHECK(ledgerOf(ws, "universe")["status"] == "ok");

    json root = h.ok("state.node", {{"contract", "spectrum"}, {"id", ""}});
    CHECK(root["kind"] == "array");
    CHECK(root["label"] == "spectrum.005");
    CHECK(root["childCount"] == 3);
    CHECK(root["rawChildCount"] == kFakeLedgerCapacity);
    CHECK(root["container"]["population"] == 3);
    CHECK(root["tabular"] == true);
    json kids = h.ok("state.children", {{"contract", "spectrum"}, {"id", ""}});
    REQUIRE(kids["items"].size() == 3);
    CHECK(kids["items"][0]["id"] == "i:1");
    CHECK(kids["items"][0]["preview"] == "contract 1 (AA), balance 900");
    CHECK(kids["items"][2]["id"] == "i:6");
    json rawKids = h.ok("state.children", {{"contract", "spectrum"}, {"id", ""}, {"view", "raw"}, {"limit", 1000}});
    CHECK(rawKids["total"] == kFakeLedgerCapacity);
    json fields = h.ok("state.children", {{"contract", "spectrum"}, {"id", "i:5"}});
    REQUIRE(fields["items"].size() == 7);
    CHECK(fields["items"][1]["value"]["v"] == "500");
    json bytes = h.ok("state.bytes", {{"contract", "spectrum"}, {"offset", 64}, {"length", 8}});
    CHECK(bytes["hex"] == "0100000000000000");
    CHECK(bytes["fileSize"] == 4096);
    json loc = h.ok("state.locate", {{"contract", "spectrum"}, {"offset", 6 * 64 + 32}});
    CHECK(loc["id"] == "i:6/f:incomingAmount");
    json rev = h.ok("state.reveal", {{"contract", "spectrum"}, {"id", "i:6"}});
    CHECK(rev["path"][1]["index"] == 2);
    json search = h.ok("state.search", {{"contract", "spectrum"}, {"query", "500"}});
    CHECK(search["matches"].size() == 1);

    json table = h.ok("table.describe", {{"contract", "spectrum"}, {"id", ""}});
    CHECK(table["view"] == "entities");
    CHECK(table["totalRows"] == 3);
    CHECK(columnIds(table)[2] == "balance");
    json rows = h.ok("table.rows", {{"contract", "spectrum"}, {"id", ""}, {"offset", 0}, {"limit", 10},
                                    {"sort", json::array({{{"column", "balance"}, {"desc", true}}})}});
    REQUIRE(rows["rows"].size() == 3);
    CHECK(rows["rows"][0]["index"] == 1);
    CHECK(rows["rows"][0]["cells"][2]["v"] == "900");
    CHECK(rows["rows"][2]["index"] == 6);
    rows = h.ok("table.rows", {{"contract", "spectrum"}, {"id", ""}, {"offset", 0}, {"limit", 10},
                               {"filters", json::array({{{"column", "balance"}, {"op", "lt"}, {"value", "100"}}})}});
    CHECK(rows["total"] == 1);

    json sum = h.ok("ledger.summary", {{"kind", "spectrum"}});
    CHECK(sum["kind"] == "spectrum");
    CHECK(sum["population"] == 3);
    CHECK(sum["nonZeroBalances"] == 3);
    CHECK(sum["totalSupply"] == "1450");
    CHECK(sum["maxBalance"]["slot"] == 1);
    CHECK(sum["maxBalance"]["balance"] == "900");
    CHECK(sum["maxBalance"]["identity"].get<std::string>().size() == 60);
    CHECK(sum["capacity"] == kFakeLedgerCapacity);
    json usum = h.ok("ledger.summary", {{"kind", "universe"}});
    CHECK(usum["population"] == 3);
    CHECK(usum["issuances"] == 1);
    CHECK(usum["ownerships"] == 1);
    CHECK(usum["possessions"] == 1);
    REQUIRE(usum["assets"].size() == 1);
    CHECK(usum["assets"][0]["name"] == "NOST");
    CHECK(usum["assets"][0]["slot"] == 3);
    CHECK(usum["assets"][0]["totalShares"] == "2500");
    CHECK(usum["assets"][0]["possessions"] == 1);

    // lookup: by the identity the summary reported, and by hex
    const std::string richest = sum["maxBalance"]["identity"];
    json lookup = h.ok("ledger.lookup", {{"kind", "spectrum"}, {"identity", richest}});
    REQUIRE(lookup["hits"].size() == 1);
    CHECK(lookup["hits"][0]["slot"] == 1);
    CHECK(lookup["hits"][0]["id"] == "i:1");
    CHECK(lookup["probes"] == 1);
    CHECK(lookup["identity"] == richest);
    lookup = h.ok("ledger.lookup", {{"kind", "spectrum"}, {"identity", "4500000000000000010000000000000000000000000000000000000000000000"}});
    REQUIRE(lookup["hits"].size() == 1);
    CHECK(lookup["hits"][0]["slot"] == 6);
    CHECK(lookup["probes"] == 2);
    lookup = h.ok("ledger.lookup", {{"kind", "universe"}, {"identity", "0300000000000000050000000000000000000000000000000000000000000000"}});
    REQUIRE(lookup["hits"].size() == 1);
    CHECK(lookup["hits"][0]["type"] == 1);
    CHECK(lookup["hits"][0]["preview"].get<std::string>().find("issuance NOST") == 0);
    // identity search = hash lookup
    search = h.ok("state.search", {{"contract", "spectrum"}, {"query", richest}});
    CHECK(search["pattern"]["mode"] == "id");
    CHECK(search["pattern"]["note"].get<std::string>().find("hash lookup") != std::string::npos);
    CHECK(search["matches"].size() == 1);

    // universe tree
    json own = h.ok("state.children", {{"contract", "universe"}, {"id", "i:10"}});
    REQUIRE(own["items"].size() == 6);
    CHECK(own["items"][1]["id"] == "i:10/a:type");
    CHECK(own["items"][1]["value"]["k"] == "enum");
    CHECK(own["items"][1]["value"]["name"] == "ownership");
    CHECK(own["items"][5]["value"]["v"] == "2500");
    json raw = h.ok("state.children", {{"contract", "universe"}, {"id", "i:10"}, {"view", "raw"}});
    REQUIRE(raw["items"].size() == 1);
    CHECK(raw["items"][0]["id"] == "i:10/f:varStruct");
    json utable = h.ok("table.describe", {{"contract", "universe"}, {"id", ""}, {"view", "possessions"}});
    CHECK(utable["totalRows"] == 1);
    CHECK(utable["views"].size() == 4);
    json urows = h.ok("table.rows", {{"contract", "universe"}, {"id", ""}, {"view", "possessions"}, {"offset", 0}, {"limit", 10}});
    REQUIRE(urows["rows"].size() == 1);
    CHECK(urows["rows"][0]["index"] == 11);
    CHECK(urows["rows"][0]["cells"][2]["text"] == "NOST");
    CHECK(urows["rows"][0]["cells"][3]["text"] == "AA"); // managingContractIndex 1 -> contract name

    // errors
    CHECK(h.errorCode("state.node", {{"contract", "ledger"}, {"id", ""}}) == "invalid_params");
    CHECK(h.errorCode("state.node", {{"contract", true}, {"id", ""}}) == "invalid_params");
    CHECK(h.errorCode("table.rows", {{"contract", "spectrum"}, {"id", ""}, {"view", "povs"}, {"offset", 0}, {"limit", 1}}) == "not_found");
    CHECK(h.errorCode("ledger.summary", {{"kind", "contracts"}}) == "invalid_params");
    CHECK(h.errorCode("ledger.lookup", {{"kind", "spectrum"}, {"identity", "nope"}}) == "invalid_params");
    CHECK(h.errorCode("ledger.lookup", {{"kind", "spectrum"}}) == "invalid_params");
    CHECK(h.errorCode("state.digest", {{"contract", "spectrum"}}) == "invalid_params"); // contracts only
    // number routing is unchanged
    CHECK(h.ok("state.node", {{"contract", 1}, {"id", ""}})["childCount"] == 2);
}

TEST_CASE("ledgers: a single ledger file is a workspace of its own") {
    Harness h;
    Fixture f;
    writeFakeLedgers(f.state.path());
    const std::string file = (f.state / "universe.005").string();
    json ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", file));
    CHECK(ws["state"]["scope"] == "file");
    CHECK(ws["state"]["dir"] == f.state.str());
    CHECK(ws["state"]["epoch"] == 5);
    CHECK(ws["state"]["epochsAvailable"] == json::array({5}));
    CHECK(ws["state"]["otherFiles"].empty());
    CHECK(ws["contracts"].empty());
    REQUIRE(ws["ledgers"].size() == 1);
    CHECK(ws["ledgers"][0]["kind"] == "universe");
    CHECK(ws["ledgers"][0]["status"] == "ok");
    CHECK(ws["ledgers"][0]["file"]["path"] == file);
    CHECK_FALSE(hasDiagnostic(ws, "warning", "does not exist"));
    CHECK(h.ok("state.node", {{"contract", "universe"}, {"id", ""}})["childCount"] == 3);
    CHECK(h.errorCode("state.node", {{"contract", "spectrum"}, {"id", ""}}) == "not_found");
    CHECK(h.errorCode("state.node", {{"contract", 1}, {"id", ""}}) == "not_found");
    // the recent list keeps the file request
    json settings = h.ok("settings.get");
    CHECK(settings["recentWorkspaces"][0]["statePath"] == file);

    // a single contract file shows no ledgers at all
    ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", (f.state / "contract0001.005").string()));
    CHECK(ws["ledgers"].empty());
    CHECK(ws["contracts"].size() == 1);
    CHECK(h.errorCode("ledger.summary", {{"kind", "spectrum"}}) == "not_found");

    // a missing ledger file is an io_error like a missing contract file
    CHECK(h.errorCode("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", (f.state / "spectrum.007").string())) == "io_error");
}

TEST_CASE("ledgers: fs.list marks ledger files and counts their epochs") {
    Harness h;
    Fixture f;
    writeFakeLedgers(f.state.path(), 5);
    writeFile(f.state / "spectrum.007", "x"); // an epoch with a ledger file only
    writeFile(f.state / "universe.bak", "x");
    json listing = h.ok("fs.list", {{"path", f.state.str()}});
    CHECK(listing["hints"]["stateEpochs"] == json::array({5, 7}));
    std::map<std::string, json> byName;
    for (const json& e : listing["entries"]) byName[e["name"]] = e;
    CHECK(byName["spectrum.005"]["ledger"] == json({{"kind", "spectrum"}, {"epoch", 5}}));
    CHECK(byName["universe.005"]["ledger"] == json({{"kind", "universe"}, {"epoch", 5}}));
    CHECK(byName["spectrum.007"]["ledger"]["epoch"] == 7);
    CHECK_FALSE(byName["spectrum.005"].contains("state"));
    CHECK_FALSE(byName["universe.bak"].contains("ledger"));
    CHECK_FALSE(byName["contract0001.005"].contains("ledger"));
    CHECK(byName["contract0001.005"]["state"]["index"] == 1);
}

TEST_CASE("ledgers: live updates of ledger files") {
    Harness h;
    Fixture f;
    writeFakeLedgers(f.state.path());
    EventCollector events(h.bus);
    json ws = h.ok("workspace.open", f.request());
    const int gen0 = ledgerOf(ws, "spectrum")["generation"];
    CHECK(h.ok("ledger.summary", {{"kind", "spectrum"}})["population"] == 3);

    // a rewritten spectrum file: contracts.changed with `ledgers`, the generation bumps and the caches are dropped
    std::vector<std::uint8_t> spectrum = fakeSpectrum();
    const std::vector<std::uint8_t> newKey = keyBytes(20, 1);
    std::copy(newKey.begin(), newKey.end(), spectrum.begin() + 20 * 64);
    putLe(spectrum, 20 * 64 + 32, 42, 8);
    writeBytes(f.state / "spectrum.005", spectrum);
    json ev = events.waitFor("contracts.changed", milliseconds(8000), [](const json& e) { return e.contains("ledgers"); });
    REQUIRE_FALSE(ev.is_null());
    CHECK(ev["workspaceId"] == ws["id"]);
    CHECK(ev["contracts"].empty());
    REQUIRE(ev["ledgers"].size() == 1);
    CHECK(ev["ledgers"][0]["kind"] == "spectrum");
    CHECK(ev["ledgers"][0]["status"] == "ok");
    CHECK(ev["ledgers"][0]["generation"] == gen0 + 1);
    CHECK(h.ok("ledger.summary", {{"kind", "spectrum"}})["population"] == 4);
    json got = h.ok("workspace.get");
    CHECK(ledgerOf(got, "spectrum")["generation"] == gen0 + 1);
    CHECK(ledgerOf(got, "universe")["generation"] == 1);

    // a ledger file disappearing / appearing rescans the directory
    const std::size_t seen = events.size();
    fs::remove(f.state / "universe.005");
    json up = events.waitFor("workspace.updated", milliseconds(8000),
                             [](const json& w) { return ledgerOf(w, "universe")["status"] == "missing-file"; }, seen);
    REQUIRE_FALSE(up.is_null());
    CHECK(ledgerOf(up, "spectrum")["status"] == "ok");
    CHECK(ledgerOf(up, "spectrum")["generation"] == gen0 + 2);
    const std::size_t seen2 = events.size();
    writeBytes(f.state / "universe.005", fakeUniverse());
    up = events.waitFor("workspace.updated", milliseconds(8000), [](const json& w) { return ledgerOf(w, "universe")["status"] == "ok"; }, seen2);
    REQUIRE_FALSE(up.is_null());
    CHECK(h.ok("ledger.summary", {{"kind", "universe"}})["issuances"] == 1);
}

TEST_CASE("ledgers: a core without the record types reports schema-error for present files") {
    Harness h;
    TempDir dir;
    // the fake core of fixtures.h minus the ledger types
    std::string def = fakeContractDef();
    const auto a = def.find("#define SPECTRUM_DEPTH");
    const auto b = def.find("namespace QPI");
    def.erase(a, b - a);
    def.insert(a, "struct m256i { unsigned long long u64[4]; };\n");
    qstate::testing::GitFixtureRepo git(dir.path() / "core");
    writeFile(dir.path() / "core/src/contract_core/contract_def.h", def);
    writeFile(dir.path() / "core/src/contracts/B.h", fakeHeaderB());
    writeFile(dir.path() / "core/src/public_settings.h", fakePublicSettings(5));
    git.commit("no ledgers");
    git.tag("v5.0.0");
    TempDir state;
    writeFakeState(state.path());
    writeFakeLedgers(state.path());
    json ws = h.ok("workspace.open", Fixture::requestFor(git.url(), "v5.0.0", state.str()));
    CHECK(ws["contracts"].size() == 3);
    REQUIRE(ws["ledgers"].size() == 2);
    CHECK(ws["ledgers"][0]["status"] == "schema-error");
    CHECK(ws["ledgers"][0]["statusMessage"].get<std::string>().find("EntityRecord") != std::string::npos);
    CHECK_FALSE(ws["ledgers"][0].contains("capacity"));
    CHECK(hasDiagnostic(ws, "warning", "ledger spectrum cannot be decoded"));
    CHECK(h.errorCode("state.node", {{"contract", "spectrum"}, {"id", ""}}) == "schema_error");
    // the bytes are still readable
    CHECK(h.ok("state.bytes", {{"contract", "spectrum"}, {"offset", 0}, {"length", 4}})["fileSize"] == 4096);
}

TEST_CASE("ledgers: the real epoch-229 directory") {
    const std::string core229 = qstate::testing::coreDir229();
    const std::string state = qstate::testing::stateDir();
    const std::string repo = qstate::testing::coreRepo();
    if (state.empty() || (core229.empty() && repo.empty())) {
        MESSAGE("QSTATE_TEST_STATE_DIR with QSTATE_TEST_CORE_REPO / QSTATE_TEST_CORE_DIR_229 not set: skipped");
        return;
    }
    // sources matching the files: the real repository with ref auto, else the snapshot as a one-commit repository
    std::unique_ptr<TempDir> dir;
    std::unique_ptr<qstate::testing::GitFixtureRepo> git;
    std::string url = repo, ref = "auto";
    if (repo.empty()) {
        dir = std::make_unique<TempDir>();
        git = std::make_unique<qstate::testing::GitFixtureRepo>(dir->path() / "snapshot");
        for (const auto& entry : fs::directory_iterator(core229)) {
            if (entry.path().filename() == ".git") continue;
            fs::copy(entry.path(), git->dir() / entry.path().filename(), fs::copy_options::recursive);
        }
        git->commit("v1.303.2");
        git->tag("snapshot");
        url = git->url();
        ref = "snapshot";
    }
    Harness h;
    json ws = h.ok("workspace.open", Fixture::requestFor(url, ref, state));
    REQUIRE(ws["ledgers"].size() == 2);
    const json sp = ledgerOf(ws, "spectrum"), un = ledgerOf(ws, "universe");
    INFO(sp.dump() << "\n" << un.dump());
    CHECK(sp["status"] == "ok");
    CHECK(un["status"] == "ok");
    CHECK(sp["expectedSize"] == 1073741824);
    CHECK(un["expectedSize"] == 805306368);
    CHECK(sp["capacity"] == 16777216);
    const auto t0 = std::chrono::steady_clock::now();
    json root = h.ok("state.node", {{"contract", "spectrum"}, {"id", ""}});
    const double scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(root["childCount"] == 670379);
    CHECK(root["label"] == "spectrum.229");
    json sum = h.ok("ledger.summary", {{"kind", "spectrum"}});
    CHECK(sum["totalSupply"] == "176923511873269");
    json usum = h.ok("ledger.summary", {{"kind", "universe"}});
    CHECK(usum["issuances"] == 147);
    CHECK(usum["ownerships"] == 40495);
    CHECK(usum["possessions"] == 40495);
    CHECK(usum["assets"][0]["name"] == "NOST");
    // the QX contract identity: contract 1's id
    json qx = h.ok("state.children", {{"contract", "spectrum"}, {"id", "i:1"}, {"limit", 1}});
    const std::string qxIdentity = qx["items"][0]["value"]["identity"];
    json lookup = h.ok("ledger.lookup", {{"kind", "spectrum"}, {"identity", qxIdentity}});
    REQUIRE(lookup["hits"].size() == 1);
    CHECK(lookup["hits"][0]["slot"] == 1);
    lookup = h.ok("ledger.lookup", {{"kind", "universe"}, {"identity", qxIdentity}});
    CHECK_FALSE(lookup["hits"].empty());
    const auto t1 = std::chrono::steady_clock::now();
    json rows = h.ok("table.rows", {{"contract", "spectrum"}, {"id", ""}, {"offset", 0}, {"limit", 1},
                                    {"sort", json::array({{{"column", "balance"}, {"desc", true}}})}});
    const double sortMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    CHECK(rows["total"] == 670379);
    CHECK(rows["rows"][0]["index"] == sum["maxBalance"]["slot"]);
    std::cerr << "[ledgers] real 229 through the service: spectrum scan " << scanMs << " ms, sort by balance " << sortMs
              << " ms, universe records of QX: " << lookup["hits"].size() << "\n";
}
