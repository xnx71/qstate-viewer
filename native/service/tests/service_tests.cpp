#include "fixtures.h"

#include "qstate/service/service.h"

#include <doctest/doctest.h>

#include <fstream>
#include <regex>
#include <sstream>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;

TEST_CASE("app.info") {
    rpc::Dispatcher d;
    rpc::EventBus bus;
    service::Service svc{service::ServiceConfig{}};
    svc.registerAll(d, bus);

    json r = d.dispatch("app.info", json::object());
    REQUIRE(r.contains("result"));
    const json& info = r["result"];
    CHECK(info["name"] == "qstate-viewer");
    CHECK(std::regex_match(info["version"].get<std::string>(), std::regex(R"(\d+\.\d+\.\d+.*)")));
    CHECK(info["platform"] == "linux");
    CHECK(info["transport"] == "webview");
    CHECK(info["pathSeparator"] == "/");
    CHECK(info["gitAvailable"] == true);
    CHECK(info["defaultRepoUrl"] == "https://github.com/qubic/core");
    CHECK(info["homeDir"].is_string());
    CHECK(info["cwd"].is_string());
    CHECK_FALSE(info["cwd"].get<std::string>().empty());
    CHECK_FALSE(info.contains("startup"));
    CHECK(svc.appInfo() == info);
}

TEST_CASE("app.info reports a missing git") {
    rpc::Dispatcher d;
    rpc::EventBus bus;
    service::ServiceConfig config;
    config.git.executable = "definitely-not-a-git-binary";
    service::Service svc(config);
    svc.registerAll(d, bus);
    CHECK(d.dispatch("app.info", json::object())["result"]["gitAvailable"] == false);
}

TEST_CASE("version comes from the CMake project unless overridden") {
    service::ServiceConfig config;
    config.version = "9.9.9";
    service::Service svc(config);
    CHECK(svc.appInfo()["version"] == "9.9.9");
    CHECK(service::Service().appInfo()["version"] != "9.9.9");
}

TEST_CASE("every contract method is registered and implemented") {
    Harness h;
    for (const std::string& method : service::Service::contractMethods()) {
        CHECK_MESSAGE(h.dispatcher.has(method), method);
    }
    // No stub is left: calling any of them without a workspace gives no_workspace / invalid_params, never
    // "not implemented yet".
    for (const std::string& method : service::Service::contractMethods()) {
        json r = h.call(method, json::object());
        if (r.contains("error")) {
            CHECK_MESSAGE(r["error"]["message"].get<std::string>().find("not implemented yet") == std::string::npos, method);
        }
    }
    CHECK(h.call("nope.nope")["error"]["code"] == "unknown_method");
}

