#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <thread>

#include "qstate/support/settings.h"
#include "temp_dir.h"

using namespace qstate::support;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

WorkspaceRequest req(int i) {
    WorkspaceRequest r;
    r.core.repoUrl = "https://example.org/core" + std::to_string(i);
    r.core.ref = "auto";
    r.statePath = "/state" + std::to_string(i);
    return r;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("settings: JSON shapes of contract.ts") {
    Settings s;
    s.theme = "dark";
    s.ui = json{{"sidebar", 240}, {"nested", json{{"a", true}}}};
    WorkspaceRequest r = req(1);
    r.core.ref = "v1.303.2";
    r.epoch = 229;
    r.defines = {"INCLUDE_CONTRACT_TEST_EXAMPLES"};
    s.recentWorkspaces.push_back(r);
    s.recentWorkspaces.push_back(req(2));

    const json j = s;
    CHECK(j["theme"] == "dark");
    CHECK(j["recentWorkspaces"].size() == 2);
    CHECK(j["recentWorkspaces"][0]["core"] == json{{"repoUrl", "https://example.org/core1"}, {"ref", "v1.303.2"}});
    CHECK(j["recentWorkspaces"][0]["statePath"] == "/state1");
    CHECK(j["recentWorkspaces"][0]["epoch"] == 229);
    CHECK(j["recentWorkspaces"][0]["defines"][0] == "INCLUDE_CONTRACT_TEST_EXAMPLES");
    CHECK_FALSE(j["recentWorkspaces"][0].contains("coreDir"));
    CHECK_FALSE(j["recentWorkspaces"][1].contains("coreRef"));
    CHECK_FALSE(j["recentWorkspaces"][1].contains("epoch"));
    CHECK_FALSE(j["recentWorkspaces"][1].contains("defines"));
    CHECK(j["ui"]["sidebar"] == 240);

    const Settings back = j.get<Settings>();
    CHECK(back == s);
}

TEST_CASE("settings: tolerant from_json") {
    const char* old = R"({"coreDir":"/core","coreRef":"auto","stateDir":"/state"})";  // shape of an earlier version
    Settings s = json::parse(std::string(R"({"theme":"blue","recentWorkspaces":[1,)") + old +
                             R"(,{"core":{"repoUrl":"a"},"statePath":"s"},
        {"core":{"repoUrl":"c","ref":"auto"},"statePath":"s","epoch":"x"},
        {"core":{"repoUrl":"c","ref":"auto"},"statePath":"s/"}],"ui":[1,2]})").get<Settings>();
    CHECK(s.theme == "system");
    REQUIRE(s.recentWorkspaces.size() == 1);  // old-shaped and incomplete ones dropped, duplicates (trailing slash) collapsed
    CHECK_FALSE(s.recentWorkspaces[0].epoch.has_value());
    CHECK(s.ui.is_object());
    CHECK(s.ui.empty());

    CHECK(json::parse("[1,2,3]").get<Settings>() == Settings{});
    CHECK(json::parse("null").get<Settings>() == Settings{});

    json many = json::object();
    many["recentWorkspaces"] = json::array();
    for (int i = 0; i < 25; i++) many["recentWorkspaces"].push_back(json(req(i)));
    CHECK(many.get<Settings>().recentWorkspaces.size() == kMaxRecentWorkspaces);
}

TEST_CASE("settings: a file written by an earlier version loads without its old recent workspaces") {
    testutil::TempDir dir;
    const auto file = dir.path() / "settings.json";
    {
        std::ofstream out(file);
        out << R"({"version":1,"theme":"light","recentWorkspaces":[{"coreDir":"/core","stateDir":"/state","coreRef":"auto"}],
                   "ui":{"sidebar":200}})";
    }
    SettingsStore store(file.string());
    const Settings s = store.get();
    CHECK(store.lastError().empty());
    CHECK(s.theme == "light");
    CHECK(s.recentWorkspaces.empty());
    CHECK(s.ui["sidebar"] == 200);
    CHECK(store.addRecentWorkspace(req(1)).recentWorkspaces.size() == 1);  // and it keeps working
}

TEST_CASE("settings: recent workspace list maintenance") {
    std::vector<WorkspaceRequest> list;
    for (int i = 0; i < 12; i++) addRecentWorkspace(list, req(i));
    REQUIRE(list.size() == 10);
    CHECK(list[0] == req(11));
    CHECK(list[9] == req(2));
    WorkspaceRequest again = req(5);
    again.epoch = 230;
    addRecentWorkspace(list, again);
    CHECK(list.size() == 10);
    CHECK(list[0].epoch == std::optional<int>(230));  // moved to the front, newest request wins
    CHECK(std::count_if(list.begin(), list.end(), [](const auto& r) { return r.statePath == "/state5"; }) == 1);
    WorkspaceRequest slash = req(7);
    slash.statePath += "/";
    addRecentWorkspace(list, slash);
    CHECK(list.size() == 10);
    CHECK(list[0].statePath == "/state7/");
    WorkspaceRequest otherRef = req(7);
    otherRef.core.ref = "v1.0.0";  // same state, another core version: a separate entry
    addRecentWorkspace(list, otherRef);
    CHECK(list.size() == 10);
    CHECK(list[1].statePath == "/state7/");
}

