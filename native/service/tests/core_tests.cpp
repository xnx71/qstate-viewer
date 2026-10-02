// core.sync / core.commits against a local git repository (a plain path is a valid repository URL: no network).
#include "fixtures.h"

#include <doctest/doctest.h>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;

namespace {

std::vector<std::string> refs(const json& list) {
    std::vector<std::string> out;
    for (const json& v : list) out.push_back(v["ref"]);
    return out;
}

json find(const json& list, const std::string& ref) {
    for (const json& v : list) {
        if (v["ref"] == ref) return v;
    }
    return json();
}

} // namespace

TEST_CASE("core.sync clones, lists tags and branches with version and epoch, reports progress") {
    Harness h;
    const FakeCoreRepo& core = sharedCore();
    EventCollector events(h.bus);

    json repo = h.ok("core.sync", {{"repoUrl", core.url()}});
    CHECK(repo["repoUrl"] == core.url());
    CHECK(repo["defaultBranch"] == "main");
    const std::string fetchedAt = repo["fetchedAt"];
    CHECK(fetchedAt.size() == 20);  // 2026-01-01T00:00:00Z
    CHECK(fetchedAt[10] == 'T');
    CHECK(fetchedAt.back() == 'Z');

    // tags: newest first, each with version / epoch / date / sha
    CHECK(refs(repo["tags"]) == std::vector<std::string>{"v9.0.0", "v5.0.1", "v5.0.0"});
    const json t = find(repo["tags"], "v5.0.1");
    CHECK(t["kind"] == "tag");
    CHECK(t["sha"] == core.shaV501);  // the annotated tag is peeled to its commit
    CHECK(t["version"] == "1.2.3");
    CHECK(t["epoch"] == 5);
    CHECK(t["date"].is_string());
    CHECK(find(repo["tags"], "v9.0.0")["epoch"] == 9);
    CHECK(find(repo["tags"], "v5.0.0")["sha"] == core.shaV500);

    // branches: the default branch first
    CHECK(refs(repo["branches"]) == std::vector<std::string>{"main", "dev"});
    CHECK(repo["branches"][0]["kind"] == "branch");
    CHECK(repo["branches"][0]["sha"] == core.shaV900);
    CHECK(repo["branches"][0]["epoch"] == 9);
    CHECK(find(repo["branches"], "dev")["epoch"] == 7);

    // the first call cloned: progress events of phase "clone" ending at 100 %
    const json first = events.waitFor("core.progress", std::chrono::milliseconds(2000), [](const json& p) { return p["phase"] == "clone"; });
    REQUIRE_FALSE(first.is_null());
    CHECK(first["message"].is_string());
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(2000),
                         [](const json& p) { return p["phase"] == "clone" && p.value("percent", -1) == 100; })
              .is_object());

    // a second call fetches
    const std::size_t seen = events.size();
    json again = h.ok("core.sync", {{"repoUrl", core.url()}});
    CHECK(again["tags"] == repo["tags"]);
    CHECK(again["branches"] == repo["branches"]);
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(2000), [](const json& p) { return p["phase"] == "fetch"; }, seen).is_object());
    CHECK(events.count("core.progress") > 0);
}

TEST_CASE("core.sync picks up new upstream tags and drops deleted ones") {
    Harness h;
    TempDir dir;
    FakeCoreRepo core(dir.path() / "origin");
    CHECK(refs(h.ok("core.sync", {{"repoUrl", core.url()}})["tags"]).size() == 3);

    writeFakeCore(core.git.dir(), 10);
    core.git.commit("epoch 10");
    core.git.tag("v10.0.0");
    core.git.git({"tag", "-d", "v5.0.0"});
    core.git.branch("topic");
    json repo = h.ok("core.sync", {{"repoUrl", core.url()}});
    CHECK(refs(repo["tags"]) == std::vector<std::string>{"v10.0.0", "v9.0.0", "v5.0.1"});
    CHECK(repo["tags"][0]["epoch"] == 10);
    CHECK(refs(repo["branches"]).size() == 3);
}

TEST_CASE("core.sync offline and with an unreachable repository") {
    Harness h;
    TempDir dir;
    FakeCoreRepo core(dir.path() / "origin");

    // no mirror yet: offline cannot invent one
    CHECK(h.errorCode("core.sync", {{"repoUrl", core.url()}, {"offline", true}}) == "io_error");
    json online = h.ok("core.sync", {{"repoUrl", core.url()}});
    json offline = h.ok("core.sync", {{"repoUrl", core.url()}, {"offline", true}});
    CHECK(offline == online);

    // the repository vanishes: the mirror still answers (offline, and online as a stale mirror)
    fs::rename(core.git.dir(), dir.path() / "gone");
    CHECK(h.ok("core.sync", {{"repoUrl", core.url()}, {"offline", true}}) == online);
    CHECK(h.ok("core.sync", {{"repoUrl", core.url()}}) == online);
    CHECK(h.ok("core.sync", {{"repoUrl", core.url()}})["fetchedAt"] == online["fetchedAt"]);  // nothing was fetched

    // an unknown repository without a mirror is an error
    CHECK(h.errorCode("core.sync", {{"repoUrl", (dir / "never-existed").string()}}) == "io_error");
    CHECK(h.errorCode("core.sync", {{"repoUrl", "file:///nonexistent/repo"}}) == "io_error");
}