TEST_CASE("the method list matches ui/src/rpc/contract.ts") {
    const char* root = std::getenv("QSTATE_SOURCE_DIR");
    if (root == nullptr || *root == '\0') {
        MESSAGE("QSTATE_SOURCE_DIR not set: skipped");
        return;
    }
    std::ifstream in(std::string(root) + "/ui/src/rpc/contract.ts");
    if (!in) {
        MESSAGE("contract.ts not found: skipped");
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    auto begin = text.find("export interface RpcMethods");
    auto end = text.find("export type RpcMethod ", begin);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    std::string block = text.substr(begin, end - begin);
    std::regex re(R"re(\n  "([a-z]+\.[a-zA-Z]+)": \{)re");
    std::vector<std::string> found;
    for (std::sregex_iterator it(block.begin(), block.end(), re), stop; it != stop; ++it) {
        found.push_back((*it)[1]);
    }
    CHECK(found == service::Service::contractMethods());
}

TEST_CASE("handlers keep working after the Service object is gone") {
    rpc::Dispatcher d;
    rpc::EventBus bus;
    {
        service::Service svc;
        svc.registerAll(d, bus);
    }
    CHECK(d.dispatch("app.info", json::object()).contains("result"));
    CHECK(d.dispatch("workspace.get", json::object())["result"].is_null());
}

TEST_CASE("settings.get / settings.update") {
    Harness h;
    json s = h.ok("settings.get");
    CHECK(s["theme"] == "system");
    CHECK(s["recentWorkspaces"].is_array());
    CHECK(s["ui"].is_object());

    s = h.ok("settings.update", {{"patch", {{"theme", "dark"}, {"ui", {{"sidebar", 240}, {"a", 1}}}}}});
    CHECK(s["theme"] == "dark");
    CHECK(s["ui"]["sidebar"] == 240);
    // ui is merged one level deep, null removes a key
    s = h.ok("settings.update", {{"patch", {{"ui", {{"a", nullptr}, {"b", true}}}}}});
    CHECK(s["ui"] == json{{"sidebar", 240}, {"b", true}});
    CHECK(s["theme"] == "dark");
    // persisted: a second service over the same file sees it
    {
        Harness other([&](service::ServiceConfig& c) { c.settingsPath = h.scratch.str() + "/settings.json"; });
        CHECK(other.ok("settings.get")["theme"] == "dark");
    }
    CHECK(h.errorCode("settings.update", json::object()) == "invalid_params");
    CHECK(h.errorCode("settings.update", {{"patch", 5}}) == "invalid_params");
    CHECK(h.errorCode("settings.update", {{"patch", {{"theme", 5}}}}) == "invalid_params");
    // an invalid theme name is ignored, not an error
    CHECK(h.ok("settings.update", {{"patch", {{"theme", "purple"}}}})["theme"] == "dark");
}

TEST_CASE("fs.list") {
    Harness h;
    TempDir t;
    fs::create_directories(t / "Zdir");
    fs::create_directories(t / "adir");
    fs::create_directories(t / ".hidden");
    writeFile(t / "b.txt", "hello");
    writeFile(t / "A.txt", "x");
    writeFakeState(t.path());
    json r = h.ok("fs.list", {{"path", t.str()}});
    CHECK(r["path"] == t.str());
    CHECK(r["parent"] == t.path().parent_path().string());
    std::vector<std::string> names;
    for (const json& e : r["entries"]) names.push_back(e["name"]);
    // directories first, then files, case-insensitive; hidden excluded
    std::vector<std::string> expected = {"adir", "Zdir", "A.txt", "b.txt", "contract0000.005", "contract0001.005", "contract0002.005"};
    CHECK(names == expected);
    CHECK(r["entries"][0]["kind"] == "dir");
    CHECK(r["entries"][2]["kind"] == "file");
    CHECK(r["entries"][3]["size"] == 5);
    CHECK(r["entries"][3]["path"] == (t / "b.txt").string());
    CHECK(r["hints"] == json{{"stateEpochs", json::array({5})}});
    // contractNNNN.EEE files carry the selectable state; nothing else does
    for (const json& e : r["entries"]) {
        const std::string name = e["name"];
        if (name.rfind("contract0001.", 0) == 0) {
            CHECK(e["state"] == json{{"index", 1}, {"epoch", 5}});
        } else if (name.rfind("contract", 0) == 0) {
            CHECK(e["state"]["epoch"] == 5);
        } else {
            CHECK_FALSE(e.contains("state"));
        }
    }
    CHECK(h.ok("fs.list", {{"path", t.str()}, {"showHidden", true}})["entries"].size() == r["entries"].size() + 1);

    // "" = home directory
    json home = h.ok("fs.list", {{"path", ""}});
    CHECK(home["path"].is_string());
    CHECK(h.errorCode("fs.list", {{"path", (t / "nope").string()}}) == "io_error");
    CHECK(h.errorCode("fs.list", {{"path", (t / "b.txt").string()}}) == "io_error");
    CHECK(h.errorCode("fs.list", json::object()) == "invalid_params");
}

