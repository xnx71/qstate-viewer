// Workspace lifecycle, state methods and live updates on a tiny synthetic core.
#include "fixtures.h"

#include "qstate/support/identity.h"
#include "qstate/support/k12.h"

#include <doctest/doctest.h>

#include <atomic>
#include <random>
#include <thread>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;
using std::chrono::milliseconds;

namespace {

struct Fixture {
    TempDir core, state;
    Fixture() {
        writeFakeCore(core.path());
        writeFakeState(state.path());
    }
    json request() const { return {{"coreDir", core.str()}, {"stateDir", state.str()}}; }
};

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("methods that need a workspace report no_workspace") {
    Harness h;
    CHECK(h.ok("workspace.get").is_null());
    CHECK(h.errorCode("workspace.reload") == "no_workspace");
    CHECK(h.errorCode("schema.types", {{"typeIds", json::array({0})}}) == "no_workspace");
    CHECK(h.errorCode("state.node", {{"contract", 1}, {"id", ""}}) == "no_workspace");
    CHECK(h.errorCode("state.children", {{"contract", 1}, {"id", ""}}) == "no_workspace");
    CHECK(h.errorCode("state.bytes", {{"contract", 1}, {"offset", 0}, {"length", 4}}) == "no_workspace");
    CHECK(h.errorCode("state.locate", {{"contract", 1}, {"offset", 0}}) == "no_workspace");
    CHECK(h.errorCode("state.search", {{"contract", 1}, {"query", "x"}}) == "no_workspace");
    CHECK(h.errorCode("state.digest", {{"contract", 1}}) == "no_workspace");
    CHECK(h.errorCode("table.describe", {{"contract", 1}, {"id", ""}}) == "no_workspace");
    CHECK(h.errorCode("table.rows", {{"contract", 1}, {"id", ""}, {"offset", 0}, {"limit", 5}}) == "no_workspace");
    CHECK(h.ok("workspace.close").is_null()); // closing nothing is fine
}

TEST_CASE("workspace.open on the synthetic core") {
    Harness h;
    Fixture f;
    writeFile(f.state / "spectrum.005", "abc");
    writeFile(f.state / "universe.005", "abcd");
    writeFile(f.state / "contract_exec_fees_rec.005", "abcde");
    writeFile(f.state / "weird.bin", "abcdef");
    json ws = h.ok("workspace.open", f.request());
    CHECK(ws["id"].get<int>() >= 1);
    CHECK(ws["request"]["coreDir"] == f.core.str());
    CHECK(ws["request"]["stateDir"] == f.state.str());
    CHECK(ws["core"]["dir"] == f.core.str());
    CHECK(ws["core"]["sourceDir"] == f.core.str());
    CHECK(ws["core"]["ref"] == "");
    CHECK(ws["core"]["version"] == "1.2.3");
    CHECK(ws["core"]["epoch"] == 5);
    CHECK(ws["core"]["fileCount"].get<int>() >= 2);
    CHECK(ws["core"]["parseMs"].get<double>() >= 0);
    CHECK(ws["state"]["dir"] == f.state.str());
    CHECK(ws["state"]["epoch"] == 5);
    CHECK(ws["state"]["epochsAvailable"] == json::array({5}));
    std::map<std::string, std::string> others;
    for (const json& o : ws["state"]["otherFiles"]) others[o["name"]] = o["kind"];
    CHECK(others["spectrum.005"] == "spectrum");
    CHECK(others["universe.005"] == "universe");
    CHECK(others["contract_exec_fees_rec.005"] == "contract-exec-fees");
    CHECK(others["weird.bin"] == "unknown");
    CHECK(ws["state"]["otherFiles"].size() == 4);

    REQUIRE(ws["contracts"].size() == 3);
    for (unsigned i = 0; i < 3; ++i) {
        const json c = ws["contracts"][i];
        CHECK(c["index"] == i);
        CHECK(c["status"] == "ok");
        CHECK(c["generation"] == 1);
        CHECK_FALSE(c.contains("statusMessage"));
    }
    const json c1 = contractOf(ws, 1);
    CHECK(c1["name"] == "AA");
    CHECK(c1["structName"] == "A");
    CHECK(c1["stateTypeName"] == "A::StateData");
    CHECK(c1["stateTypeId"].is_number());
    CHECK(c1["constructionEpoch"] == 5);
    CHECK(c1["expectedSize"] == 328);
    CHECK(c1["file"]["size"] == 328);
    CHECK(c1["file"]["name"] == "contract0001.005");
    CHECK(c1["file"]["path"] == (f.state / "contract0001.005").string());
    CHECK(c1["file"]["mtimeMs"].is_number());
    CHECK(contractOf(ws, 0)["name"] == "");
    CHECK(contractOf(ws, 0)["stateTypeName"] == "Contract0State");
    CHECK(ws["diagnostics"].is_array());

    // workspace.get returns the same thing; settings keep the recent workspace
    CHECK(h.ok("workspace.get") == ws);
    json recent = h.ok("settings.get")["recentWorkspaces"];
    REQUIRE(recent.size() == 1);
    CHECK(recent[0]["coreDir"] == f.core.str());
    CHECK(recent[0]["stateDir"] == f.state.str());

    // ids increase on open and reload
    json again = h.ok("workspace.open", f.request());
    CHECK(again["id"].get<int>() > ws["id"].get<int>());
    CHECK(h.ok("settings.get")["recentWorkspaces"].size() == 1); // deduplicated
    json reloaded = h.ok("workspace.reload");
    CHECK(reloaded["id"].get<int>() > again["id"].get<int>());
    CHECK(reloaded["request"] == again["request"]);
    CHECK(contractOf(reloaded, 1)["generation"].get<int>() > contractOf(again, 1)["generation"].get<int>());
    h.ok("workspace.close");
    CHECK(h.ok("workspace.get").is_null());
    CHECK(h.errorCode("state.node", {{"contract", 1}, {"id", ""}}) == "no_workspace");
    CHECK(h.errorCode("workspace.reload") == "no_workspace");
}

TEST_CASE("contract statuses") {
    Harness h;
    Fixture f;
    // BB: wrong size; AA: file missing; contract 3: no such contract in the core sources
    writeBytes(f.state / "contract0002.005", patternBytes(16, 1));
    fs::remove(f.state / "contract0001.005");
    writeBytes(f.state / "contract0003.005", patternBytes(4, 1));
    json ws = h.ok("workspace.open", f.request());
    REQUIRE(ws["contracts"].size() == 4);
    const json c1 = contractOf(ws, 1), c2 = contractOf(ws, 2), c3 = contractOf(ws, 3);
    CHECK(c1["status"] == "missing-file");
    CHECK_FALSE(c1.contains("file"));
    CHECK(c1["statusMessage"].get<std::string>().find("contract0001.005") != std::string::npos);
    CHECK(c2["status"] == "size-mismatch");
    CHECK(c2["file"]["size"] == 16);
    CHECK(c2["expectedSize"] == 8);
    const std::string msg = c2["statusMessage"];
    CHECK(msg.find("16") != std::string::npos);
    CHECK(msg.find("8 bytes") != std::string::npos);
    CHECK(msg.find("B::StateData") != std::string::npos);
    CHECK(c3["status"] == "unknown-contract");
    CHECK(c3["file"]["size"] == 4);
    CHECK(c3["statusMessage"].get<std::string>().find("3") != std::string::npos);

    // state calls: no file / unknown contract -> not_found, but bytes work for any contract with a file
    CHECK(h.errorCode("state.node", {{"contract", 1}, {"id", ""}}) == "not_found");
    CHECK(h.errorCode("state.node", {{"contract", 3}, {"id", ""}}) == "not_found");
    CHECK(h.errorCode("state.node", {{"contract", 99}, {"id", ""}}) == "not_found");
    CHECK(h.ok("state.bytes", {{"contract", 3}, {"offset", 0}, {"length", 8}})["length"] == 4);
    CHECK(h.errorCode("state.bytes", {{"contract", 1}, {"offset", 0}, {"length", 8}}) == "not_found");
    // the mismatching contract is still browsable
    json node = h.ok("state.node", {{"contract", 2}, {"id", ""}});
    CHECK(node["typeName"] == "B::StateData");
}

TEST_CASE("construction epoch hint for missing files") {
    Harness h;
    Fixture f;
    fs::remove(f.state / "contract0002.005");
    json ws = h.ok("workspace.open", f.request());
    const json c2 = contractOf(ws, 2);
    CHECK(c2["status"] == "missing-file");
    CHECK(c2["statusMessage"].get<std::string>().find("not yet constructed") != std::string::npos);
}

TEST_CASE("size mismatch message points at the epoch difference of core and files") {
    Harness h;
    TempDir core, state;
    writeFakeCore(core.path(), 9); // core sources of epoch 9
    writeFakeState(state.path(), 5);
    writeBytes(state / "contract0002.005", patternBytes(16, 1));
    json ws = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}});
    const std::string msg = contractOf(ws, 2)["statusMessage"];
    CHECK(msg.find("epoch 5") != std::string::npos);
    CHECK(msg.find("epoch 9") != std::string::npos);
    bool warned = false;
    for (const json& d : ws["diagnostics"]) {
        if (d["severity"] == "warning" && d["message"].get<std::string>().find("epoch 9") != std::string::npos) warned = true;
    }
    CHECK(warned);
}