TEST_CASE("core.sync refuses unsafe or malformed repository URLs") {
    Harness h;
    TempDir dir;
    const auto pwned = dir / "pwned";
    CHECK(h.errorCode("core.sync", json::object()) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", 5}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", ""}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", "   "}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", "-x"}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", "--upload-pack=touch " + pwned.string()}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", "ext::sh -c touch% " + pwned.string()}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", "https://example.org/a\nb"}}) == "invalid_params");
    CHECK(h.errorCode("core.sync", {{"repoUrl", sharedCore().url()}, {"offline", "yes"}}) == "invalid_params");
    CHECK_FALSE(fs::exists(pwned));
    CHECK(h.errorCode("core.commits", {{"repoUrl", "-x"}, {"ref", "main"}}) == "invalid_params");
    CHECK(h.errorCode("workspace.open", {{"core", {{"repoUrl", "-x"}, {"ref", "main"}}}, {"statePath", dir.str()}}) == "invalid_params");
}

TEST_CASE("core.sync without git is an io_error") {
    Harness h([](service::ServiceConfig& c) { c.git.executable = "definitely-not-a-git-binary"; });
    CHECK(h.errorCode("core.sync", {{"repoUrl", sharedCore().url()}}) == "io_error");
    CHECK(h.errorCode("core.commits", {{"repoUrl", sharedCore().url()}, {"ref", "main"}}) == "io_error");
    TempDir state;
    writeFakeState(state.path());
    json r = h.call("workspace.open", {{"core", {{"repoUrl", sharedCore().url()}, {"ref", "auto"}}}, {"statePath", state.str()}});
    REQUIRE(r.contains("error"));
    CHECK(r["error"]["code"] == "io_error");
    CHECK(r["error"]["message"].get<std::string>().find("git") != std::string::npos);
}

TEST_CASE("core.sync honours cancellation and leaves no mirror behind") {
    Harness h;
    auto token = std::make_shared<rpc::CancelToken>();
    token->cancel();
    rpc::CallContext ctx("core.sync", token);
    json r = h.dispatcher.dispatch("core.sync", {{"repoUrl", sharedCore().url()}}, ctx);
    REQUIRE(r.contains("error"));
    CHECK(r["error"]["message"] == "cancelled");
    CHECK_FALSE(fs::exists(h.scratch.path() / "cache" / "repos"));
}

TEST_CASE("core.commits") {
    Harness h;
    const FakeCoreRepo& core = sharedCore();
    // the mirror has to exist
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}}) == "io_error");
    h.ok("core.sync", {{"repoUrl", core.url()}});

    json all = h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}});
    CHECK(all["total"] == 3);
    REQUIRE(all["commits"].size() == 3);
    const json top = all["commits"][0];
    CHECK(top["ref"] == core.shaV900);
    CHECK(top["kind"] == "commit");
    CHECK(top["sha"] == core.shaV900);
    CHECK(top["subject"] == "core epoch 9");
    CHECK(top["epoch"] == 9);
    CHECK(top["version"] == "1.2.3");
    CHECK(top["date"].is_string());
    CHECK(all["commits"][2]["sha"] == core.shaV500);
    CHECK(all["commits"][2]["epoch"] == 5);

    // paging
    json page = h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"limit", 1}, {"skip", 1}});
    CHECK(page["total"] == 3);
    REQUIRE(page["commits"].size() == 1);
    CHECK(page["commits"][0]["sha"] == core.shaV501);

    // refs: tag, branch, full and abbreviated sha
    CHECK(h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "v5.0.1"}})["commits"].size() == 2);
    CHECK(h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "dev"}})["commits"][0]["epoch"] == 7);
    CHECK(h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", core.shaV500}})["commits"].size() == 1);
    CHECK(h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", core.shaV501.substr(0, 9)}})["commits"].size() == 2);

    // search over subject and sha
    json bySubject = h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"search", "PATCH"}});
    REQUIRE(bySubject["commits"].size() == 1);
    CHECK(bySubject["commits"][0]["sha"] == core.shaV501);
    CHECK(bySubject["total"] == 1);
    json bySha = h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"search", core.shaV500.substr(0, 8)}});
    REQUIRE(bySha["commits"].size() == 1);
    CHECK(bySha["commits"][0]["sha"] == core.shaV500);
    CHECK(h.ok("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"search", "no such text"}})["commits"].empty());

    // errors
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "nope"}}) == "invalid_params");
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "main~1"}}) == "invalid_params");
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "--all"}}) == "invalid_params");
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}}) == "invalid_params");
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"limit", 0}}) == "invalid_params");
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"limit", 1001}}) == "invalid_params");
    CHECK(h.errorCode("core.commits", {{"repoUrl", core.url()}, {"ref", "main"}, {"skip", -1}}) == "invalid_params");
}
