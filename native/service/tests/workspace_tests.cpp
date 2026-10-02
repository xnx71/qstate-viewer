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
    CHECK(ws["request"] == f.request());
    CHECK(ws["core"]["repoUrl"] == f.core.url());
    CHECK(ws["core"]["ref"] == "v5.0.0");
    CHECK(ws["core"]["kind"] == "tag");
    CHECK(ws["core"]["sha"] == f.core.shaV500);
    CHECK(ws["core"]["version"] == "1.2.3");
    CHECK(ws["core"]["epoch"] == 5);
    CHECK(ws["core"]["fileCount"].get<int>() >= 2);
    CHECK(ws["core"]["parseMs"].get<double>() >= 0);
    CHECK_FALSE(ws["core"].contains("dir"));
    CHECK_FALSE(ws["core"].contains("sourceDir"));
    CHECK(ws["state"]["dir"] == f.state.str());
    CHECK(ws["state"]["scope"] == "dir");
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

    // workspace.get returns the same thing; settings keep the recent workspace (new shape)
    CHECK(h.ok("workspace.get") == ws);
    json recent = h.ok("settings.get")["recentWorkspaces"];
    REQUIRE(recent.size() == 1);
    CHECK(recent[0] == f.request());

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

TEST_CASE("workspace.open normalises the request it stores") {
    Harness h;
    Fixture f;
    json request = f.request();
    request["core"]["repoUrl"] = "  " + f.core.url() + "  ";
    request["core"]["ref"] = " v5.0.0 ";
    request["statePath"] = f.state.str() + "/./";
    json ws = h.ok("workspace.open", request);
    CHECK(ws["request"] == f.request());
    // two requests that differ only in the ref are two recent entries; the same request twice is one
    h.ok("workspace.open", f.request("v5.0.1"));
    h.ok("workspace.open", f.request("v5.0.1"));
    json recent = h.ok("settings.get")["recentWorkspaces"];
    REQUIRE(recent.size() == 2);
    CHECK(recent[0]["core"]["ref"] == "v5.0.1");
    CHECK(recent[1]["core"]["ref"] == "v5.0.0");
}

TEST_CASE("recent workspaces of an earlier settings format are dropped, not fatal") {
    TempDir scratch;
    {
        std::ofstream out(scratch / "settings.json");
        out << R"({"version":1,"theme":"dark","recentWorkspaces":[{"coreDir":"/core","coreRef":"auto","stateDir":"/state"},
                   {"core":{"repoUrl":"https://example.org/x","ref":"auto"},"statePath":"/kept"}],"ui":{"a":1}})";
    }
    Harness h([&](service::ServiceConfig& c) { c.settingsPath = (scratch / "settings.json").string(); });
    json s = h.ok("settings.get");
    CHECK(s["theme"] == "dark");
    CHECK(s["ui"]["a"] == 1);
    REQUIRE(s["recentWorkspaces"].size() == 1);
    CHECK(s["recentWorkspaces"][0]["statePath"] == "/kept");
    Fixture f;
    h.ok("workspace.open", f.request());
    s = h.ok("settings.get");
    REQUIRE(s["recentWorkspaces"].size() == 2);
    CHECK(s["recentWorkspaces"][0] == f.request());
}

