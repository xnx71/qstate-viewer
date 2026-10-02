// The real flow against https://github.com/qubic/core. Opt in with QSTATE_TEST_NETWORK=1 (needs git, the network and
// QSTATE_TEST_STATE_DIR with the epoch-229 files for the last part). The mirror lives in the harness' scratch directory
// (below TMPDIR) and is deleted with it.
#include "fixtures.h"
#include "test_env.h"

#include <doctest/doctest.h>

#include <iostream>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

} // namespace

TEST_CASE("network: sync, list tags, auto ref for the epoch-229 files on https://github.com/qubic/core") {
    if (!qstate::testing::networkTests()) {
        MESSAGE("skipped: QSTATE_TEST_NETWORK=1 not set");
        return;
    }
    Harness h;
    const std::string url = h.ok("app.info")["defaultRepoUrl"];
    REQUIRE(url == "https://github.com/qubic/core");
    EventCollector events(h.bus);

    const auto t0 = Clock::now();
    json repo = h.ok("core.sync", {{"repoUrl", url}});
    const double firstMs = msSince(t0);
    CHECK(events.count("core.progress") > 0);
    CHECK(events.waitFor("core.progress", std::chrono::milliseconds(100), [](const json& p) { return p["phase"] == "clone" && p.value("percent", 0) >= 20; }).is_object());

    const auto t1 = Clock::now();
    json second = h.ok("core.sync", {{"repoUrl", url}});
    const double secondMs = msSince(t1);
    const auto t2 = Clock::now();
    json offline = h.ok("core.sync", {{"repoUrl", url}, {"offline", true}});
    const double offlineMs = msSince(t2);
    std::cerr << "[network] core.sync " << url << ": first (clone + " << repo["tags"].size() << " tags with version / epoch) " << firstMs
              << " ms, second (fetch) " << secondMs << " ms, offline listing " << offlineMs << " ms\n";
    CHECK(offlineMs < 1000);
    CHECK(offline == second);

    REQUIRE(repo["tags"].size() > 150);
    CHECK(repo["defaultBranch"].is_string());
    bool found = false;
    for (const json& tag : repo["tags"]) {
        if (tag["ref"] != "v1.303.2") continue;
        found = true;
        CHECK(tag["epoch"] == 229);
        CHECK(tag["version"] == "1.303.2");
    }
    CHECK(found);

    json commits = h.ok("core.commits", {{"repoUrl", url}, {"ref", "v1.303.2"}, {"limit", 20}});
    CHECK(commits["commits"].size() == 20);
    CHECK(commits["total"].get<int>() > 100);

    const std::string state = qstate::testing::stateDir();
    if (state.empty()) {
        MESSAGE("QSTATE_TEST_STATE_DIR not set: the workspace part is skipped");
        return;
    }
    const auto t3 = Clock::now();
    json ws = h.ok("workspace.open", {{"core", {{"repoUrl", url}, {"ref", "auto"}}}, {"statePath", state}});
    std::cerr << "[network] workspace.open (auto, mirror warm): " << msSince(t3) << " ms\n";
    CHECK(ws["core"]["ref"] == "v1.303.2");
    CHECK(ws["core"]["repoUrl"] == url);
    REQUIRE(ws["contracts"].size() == 29);
    for (const json& c : ws["contracts"]) CHECK_MESSAGE(c["status"] == "ok", c["index"]);
}