TEST_CASE("settings: store round trip at an injected path, creating directories") {
    testutil::TempDir dir;
    const auto file = dir.path() / "nested" / "deeper" / "settings.json";
    {
        SettingsStore store(file.string());
        CHECK(store.path() == file.string());
        CHECK(store.get() == Settings{});  // missing file: defaults
        CHECK(store.lastError().empty());
        CHECK_FALSE(fs::exists(file));      // reading does not create anything
        store.applyPatch(json{{"theme", "light"}, {"ui", json{{"a", 1}}}});
        store.addRecentWorkspace(req(1));
        CHECK(store.lastError().empty());
    }
    REQUIRE(fs::exists(file));
    const json raw = json::parse(slurp(file));
    CHECK(raw["version"] == 1);
    CHECK(raw["theme"] == "light");
    {
        SettingsStore store(file.string());
        const Settings s = store.get();
        CHECK(s.theme == "light");
        CHECK(s.ui["a"] == 1);
        REQUIRE(s.recentWorkspaces.size() == 1);
        CHECK(s.recentWorkspaces[0] == req(1));
    }
    // no stray temporary files
    for (const auto& e : fs::directory_iterator(file.parent_path())) CHECK(e.path().filename() == "settings.json");
}

TEST_CASE("settings: patch semantics") {
    testutil::TempDir dir;
    SettingsStore store((dir.path() / "s.json").string());
    store.applyPatch(json{{"ui", json{{"a", 1}, {"b", 2}}}});
    Settings s = store.applyPatch(json{{"ui", json{{"b", nullptr}, {"c", json{{"d", 3}}}}}, {"theme", "nope"}, {"other", 1}});
    CHECK(s.theme == "system");  // invalid theme ignored
    CHECK(s.ui == json{{"a", 1}, {"c", json{{"d", 3}}}});
    s = store.applyPatch(json{{"theme", "dark"}});
    CHECK(s.theme == "dark");
    CHECK(s.ui.size() == 2);
    s = store.applyPatch(json::array());  // not an object: ignored
    CHECK(s.theme == "dark");
}

TEST_CASE("settings: corrupt and unreadable files") {
    testutil::TempDir dir;
    const auto file = dir.path() / "s.json";
    {
        std::ofstream f(file);
        f << "{ this is not json";
    }
    SettingsStore store(file.string());
    CHECK(store.get() == Settings{});
    CHECK(store.lastError().find("corrupt") != std::string::npos);
    CHECK(fs::exists(dir.path() / "s.json.corrupt"));  // the user's bytes are kept
    CHECK(slurp(dir.path() / "s.json.corrupt") == "{ this is not json");
    // saving replaces the corrupt file and clears the error
    store.applyPatch(json{{"theme", "dark"}});
    CHECK(store.lastError().empty());
    CHECK(SettingsStore(file.string()).get().theme == "dark");

    // a valid JSON file that is not an object
    {
        std::ofstream f(file, std::ios::trunc);
        f << "[1,2]";
    }
    CHECK(SettingsStore(file.string()).load() == Settings{});

    // empty file
    {
        std::ofstream f(file, std::ios::trunc);
    }
    SettingsStore empty(file.string());
    CHECK(empty.load() == Settings{});
    CHECK_FALSE(empty.lastError().empty());

#if !defined(_WIN32)
    if (::geteuid() != 0) {
        // Unwritable directory: set() reports the failure, the in-memory state is still updated.
        const auto ro = dir.path() / "ro";
        fs::create_directories(ro);
        fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec);
        SettingsStore rs((ro / "s.json").string());
        Settings t;
        t.theme = "dark";
        CHECK_FALSE(rs.set(t));
        CHECK_FALSE(rs.lastError().empty());
        CHECK(rs.get().theme == "dark");
        fs::permissions(ro, fs::perms::owner_all);

        // Unreadable file
        const auto locked = dir.path() / "locked.json";
        {
            std::ofstream f(locked);
            f << R"({"theme":"dark"})";
        }
        fs::permissions(locked, fs::perms::none);
        SettingsStore ls(locked.string());
        CHECK(ls.get() == Settings{});
        CHECK_FALSE(ls.lastError().empty());
        fs::permissions(locked, fs::perms::owner_all);
    }
#endif

    // parent "directory" is a file
    SettingsStore blocked((file / "child" / "s.json").string());
    Settings t;
    CHECK_FALSE(blocked.set(t));
    CHECK_FALSE(blocked.lastError().empty());
}

TEST_CASE("settings: default path follows XDG_CONFIG_HOME") {
#if defined(__linux__)
    const char* old = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = old ? old : "";
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-test-config", 1);
    CHECK(SettingsStore::defaultPath() == "/tmp/xdg-test-config/qstate-viewer/settings.json");
    unsetenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (home) CHECK(SettingsStore::defaultPath() == std::string(home) + "/.config/qstate-viewer/settings.json");
    if (old) setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
#endif
}

TEST_CASE("settings: concurrent updates never tear the file") {
    testutil::TempDir dir;
    const auto file = dir.path() / "s.json";
    SettingsStore store(file.string());
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 25; i++) store.addRecentWorkspace(req(t * 100 + i));
        });
    }
    for (auto& th : threads) th.join();
    CHECK(store.get().recentWorkspaces.size() == kMaxRecentWorkspaces);
    const Settings reread = SettingsStore(file.string()).load();
    CHECK(reread.recentWorkspaces.size() == kMaxRecentWorkspaces);
    CHECK(reread == store.get());
}