TEST_CASE("every kind of ref: tag, branch, commit, auto") {
    Harness h;
    Fixture f;
    const FakeCoreRepo& core = f.core;
    json ws = h.ok("workspace.open", f.request("v5.0.1"));
    CHECK(ws["core"]["ref"] == "v5.0.1");
    CHECK(ws["core"]["kind"] == "tag");
    CHECK(ws["core"]["sha"] == core.shaV501);

    ws = h.ok("workspace.open", f.request("main"));
    CHECK(ws["request"]["core"]["ref"] == "main");
    CHECK(ws["core"]["ref"] == "main");
    CHECK(ws["core"]["kind"] == "branch");
    CHECK(ws["core"]["sha"] == core.shaV900);
    CHECK(ws["core"]["epoch"] == 9);
    CHECK(contractOf(ws, 2)["expectedSize"] == 16);  // the head of main has the second field

    ws = h.ok("workspace.open", f.request("dev"));
    CHECK(ws["core"]["kind"] == "branch");
    CHECK(ws["core"]["epoch"] == 7);
    CHECK(contractOf(ws, 2)["expectedSize"] == 8);

    ws = h.ok("workspace.open", f.request(core.shaV500));
    CHECK(ws["core"]["kind"] == "commit");
    CHECK(ws["core"]["ref"] == core.shaV500);
    CHECK(ws["core"]["sha"] == core.shaV500);
    ws = h.ok("workspace.open", f.request(core.shaV501.substr(0, 10)));  // abbreviated: resolved to the full sha
    CHECK(ws["core"]["kind"] == "commit");
    CHECK(ws["core"]["ref"] == core.shaV501);
    CHECK(ws["request"]["core"]["ref"] == core.shaV501.substr(0, 10));  // the request is kept as typed

    // auto: the newest tag whose EPOCH equals the state epoch
    ws = h.ok("workspace.open", f.request("auto"));
    CHECK(ws["request"]["core"]["ref"] == "auto");
    CHECK(ws["core"]["ref"] == "v5.0.1");  // v5.0.0 and v5.0.1 are both epoch 5: the newer one
    CHECK(ws["core"]["kind"] == "tag");
    CHECK_FALSE(hasDiagnostic(ws, "warning", "auto"));
    TempDir state9, state4, empty;
    writeFakeState(state9.path(), 9);
    ws = h.ok("workspace.open", Fixture::requestFor(core.url(), "auto", state9.str()));
    CHECK(ws["core"]["ref"] == "v9.0.0");
    CHECK(ws["state"]["epoch"] == 9);
    CHECK(contractOf(ws, 2)["expectedSize"] == 16);
    // no tag of that epoch: the head of the default branch, with a warning
    writeFakeState(state4.path(), 4);
    ws = h.ok("workspace.open", Fixture::requestFor(core.url(), "auto", state4.str()));
    CHECK(ws["core"]["ref"] == "main");
    CHECK(ws["core"]["kind"] == "branch");
    CHECK(hasDiagnostic(ws, "warning", "no tag with #define EPOCH 4"));
    CHECK(hasDiagnostic(ws, "warning", "epoch 9"));  // and the usual epoch mismatch hint
    // no state files at all: the epoch is unknown
    ws = h.ok("workspace.open", Fixture::requestFor(core.url(), "auto", empty.str()));
    CHECK(ws["core"]["ref"] == "main");
    CHECK(hasDiagnostic(ws, "warning", "epoch of the state files is unknown"));
    CHECK(ws["contracts"].size() == 3);
    CHECK(contractOf(ws, 1)["status"] == "missing-file");
}

TEST_CASE("workspace.open reads the local mirror and fetches only when it must") {
    Harness h;
    TempDir dir, state;
    FakeCoreRepo core(dir.path() / "origin");
    writeFakeState(state.path());
    auto open = [&](const std::string& ref) { return h.call("workspace.open", Fixture::requestFor(core.url(), ref, state.str())); };

    EventCollector events(h.bus);
    json ws = open("main")["result"];
    CHECK(ws["core"]["sha"] == core.shaV900);
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(1000), [](const json& p) { return p["phase"] == "clone"; }).is_object());

    // upstream moves: an open of a branch does not touch the network ...
    writeFakeCore(core.git.dir(), 11);
    const std::string newHead = core.git.commit("epoch 11");
    core.git.tag("v11.0.0");
    const std::size_t seen = events.size();
    CHECK(open("main")["result"]["core"]["sha"] == core.shaV900);
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(200), [](const json& p) { return p["phase"] == "fetch"; }, seen).is_null());
    // ... a ref the mirror does not know yet triggers one fetch
    json byTag = open("v11.0.0")["result"];
    CHECK(byTag["core"]["sha"] == newHead);
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(2000), [](const json& p) { return p["phase"] == "fetch"; }, seen).is_object());
    // (and the branch is current now)
    CHECK(open("main")["result"]["core"]["sha"] == newHead);

    // "auto" for an epoch that only the upstream has tags for
    writeFakeCore(core.git.dir(), 12);
    core.git.commit("epoch 12");
    core.git.tag("v12.0.0");
    TempDir state12;
    writeFakeState(state12.path(), 12);
    json auto12 = h.ok("workspace.open", Fixture::requestFor(core.url(), "auto", state12.str()));
    CHECK(auto12["core"]["ref"] == "v12.0.0");
    CHECK(auto12["core"]["kind"] == "tag");

    // a ref that does not exist anywhere is invalid_params, also after the fetch
    json r = open("v99.0.0");
    REQUIRE(r.contains("error"));
    CHECK(r["error"]["code"] == "invalid_params");
    CHECK(r["error"]["message"].get<std::string>().find("v99.0.0") != std::string::npos);

    // the upstream is gone: refs the mirror knows keep working, unknown ones fail cleanly
    fs::rename(core.git.dir(), dir.path() / "gone");
    CHECK(open("v5.0.0").contains("result"));
    CHECK(open("v77.0.0")["error"]["code"] == "invalid_params");
}