TEST_CASE("epoch selection") {
    Harness h;
    TempDir core, state;
    writeFakeCore(core.path());
    writeFakeState(state.path(), 4);
    writeFakeState(state.path(), 5);
    json ws = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}});
    CHECK(ws["state"]["epoch"] == 5);
    CHECK(ws["state"]["epochsAvailable"] == json::array({4, 5}));
    CHECK(contractOf(ws, 1)["file"]["name"] == "contract0001.005");
    ws = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}, {"epoch", 4}});
    CHECK(ws["state"]["epoch"] == 4);
    CHECK(contractOf(ws, 1)["file"]["name"] == "contract0001.004");
    // an epoch without files: no error, all missing, with a diagnostic
    ws = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}, {"epoch", 7}});
    CHECK(ws["state"]["epoch"] == 7);
    CHECK(contractOf(ws, 1)["status"] == "missing-file");
    bool found = false;
    for (const json& d : ws["diagnostics"]) found = found || d["message"].get<std::string>().find("epoch 7") != std::string::npos;
    CHECK(found);
}

TEST_CASE("workspace.open reports bad input") {
    Harness h;
    Fixture f;
    TempDir empty;
    CHECK(h.errorCode("workspace.open", json::object()) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", ""}, {"stateDir", f.state.str()}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}, {"epoch", "x"}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}, {"epoch", -1}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}, {"defines", 5}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}, {"defines", json::array({5})}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", (f.state / "nope").string()}}) == "io_error");
    CHECK(h.errorCode("workspace.open", {{"coreDir", (f.core / "nope").string()}, {"stateDir", f.state.str()}}) == "io_error");
    CHECK(h.errorCode("workspace.open", {{"coreDir", empty.str()}, {"stateDir", f.state.str()}}) == "schema_error");
    // an explicit ref needs a git repository
    CHECK(h.errorCode("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}, {"coreRef", "v1.0"}}) == "invalid_params");
    // a failed open leaves no workspace behind
    CHECK(h.ok("workspace.get").is_null());

    // "auto" without git falls back to the sources on disk, with a warning
    json ws = h.ok("workspace.open", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}, {"coreRef", "auto"}});
    CHECK(ws["core"]["ref"] == "");
    bool warned = false;
    for (const json& d : ws["diagnostics"]) {
        warned = warned || (d["severity"] == "warning" && d["message"].get<std::string>().find("auto") != std::string::npos);
    }
    CHECK(warned);
    CHECK(h.ok("workspace.get")["id"] == ws["id"]);
}