TEST_CASE("workspace.open reports its progress") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    h.ok("workspace.open", f.request());
    for (const std::string phase : {"clone", "export", "parse"}) {
        CHECK_MESSAGE(events.waitFor("core.progress", std::chrono::milliseconds(1000), [&](const json& p) { return p["phase"] == phase; }).is_object(), phase);
    }
    // a second open of the same ref neither clones nor fetches
    const std::size_t seen = events.size();
    h.ok("workspace.open", f.request());
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(1000), [](const json& p) { return p["phase"] == "parse"; }, seen).is_object());
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(100), [](const json& p) { return p["phase"] == "clone" || p["phase"] == "fetch"; }, seen).is_null());
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
    Fixture f;
    writeFakeState(f.state.path(), 5);
    // the files are epoch 5; v9.0.0 is a core of epoch 9 in which contract 2 has a second field
    json ws = h.ok("workspace.open", f.request("v9.0.0"));
    const json c2 = contractOf(ws, 2);
    CHECK(c2["status"] == "size-mismatch");
    const std::string msg = c2["statusMessage"];
    CHECK(msg.find("epoch 5") != std::string::npos);
    CHECK(msg.find("epoch 9") != std::string::npos);
    CHECK(msg.find("v9.0.0") != std::string::npos);
    CHECK(msg.find("ref \"auto\"") != std::string::npos);
    CHECK(hasDiagnostic(ws, "warning", "epoch 9"));
    // with the matching version everything is fine
    CHECK(contractOf(h.ok("workspace.open", f.request("auto")), 2)["status"] == "ok");
}

TEST_CASE("epoch selection") {
    Harness h;
    Fixture f;
    writeFakeState(f.state.path(), 4);
    json ws = h.ok("workspace.open", f.request());
    CHECK(ws["state"]["epoch"] == 5);
    CHECK(ws["state"]["epochsAvailable"] == json::array({4, 5}));
    CHECK(contractOf(ws, 1)["file"]["name"] == "contract0001.005");
    json request = f.request();
    request["epoch"] = 4;
    ws = h.ok("workspace.open", request);
    CHECK(ws["state"]["epoch"] == 4);
    CHECK(ws["request"]["epoch"] == 4);
    CHECK(contractOf(ws, 1)["file"]["name"] == "contract0001.004");
    // an epoch without files: no error, all missing, with a diagnostic
    request["epoch"] = 7;
    ws = h.ok("workspace.open", request);
    CHECK(ws["state"]["epoch"] == 7);
    CHECK(contractOf(ws, 1)["status"] == "missing-file");
    CHECK(hasDiagnostic(ws, "warning", "epoch 7"));
}

TEST_CASE("workspace.open reports bad input") {
    Harness h;
    Fixture f;
    TempDir other;
    auto with = [&](const std::function<void(json&)>& edit) {
        json r = f.request();
        edit(r);
        return h.errorCode("workspace.open", r);
    };
    CHECK(h.errorCode("workspace.open", json::object()) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"statePath", f.state.str()}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"core", {{"repoUrl", f.core.url()}, {"ref", "auto"}}}}) == "invalid_params");
    CHECK(with([](json& r) { r["core"] = 5; }) == "invalid_params");
    CHECK(with([](json& r) { r["core"].erase("repoUrl"); }) == "invalid_params");
    CHECK(with([](json& r) { r["core"].erase("ref"); }) == "invalid_params");
    CHECK(with([](json& r) { r["core"]["repoUrl"] = ""; }) == "invalid_params");
    CHECK(with([](json& r) { r["core"]["ref"] = ""; }) == "invalid_params");
    CHECK(with([](json& r) { r["core"]["ref"] = "-x"; }) == "invalid_params");
    CHECK(with([](json& r) { r["core"]["ref"] = "main~1"; }) == "invalid_params");
    CHECK(with([](json& r) { r["core"]["ref"] = "no-such-ref"; }) == "invalid_params");
    CHECK(with([](json& r) { r["statePath"] = ""; }) == "invalid_params");
    CHECK(with([](json& r) { r["statePath"] = 5; }) == "invalid_params");
    CHECK(with([](json& r) { r["epoch"] = "x"; }) == "invalid_params");
    CHECK(with([](json& r) { r["epoch"] = -1; }) == "invalid_params");
    CHECK(with([](json& r) { r["defines"] = 5; }) == "invalid_params");
    CHECK(with([](json& r) { r["defines"] = json::array({5}); }) == "invalid_params");
    // paths: a missing state path is an io_error, a file that is not a state file is invalid
    CHECK(with([&](json& r) { r["statePath"] = (f.state / "nope").string(); }) == "io_error");
    CHECK(with([&](json& r) { r["statePath"] = (f.state / "contract0009.005").string(); }) == "io_error");
    writeFile(other / "notes.txt", "x");
    CHECK(with([&](json& r) { r["statePath"] = (other / "notes.txt").string(); }) == "invalid_params");
    writeFile(other / "contract1.005", "x");  // wrong number of digits
    CHECK(with([&](json& r) { r["statePath"] = (other / "contract1.005").string(); }) == "invalid_params");
    // repositories: nothing there, not a git repository, not a core repository
    CHECK(with([&](json& r) { r["core"]["repoUrl"] = (other / "nope").string(); }) == "io_error");
    CHECK(with([&](json& r) { r["core"]["repoUrl"] = other.str(); }) == "io_error");  // exists, but is no repository
    {
        testing::GitFixtureRepo plain(other.path() / "plain");
        plain.write("README", "hello\n");
        plain.commit("readme");
        json r = f.request("main");
        r["core"]["repoUrl"] = plain.url();
        json resp = h.call("workspace.open", r);
        REQUIRE(resp.contains("error"));
        CHECK(resp["error"]["code"] == "schema_error");
        CHECK(resp["error"]["message"].get<std::string>().find("contract_def.h") != std::string::npos);
    }
    // a failed open leaves no workspace behind
    CHECK(h.ok("workspace.get").is_null());
    CHECK(h.ok("settings.get")["recentWorkspaces"].empty());
}

TEST_CASE("defines are passed to the preprocessor") {
    Harness h;
    TempDir dir, state;
    testing::GitFixtureRepo repo(dir.path() / "core");
    repo.write("src/contract_core/contract_def.h", R"(
struct A { struct StateData {
#ifdef EXTRA
    unsigned long long extra;
#endif
    unsigned long long x; }; };
constexpr struct ContractDescription { char assetName[8]; unsigned short constructionEpoch, destructionEpoch; unsigned long long stateSize; }
contractDescriptions[] = { {"", 0, 0, sizeof(A::StateData)}, };
)");
    repo.commit("core");
    repo.tag("v1");
    auto request = [&](const json& defines) {
        json r = Fixture::requestFor(repo.url(), "v1", state.str());
        if (!defines.is_null()) r["defines"] = defines;
        return r;
    };
    json plain = h.ok("workspace.open", request(nullptr));
    CHECK(contractOf(plain, 0)["expectedSize"] == 8);
    json extra = h.ok("workspace.open", request(json::array({"EXTRA"})));
    CHECK(contractOf(extra, 0)["expectedSize"] == 16);
    CHECK(extra["request"]["defines"] == json::array({"EXTRA"}));
    json valued = h.ok("workspace.open", request(json::array({"EXTRA=1"})));
    CHECK(contractOf(valued, 0)["expectedSize"] == 16);
}