TEST_CASE("defines are passed to the preprocessor") {
    Harness h;
    TempDir core, state;
    writeFile(core / "src/contract_core/contract_def.h", R"(
struct A { struct StateData {
#ifdef EXTRA
    unsigned long long extra;
#endif
    unsigned long long x; }; };
constexpr struct ContractDescription { char assetName[8]; unsigned short constructionEpoch, destructionEpoch; unsigned long long stateSize; }
contractDescriptions[] = { {"", 0, 0, sizeof(A::StateData)}, };
)");
    json plain = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}});
    CHECK(contractOf(plain, 0)["expectedSize"] == 8);
    json extra = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}, {"defines", json::array({"EXTRA"})}});
    CHECK(contractOf(extra, 0)["expectedSize"] == 16);
    CHECK(extra["request"]["defines"] == json::array({"EXTRA"}));
    json valued = h.ok("workspace.open", {{"coreDir", core.str()}, {"stateDir", state.str()}, {"defines", json::array({"EXTRA=1"})}});
    CHECK(contractOf(valued, 0)["expectedSize"] == 16);
}

TEST_CASE("state methods on the synthetic contract") {
    Harness h;
    Fixture f;
    h.ok("workspace.open", f.request());

    json root = h.ok("state.node", {{"contract", 1}, {"id", ""}});
    CHECK(root["id"] == "");
    CHECK(root["kind"] == "struct");
    CHECK(root["typeName"] == "A::StateData");
    CHECK(root["offset"] == 0);
    CHECK(root["size"] == 328);
    CHECK(root["inFile"] == true);
    CHECK(root["childCount"] == 2);

    json kids = h.ok("state.children", {{"contract", 1}, {"id", ""}});
    CHECK(kids["total"] == 2);
    REQUIRE(kids["items"].size() == 2);
    CHECK(kids["items"][0]["label"] == "rows");
    CHECK(kids["items"][1]["label"] == "counter");
    CHECK(kids["items"][1]["value"]["v"] == "7");
    const std::string rowsId = kids["items"][0]["id"];
    CHECK(kids["items"][0]["tabular"] == true);

    json rows = h.ok("state.children", {{"contract", 1}, {"id", rowsId}, {"limit", 3}});
    CHECK(rows["total"] == 8);
    CHECK(rows["items"].size() == 3);
    json page2 = h.ok("state.children", {{"contract", 1}, {"id", rowsId}, {"offset", 6}, {"limit", 10}});
    CHECK(page2["items"].size() == 2);
    json nonEmpty = h.ok("state.children", {{"contract", 1}, {"id", rowsId}, {"hideEmpty", true}});
    CHECK(nonEmpty["total"] == 1);
    json rawKids = h.ok("state.children", {{"contract", 1}, {"id", ""}, {"view", "raw"}});
    CHECK(rawKids["total"] == 2);

    json info = h.ok("table.describe", {{"contract", 1}, {"id", rowsId}});
    CHECK(info["id"] == rowsId);
    CHECK(info["totalRows"] == 8);
    CHECK(info["columns"].size() >= 3);
    json tbl = h.ok("table.rows", {{"contract", 1}, {"id", rowsId}, {"offset", 0}, {"limit", 5}});
    CHECK(tbl["total"] == 8);
    CHECK(tbl["rows"].size() == 5);
    CHECK(tbl["rows"][0]["cells"].size() == info["columns"].size());
    json sorted = h.ok("table.rows", {{"contract", 1}, {"id", rowsId}, {"offset", 0}, {"limit", 8},
                                      {"sort", json::array({json{{"column", "value.amount"}, {"desc", true}}})}});
    CHECK(sorted["rows"][0]["index"] == 0); // the only non-zero amount first
    CHECK(h.errorCode("table.rows", {{"contract", 1}, {"id", rowsId}, {"offset", 0}, {"limit", 5},
                                     {"sort", json::array({json{{"column", "nope"}}})}}) == "invalid_params");
    CHECK(h.errorCode("table.rows", {{"contract", 1}, {"id", rowsId}, {"offset", 0}, {"limit", 5},
                                     {"filters", json::array({json{{"column", "value.amount"}}})}}) == "invalid_params");

    // bytes / locate / search / digest
    json bytes = h.ok("state.bytes", {{"contract", 1}, {"offset", 32}, {"length", 8}});
    CHECK(bytes["hex"] == "2a00000000000000");
    CHECK(bytes["offset"] == 32);
    CHECK(bytes["length"] == 8);
    CHECK(bytes["fileSize"] == 328);
    json tail = h.ok("state.bytes", {{"contract", 1}, {"offset", 320}, {"length", 100}});
    CHECK(tail["length"] == 8);
    CHECK(tail["hex"] == "0700000000000000");
    json past = h.ok("state.bytes", {{"contract", 1}, {"offset", 1000}, {"length", 10}});
    CHECK(past["length"] == 0);
    CHECK(past["hex"] == "");
    CHECK(h.errorCode("state.bytes", {{"contract", 1}, {"offset", 0}, {"length", 65537}}) == "invalid_params");
    CHECK(h.ok("state.bytes", {{"contract", 1}, {"offset", 0}, {"length", 65536}})["length"] == 328);
    CHECK(h.errorCode("state.bytes", {{"contract", 1}, {"offset", -1}, {"length", 4}}) == "invalid_params");

    json loc = h.ok("state.locate", {{"contract", 1}, {"offset", 320}});
    CHECK(loc["offset"] == 320);
    CHECK(loc["path"].back()["label"] == "counter");
    CHECK(h.errorCode("state.locate", {{"contract", 1}, {"offset", 5000}}) == "not_found");

    // reveal: by id and by offset, same exact path; the steps point at the children listing
    json rv = h.ok("state.reveal", {{"contract", 1}, {"offset", 320}});
    json rvId = h.ok("state.reveal", {{"contract", 1}, {"id", loc["id"]}, {"hideEmpty", false}});
    CHECK(rv == rvId);
    CHECK(rv["id"] == loc["id"]);
    CHECK(rv["path"].size() == loc["path"].size());
    CHECK(rv["path"][0]["index"] == 0);
    for (std::size_t i = 1; i < rv["path"].size(); ++i) {
        const json& step = rv["path"][i];
        CHECK(step["id"] == loc["path"][i]["id"]);
        json page = h.ok("state.children", {{"contract", 1}, {"id", rv["path"][i - 1]["id"]}, {"view", step["view"]},
                                            {"offset", step["index"]}, {"limit", 1}});
        REQUIRE(page["items"].size() == 1);
        CHECK(page["items"][0]["id"] == step["id"]);
        CHECK(page["total"] == rv["path"][i - 1]["childTotal"]);
    }
    CHECK(h.errorCode("state.reveal", {{"contract", 1}}) == "invalid_params");
    CHECK(h.errorCode("state.reveal", {{"contract", 1}, {"id", ""}, {"offset", 0}}) == "invalid_params");
    CHECK(h.errorCode("state.reveal", {{"contract", 1}, {"id", "f:nope"}}) == "not_found");
    CHECK(h.errorCode("state.reveal", {{"contract", 1}, {"offset", 5000}}) == "not_found");

    json found = h.ok("state.search", {{"contract", 1}, {"query", "0x2a00000000000000"}});
    CHECK(found["pattern"]["mode"] == "hex");
    REQUIRE(found["matches"].size() == 1);
    CHECK(found["matches"][0]["offset"] == 32);
    CHECK(found["truncated"] == false);
    json intFound = h.ok("state.search", {{"contract", 1}, {"query", "7"}, {"mode", "int"}});
    CHECK(intFound["pattern"]["mode"] == "int");
    CHECK(intFound["matches"].size() >= 1);
    CHECK(h.errorCode("state.search", {{"contract", 1}, {"query", "7"}, {"limit", 0}}) == "invalid_params");
    CHECK(h.errorCode("state.search", {{"contract", 1}, {"query", "7"}, {"limit", 5001}}) == "invalid_params");
    CHECK(h.errorCode("state.search", {{"contract", 1}, {"query", "7"}, {"mode", "zzz"}}) == "invalid_params");
    CHECK(h.errorCode("state.search", {{"contract", 1}, {"query", "not an identity"}, {"mode", "id"}}) == "invalid_params");
    CHECK(h.errorCode("state.children", {{"contract", 1}, {"id", ""}, {"limit", 1001}}) == "invalid_params");
    CHECK(h.errorCode("state.children", {{"contract", 1}, {"id", ""}, {"limit", 0}}) == "invalid_params");
    CHECK(h.errorCode("state.children", {{"contract", 1}, {"id", ""}, {"view", "weird"}}) == "invalid_params");
    CHECK(h.errorCode("state.node", {{"contract", 1}, {"id", "nonsense"}}) == "not_found");
    CHECK(h.errorCode("state.node", {{"id", ""}}) == "invalid_params");

    json digest = h.ok("state.digest", {{"contract", 1}});
    const std::string content = readAll(f.state / "contract0001.005");
    const auto expected = support::k12Digest(content.data(), content.size());
    CHECK(digest["k12"] == support::toHex(expected.data(), expected.size()));
    CHECK(digest["elapsedMs"].get<double>() >= 0);

    // schema.types
    const int typeId = h.ok("workspace.get")["contracts"][1]["stateTypeId"];
    json types = h.ok("schema.types", {{"typeIds", json::array({typeId})}});
    REQUIRE(types.size() == 1);
    CHECK(types[0]["id"] == typeId);
    CHECK(types[0]["name"] == "A::StateData");
    CHECK(types[0]["kind"] == "record");
    CHECK(types[0]["recordKind"] == "struct");
    CHECK(types[0]["size"] == 328);
    REQUIRE(types[0]["fields"].size() == 2);
    CHECK(types[0]["fields"][0]["name"] == "rows");
    CHECK(types[0]["fields"][0]["offset"] == 0);
    CHECK(types[0]["fields"][1]["offset"] == 320);
    const int rowsType = types[0]["fields"][0]["type"];
    json rowsInfo = h.ok("schema.types", {{"typeIds", json::array({rowsType})}});
    CHECK(rowsInfo[0]["role"]["kind"] == "array");
    CHECK(rowsInfo[0]["role"]["capacity"] == 8);
    const int rowType = rowsInfo[0]["role"]["element"];
    json rowInfo = h.ok("schema.types", {{"typeIds", json::array({rowType})}});
    const int idType = rowInfo[0]["fields"][0]["type"];
    CHECK(h.ok("schema.types", {{"typeIds", json::array({idType})}})[0]["role"]["kind"] == "id");
    CHECK(h.errorCode("schema.types", {{"typeIds", json::array({999999})}}) == "not_found");
    CHECK(h.errorCode("schema.types", {{"typeIds", 5}}) == "invalid_params");
}