TEST_CASE("single state file: only that contract is shown") {
    Harness h;
    Fixture f;
    writeFile(f.state / "spectrum.005", "abc");
    const std::string file = (f.state / "contract0001.005").string();
    json ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", file));
    CHECK(ws["request"]["statePath"] == file);
    CHECK(ws["state"]["scope"] == "file");
    CHECK(ws["state"]["dir"] == f.state.str());
    CHECK(ws["state"]["epoch"] == 5);
    CHECK(ws["state"]["epochsAvailable"] == json::array({5}));
    CHECK(ws["state"]["otherFiles"].empty());
    REQUIRE(ws["contracts"].size() == 1);
    CHECK(ws["contracts"][0]["index"] == 1);
    CHECK(ws["contracts"][0]["name"] == "AA");
    CHECK(ws["contracts"][0]["status"] == "ok");
    CHECK(ws["contracts"][0]["file"]["path"] == file);
    CHECK_FALSE(hasDiagnostic(ws, "warning", "gaps"));  // a single file has no gaps
    CHECK(h.ok("state.node", {{"contract", 1}, {"id", ""}})["size"] == 328);
    CHECK(h.ok("state.bytes", {{"contract", 1}, {"offset", 32}, {"length", 8}})["hex"] == "2a00000000000000");
    CHECK(h.errorCode("state.node", {{"contract", 2}, {"id", ""}}) == "not_found");  // the neighbours are not part of it
    CHECK(h.errorCode("state.bytes", {{"contract", 0}, {"offset", 0}, {"length", 8}}) == "not_found");
    CHECK(h.ok("settings.get")["recentWorkspaces"][0]["statePath"] == file);

    // the epoch comes from the file name; a requested epoch is ignored
    json request = Fixture::requestFor(f.core.url(), "auto", file);
    request["epoch"] = 4;
    ws = h.ok("workspace.open", request);
    CHECK(ws["state"]["epoch"] == 5);
    CHECK(ws["core"]["ref"] == "v5.0.1");
    // a mismatching file is shown like in a directory
    writeBytes(f.state / "contract0002.005", patternBytes(16, 1));
    ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", (f.state / "contract0002.005").string()));
    REQUIRE(ws["contracts"].size() == 1);
    CHECK(ws["contracts"][0]["status"] == "size-mismatch");
    // a contract the sources do not know
    writeBytes(f.state / "contract0007.005", patternBytes(4, 1));
    ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", (f.state / "contract0007.005").string()));
    REQUIRE(ws["contracts"].size() == 1);
    CHECK(ws["contracts"][0]["index"] == 7);
    CHECK(ws["contracts"][0]["status"] == "unknown-contract");
    // a file of another epoch than the core: the usual hint
    writeFakeState(f.state.path(), 9);
    ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", (f.state / "contract0002.009").string()));
    CHECK(ws["state"]["epoch"] == 9);
    CHECK(hasDiagnostic(ws, "warning", "epoch 9"));
    // reload keeps the scope
    json reloaded = h.ok("workspace.reload");
    CHECK(reloaded["state"]["scope"] == "file");
    CHECK(reloaded["contracts"].size() == 1);
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
    rpc::CallContext ctx("workspace.open", token);
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
        rpc::CallContext ctx(method, token);
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

TEST_CASE("live: only state files are watched") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    h.ok("workspace.open", f.request());
    writeFile(f.state / "spectrum.005", "abc");
    writeFile(f.state / "notes.txt", "abc");
    fs::create_directories(f.state / "ep5");
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(events.count("contracts.changed") == 0);
    CHECK(events.count("workspace.updated") == 0);
    // while a state file is noticed
    appendBytes(f.state / "contract0002.005", patternBytes(8, 3));
    CHECK_FALSE(events.waitFor("contracts.changed").is_null());
}