TEST_CASE("a cancelled call does not open a workspace") {
    Harness h;
    Fixture f;
    auto token = std::make_shared<rpc::CancelToken>();
    token->cancel();
    rpc::CallContext ctx("workspace.open", token, "http");
    json r = h.dispatcher.dispatch("workspace.open", f.request(), ctx);
    REQUIRE(r.contains("error"));
    CHECK(r["error"]["message"] == "cancelled");
    CHECK(h.ok("workspace.get").is_null());
}

TEST_CASE("state.digest and state.search honour cancellation") {
    Harness h;
    Fixture f;
    h.ok("workspace.open", f.request());
    for (const char* method : {"state.digest", "state.search"}) {
        auto token = std::make_shared<rpc::CancelToken>();
        token->cancel();
        rpc::CallContext ctx(method, token, "http");
        json params = {{"contract", 1}, {"query", "x"}};
        json r = h.dispatcher.dispatch(method, params, ctx);
        INFO(method << " " << r.dump());
        // Tiny inputs may finish before the first cancellation check: either outcome is a valid response, but an
        // error must be the "cancelled" one.
        if (r.contains("error")) CHECK(r["error"]["message"] == "cancelled");
    }
}

TEST_CASE("live: state file changes produce contracts.changed") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    json ws = h.ok("workspace.open", f.request());
    const int wsId = ws["id"];
    const int gen0 = contractOf(ws, 2)["generation"];

    // grow BB's file by 8 bytes: size mismatch, new generation
    appendBytes(f.state / "contract0002.005", patternBytes(8, 3));
    json ev = h.dispatcher.has("app.info") ? events.waitFor("contracts.changed") : json();
    REQUIRE_FALSE(ev.is_null());
    CHECK(ev["workspaceId"] == wsId);
    REQUIRE(ev["contracts"].size() == 1);
    const json changed = ev["contracts"][0];
    CHECK(changed["index"] == 2);
    CHECK(changed["generation"].get<int>() > gen0);
    CHECK(changed["file"]["size"] == 16);
    CHECK(changed["status"] == "size-mismatch");
    CHECK(h.ok("state.bytes", {{"contract", 2}, {"offset", 0}, {"length", 100}})["length"] == 16);
    CHECK(h.ok("workspace.get")["contracts"][2]["file"]["size"] == 16);
    CHECK(events.count("workspace.updated") == 0);

    // same-size rewrite: content changes, generation bumps again, decoder sees the new bytes
    const std::size_t seen = events.size();
    const int gen1 = changed["generation"];
    json before = h.ok("state.children", {{"contract", 1}, {"id", ""}});
    CHECK(before["items"][1]["value"]["v"] == "7");
    std::vector<std::uint8_t> a(328, 0);
    a[320] = 99;
    std::this_thread::sleep_for(milliseconds(30)); // distinct mtime
    writeBytes(f.state / "contract0001.005", a);
    json ev2 = events.waitFor("contracts.changed", milliseconds(8000), [](const json& p) { return p["contracts"][0]["index"] == 1; }, seen);
    REQUIRE_FALSE(ev2.is_null());
    CHECK(ev2["contracts"][0]["generation"].get<int>() > 1);
    CHECK(ev2["contracts"][0]["status"] == "ok");
    json after = h.ok("state.children", {{"contract", 1}, {"id", ""}});
    CHECK(after["items"][1]["value"]["v"] == "99");
    (void)gen1;
}