TEST_CASE("live: a single state file is watched on its own") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    const std::string file = (f.state / "contract0002.005").string();
    json ws = h.ok("workspace.open", Fixture::requestFor(f.core.url(), "v5.0.0", file));
    const int gen0 = ws["contracts"][0]["generation"];

    // a neighbour changes, appears: nothing happens
    appendBytes(f.state / "contract0001.005", patternBytes(8, 3));
    writeBytes(f.state / "contract0003.005", patternBytes(4, 1));
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(events.count("contracts.changed") == 0);
    CHECK(events.count("workspace.updated") == 0);

    // the file itself changes
    appendBytes(file, patternBytes(8, 3));
    json ev = events.waitFor("contracts.changed");
    REQUIRE_FALSE(ev.is_null());
    REQUIRE(ev["contracts"].size() == 1);
    CHECK(ev["contracts"][0]["index"] == 2);
    CHECK(ev["contracts"][0]["status"] == "size-mismatch");
    CHECK(ev["contracts"][0]["generation"].get<int>() > gen0);

    // it disappears: the workspace stays, the contract is missing
    const std::size_t seen = events.size();
    fs::remove(file);
    json gone = events.waitFor("workspace.updated", milliseconds(8000), {}, seen);
    REQUIRE_FALSE(gone.is_null());
    CHECK(gone["state"]["scope"] == "file");
    REQUIRE(gone["contracts"].size() == 1);
    CHECK(gone["contracts"][0]["status"] == "missing-file");
    CHECK(h.ok("workspace.get")["id"] == gone["id"]);

    // and comes back
    const std::size_t seen2 = events.size();
    writeBytes(file, patternBytes(8, 9));
    json back = events.waitFor("workspace.updated", milliseconds(8000), [](const json& w) { return w["contracts"][0]["status"] == "ok"; }, seen2);
    REQUIRE_FALSE(back.is_null());
    CHECK(back["contracts"][0]["file"]["size"] == 8);
    CHECK(h.ok("state.bytes", {{"contract", 2}, {"offset", 0}, {"length", 8}})["length"] == 8);
}

TEST_CASE("live: nothing is emitted after workspace.close or for a replaced workspace") {
    Harness h;
    Fixture f;
    EventCollector events(h.bus);
    h.ok("workspace.open", f.request());
    h.ok("workspace.close");
    const std::size_t n = events.size();
    appendBytes(f.state / "contract0002.005", patternBytes(8, 3));
    std::this_thread::sleep_for(milliseconds(700));
    CHECK(events.size() == n);

    // replaced: the old workspace's directory is no longer watched
    Fixture other;
    h.ok("workspace.open", f.request());
    h.ok("workspace.open", other.request());
    const std::size_t m = events.size();
    appendBytes(f.state / "contract0002.005", patternBytes(8, 5));
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(events.size() == m);
    appendBytes(other.state / "contract0002.005", patternBytes(8, 5));
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
    Fixture second;
    std::atomic<int> unexpected{0};
    auto opener = [&](const json& req) {
        for (int i = 0; i < 20; ++i) {
            json r = h.call("workspace.open", req);
            if (r.contains("error") && r["error"]["message"] != "cancelled") ++unexpected;
        }
    };
    std::thread a(opener, f.request());
    std::thread b(opener, second.request("v9.0.0"));
    a.join();
    b.join();
    CHECK(unexpected.load() == 0);
    json cur = h.ok("workspace.get");
    CHECK((cur["request"] == f.request() || cur["request"] == second.request("v9.0.0")));
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
        config.cacheDir = (f.state / "cache").string();
        {
            service::Service svc(config);
            svc.registerAll(dispatcher, bus);
            json r = dispatcher.dispatch("workspace.open", f.request());
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