TEST_CASE("live: a burst of writes is reported once") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    h.ok("workspace.open", f.request());
    for (int i = 0; i < 10; ++i) {
        appendBytes(f.state / "contract0002.005", patternBytes(1, static_cast<std::uint8_t>(i)));
        std::this_thread::sleep_for(milliseconds(5));
    }
    json ev = events.waitFor("contracts.changed");
    REQUIRE_FALSE(ev.is_null());
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(events.count("contracts.changed") == 1);
    CHECK(h.ok("workspace.get")["contracts"][2]["file"]["size"] == 18);
}

TEST_CASE("live: files appearing and disappearing produce workspace.updated") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    json ws = h.ok("workspace.open", f.request());

    writeBytes(f.state / "contract0003.005", patternBytes(4, 1));
    json up = events.waitFor("workspace.updated");
    REQUIRE_FALSE(up.is_null());
    CHECK(up["id"].get<int>() > ws["id"].get<int>());
    CHECK(contractOf(up, 3)["status"] == "unknown-contract");
    CHECK(h.ok("workspace.get")["id"] == up["id"]);
    // generations stay monotonic across the new workspace
    CHECK(contractOf(up, 1)["generation"].get<int>() > contractOf(ws, 1)["generation"].get<int>());

    const std::size_t seen = events.size();
    fs::remove(f.state / "contract0001.005");
    json up2 = events.waitFor("workspace.updated", milliseconds(8000), [](const json& w) { return contractOf(w, 1)["status"] == "missing-file"; }, seen);
    REQUIRE_FALSE(up2.is_null());
    CHECK(contractOf(up2, 1)["status"] == "missing-file");

    // a file of another epoch changes the list of available epochs
    const std::size_t seen2 = events.size();
    writeBytes(f.state / "contract0000.006", patternBytes(16, 1));
    json up3 = events.waitFor("workspace.updated", milliseconds(8000), [](const json& w) { return w["state"]["epochsAvailable"].size() == 2; }, seen2);
    REQUIRE_FALSE(up3.is_null());
    CHECK(up3["state"]["epoch"] == 6); // no explicit epoch: the newest one is shown
}

TEST_CASE("live: editing a core header re-extracts the schema") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    json ws = h.ok("workspace.open", f.request());
    CHECK(contractOf(ws, 2)["status"] == "ok");

    std::this_thread::sleep_for(milliseconds(30));
    writeFile(f.core / "src/contracts/B.h", fakeHeaderB(true)); // StateData grows to 16 bytes
    json up = events.waitFor("workspace.updated");
    REQUIRE_FALSE(up.is_null());
    CHECK(up["id"].get<int>() > ws["id"].get<int>());
    const json c2 = contractOf(up, 2);
    CHECK(c2["expectedSize"] == 16);
    CHECK(c2["status"] == "size-mismatch");
    CHECK(c2["generation"].get<int>() > contractOf(ws, 2)["generation"].get<int>());
    CHECK(contractOf(up, 1)["status"] == "ok");
    // new schema is live for state calls
    CHECK(h.ok("state.node", {{"contract", 2}, {"id", ""}})["size"] == 16);

    // fixing the state file makes it ok again (contracts.changed with the new workspace id)
    const std::size_t seen = events.size();
    appendBytes(f.state / "contract0002.005", patternBytes(8, 3));
    json ev = events.waitFor("contracts.changed", milliseconds(8000), {}, seen);
    REQUIRE_FALSE(ev.is_null());
    CHECK(ev["workspaceId"] == up["id"]);
    CHECK(ev["contracts"][0]["status"] == "ok");

    // a broken header: the previous schema stays, errors show up as diagnostics
    const std::size_t seen2 = events.size();
    std::this_thread::sleep_for(milliseconds(30));
    writeFile(f.core / "src/contract_core/contract_def.h", "this is not c++ {{{");
    json broken = events.waitFor("workspace.updated", milliseconds(8000), {}, seen2);
    REQUIRE_FALSE(broken.is_null());
    CHECK(contractOf(broken, 1)["status"] == "ok");
    bool hasError = false;
    for (const json& d : broken["diagnostics"]) hasError = hasError || d["severity"] == "error";
    CHECK(hasError);
}

TEST_CASE("live: nothing is emitted after workspace.close or for a replaced workspace") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    h.ok("workspace.open", f.request());
    h.ok("workspace.close");
    const std::size_t n = events.size();
    appendBytes(f.state / "contract0002.005", patternBytes(8, 3));
    writeFile(f.core / "src/contracts/B.h", fakeHeaderB(true));
    std::this_thread::sleep_for(milliseconds(700));
    CHECK(events.size() == n);

    // replaced: the old workspace's directory is no longer watched
    TempDir otherCore, otherState;
    writeFakeCore(otherCore.path());
    writeFakeState(otherState.path());
    h.ok("workspace.open", f.request());
    h.ok("workspace.open", {{"coreDir", otherCore.str()}, {"stateDir", otherState.str()}});
    const std::size_t m = events.size();
    appendBytes(f.state / "contract0002.005", patternBytes(8, 5));
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(events.size() == m);
    appendBytes(otherState / "contract0002.005", patternBytes(8, 5));
    CHECK_FALSE(events.waitFor("contracts.changed").is_null());
}

TEST_CASE("concurrent calls while the workspace is reloaded and files change") {
    Harness h;
    Fixture f;
    h.ok("workspace.open", f.request());
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<long> calls{0};
    auto reader = [&](int seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        while (!stop.load()) {
            json r;
            switch (rng() % 7) {
            case 0: r = h.call("state.node", {{"contract", 1}, {"id", ""}}); break;
            case 1: r = h.call("state.children", {{"contract", 1}, {"id", ""}}); break;
            case 2: r = h.call("state.bytes", {{"contract", 2}, {"offset", 0}, {"length", 8}}); break;
            case 3: r = h.call("state.search", {{"contract", 1}, {"query", "0x2a00000000000000"}}); break;
            case 4: r = h.call("workspace.get"); break;
            case 5: r = h.call("state.digest", {{"contract", 1}}); break;
            default: r = h.call("state.locate", {{"contract", 1}, {"offset", 320}}); break;
            }
            ++calls;
            if (r.contains("error")) {
                // while a reload swaps the workspace every call must still succeed (snapshots), except for
                // not_found for a contract whose status flipped
                const std::string code = r["error"]["code"];
                if (code != "not_found") ++failures;
            }
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) threads.emplace_back(reader, i);
    std::thread writer([&] {
        for (int i = 0; i < 6 && !stop.load(); ++i) {
            appendBytes(f.state / "contract0002.005", patternBytes(1, 1));
            std::this_thread::sleep_for(milliseconds(40));
        }
    });
    for (int i = 0; i < 15; ++i) {
        h.ok("workspace.reload");
        if (i % 5 == 4) h.ok("workspace.open", f.request());
    }
    stop = true;
    writer.join();
    for (auto& t : threads) t.join();
    CHECK(failures.load() == 0);
    CHECK(calls.load() > 50);
}

TEST_CASE("a newer open cancels a running one") {
    Harness h;
    Fixture f;
    h.ok("workspace.open", f.request());
    // Hammer opens from two threads: whichever finishes last wins, the others either succeed or report
    // "cancelled"; the current workspace is always one of the requested ones.
    TempDir core2, state2;
    writeFakeCore(core2.path());
    writeFakeState(state2.path());
    std::atomic<int> unexpected{0};
    auto opener = [&](const json& req) {
        for (int i = 0; i < 20; ++i) {
            json r = h.call("workspace.open", req);
            if (r.contains("error") && r["error"]["message"] != "cancelled") ++unexpected;
        }
    };
    std::thread a(opener, f.request());
    std::thread b(opener, json{{"coreDir", core2.str()}, {"stateDir", state2.str()}});
    a.join();
    b.join();
    CHECK(unexpected.load() == 0);
    json cur = h.ok("workspace.get");
    CHECK((cur["request"]["coreDir"] == f.core.str() || cur["request"]["coreDir"] == core2.str()));
}

TEST_CASE("events stop when the Service object is destroyed; the bus may die first") {
    Fixture f;
    rpc::Dispatcher dispatcher(2);
    {
        rpc::EventBus bus;
        service::ServiceConfig config;
        config.watcher.pollInterval = milliseconds(20);
        config.watcher.settle = milliseconds(60);
        config.settingsPath = (f.state / "settings.json").string();
        {
            service::Service svc(config);
            svc.registerAll(dispatcher, bus);
            json r = dispatcher.dispatch("workspace.open", f.request(), "http");
            REQUIRE(r.contains("result"));
            EventCollector events(bus);
            appendBytes(f.state / "contract0002.005", patternBytes(8, 3));
            CHECK_FALSE(events.waitFor("contracts.changed").is_null());
        }
        // Service gone, bus still alive: no more events, handlers still work.
        EventCollector after(bus);
        appendBytes(f.state / "contract0002.005", patternBytes(8, 4));
        std::this_thread::sleep_for(milliseconds(400));
        CHECK(after.size() == 0);
        CHECK(dispatcher.dispatch("workspace.get", json::object()).contains("result"));
    } // bus destroyed while the dispatcher (and the manager inside its handlers) lives on
    appendBytes(f.state / "contract0002.005", patternBytes(8, 5));
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(dispatcher.dispatch("state.bytes", {{"contract", 2}, {"offset", 0}, {"length", 4}}).contains("result"));
}
