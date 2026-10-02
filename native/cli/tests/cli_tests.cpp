#include "../../service/tests/fixtures.h"
#include "qstate/cli/cli.h"

#include <httplib/httplib.h>

#include <doctest/doctest.h>

#include <sstream>

using namespace qstate;
using namespace qstate::service::testing;
using nlohmann::json;

namespace {

struct Run {
    int code = -1;
    std::string out, err;
};

Run runCli(std::vector<std::string> args, const std::atomic<bool>* stop = nullptr) {
    std::ostringstream out, err;
    Run r;
    r.code = cli::run(args, out, err, stop);
    r.out = out.str();
    r.err = err.str();
    return r;
}

struct Fixture {
    TempDir core, state;
    Fixture() {
        writeFakeCore(core.path());
        writeFakeState(state.path());
    }
    std::vector<std::string> common() const { return {"--core", core.str(), "--state", state.str()}; }
    std::vector<std::string> with(std::vector<std::string> first, const std::vector<std::string>& more = {}) const {
        for (const auto& c : common()) first.push_back(c);
        for (const auto& m : more) first.push_back(m);
        return first;
    }
};

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("cli: help and usage errors") {
    Run help = runCli({"--help"});
    CHECK(help.code == 0);
    CHECK(contains(help.out, "verify"));
    CHECK(contains(help.out, "serve"));
    CHECK(runCli({"verify", "--help"}).code == 0);
    CHECK(runCli({}).code == 2);
    Run bad = runCli({"frobnicate"});
    CHECK(bad.code == 2);
    CHECK(contains(bad.err, "unknown command"));
    Run missing = runCli({"verify", "--core", "/x"});
    CHECK(missing.code == 2);
    CHECK(contains(missing.err, "--state"));
    CHECK(runCli({"verify", "--nope"}).code == 2);
    CHECK(runCli({"dump", "--core", "/x", "--state", "/y"}).code == 2); // --contract missing
    CHECK(runCli({"verify", "--core", "/x", "--state", "/y", "--epoch", "abc"}).code == 2);
    Run notDir = runCli({"verify", "--core", "/does/not/exist", "--state", "/tmp"});
    CHECK(notDir.code == 2);
    CHECK(contains(notDir.err, "io_error"));
}

TEST_CASE("cli: verify") {
    Fixture f;
    Run ok = runCli(f.with({"verify"}));
    INFO(ok.out << ok.err);
    CHECK(ok.code == 0);
    CHECK(contains(ok.out, "3 ok, 0 not ok, 0 without file"));
    CHECK(contains(ok.out, "A::StateData"));
    CHECK(contains(ok.out, "328"));

    writeBytes(f.state / "contract0002.005", patternBytes(16, 1));
    Run bad = runCli(f.with({"verify"}));
    CHECK(bad.code == 1);
    CHECK(contains(bad.out, "size-mismatch"));
    CHECK(contains(bad.out, "2 ok, 1 not ok"));
    CHECK(contains(bad.out, "B::StateData"));

    // a missing file alone does not fail verification
    fs::remove(f.state / "contract0002.005");
    Run missing = runCli(f.with({"verify"}));
    CHECK(missing.code == 0);
    CHECK(contains(missing.out, "1 without file"));
    // the sample epoch
    CHECK(runCli(f.with({"verify"}, {"--epoch", "5"})).code == 0);
}

TEST_CASE("cli: schema") {
    Fixture f;
    Run list = runCli({"schema", "--core", f.core.str()});
    INFO(list.out << list.err);
    CHECK(list.code == 0);
    CHECK(contains(list.out, "A::StateData"));
    CHECK(contains(list.out, "src/contracts/B.h"));
    CHECK(contains(list.out, "3 contracts"));

    Run one = runCli({"schema", "--core", f.core.str(), "--contract", "1"});
    CHECK(one.code == 0);
    CHECK(contains(one.out, "size 328"));
    CHECK(contains(one.out, "rows"));
    CHECK(contains(one.out, "counter"));
    CHECK(contains(one.out, "0x140")); // counter at 320
    Run deep = runCli({"schema", "--core", f.core.str(), "--contract", "1", "--depth", "2"});
    CHECK(deep.code == 0);

    Run js = runCli({"schema", "--core", f.core.str(), "--contract", "1", "--json"});
    REQUIRE(js.code == 0);
    json j = json::parse(js.out);
    CHECK(j["contract"]["name"] == "AA");
    CHECK(j["contract"]["expectedSize"] == 328);
    CHECK(j["types"].size() >= 3);
    Run all = runCli({"schema", "--core", f.core.str(), "--json"});
    CHECK(json::parse(all.out)["contracts"].size() == 3);

    Run unknown = runCli({"schema", "--core", f.core.str(), "--contract", "9"});
    CHECK(unknown.code == 2);
    CHECK(contains(unknown.err, "not_found"));
    TempDir empty;
    CHECK(runCli({"schema", "--core", empty.str()}).code == 2);
}

TEST_CASE("cli: dump, table, search, digest") {
    Fixture f;
    Run dump = runCli(f.with({"dump"}, {"--contract", "1"}));
    INFO(dump.out << dump.err);
    REQUIRE(dump.code == 0);
    CHECK(contains(dump.out, "contract 1 AA"));
    CHECK(contains(dump.out, "rows"));
    CHECK(contains(dump.out, "counter"));
    CHECK(contains(dump.out, "f:rows") == contains(dump.out, "rows"));

    // find the node id of rows from the output: the NODE column is the last token of the line
    std::string rowsNode;
    {
        std::istringstream in(dump.out);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("rows", 0) == 0) rowsNode = line.substr(line.find_last_of(' ') + 1);
        }
    }
    REQUIRE_FALSE(rowsNode.empty());
    Run kids = runCli(f.with({"dump"}, {"--contract", "1", "--node", rowsNode, "--limit", "3"}));
    CHECK(kids.code == 0);
    CHECK(contains(kids.out, "showing 1-3 of 8"));
    CHECK(runCli(f.with({"dump"}, {"--contract", "1", "--limit", "0"})).code == 2);
    CHECK(runCli(f.with({"dump"}, {"--contract", "1", "--node", "bogus"})).code == 2);

    Run table = runCli(f.with({"table"}, {"--contract", "1", "--node", rowsNode, "--limit", "4", "--sort", "value.amount:desc"}));
    INFO(table.out << table.err);
    CHECK(table.code == 0);
    CHECK(contains(table.out, "8 rows"));
    CHECK(contains(table.out, "42"));
    Run filtered = runCli(f.with({"table"}, {"--contract", "1", "--node", rowsNode, "--filter", "value.amount:nonzero"}));
    CHECK(filtered.code == 0);
    CHECK(contains(filtered.out, "of 1 rows"));
    CHECK(runCli(f.with({"table"}, {"--contract", "1"})).code == 2); // --node missing
    CHECK(runCli(f.with({"table"}, {"--contract", "1", "--node", rowsNode, "--sort", "nope"})).code == 2);

    Run search = runCli(f.with({"search"}, {"--contract", "1", "--query", "0x2a00000000000000"}));
    CHECK(search.code == 0);
    CHECK(contains(search.out, "0x20"));
    CHECK(contains(search.out, "1 matches"));

    Run digest = runCli(f.with({"digest"}, {"--contract", "1"}));
    CHECK(digest.code == 0);
    CHECK(contains(digest.out, "k12 "));
    CHECK(digest.out.size() > 70);
}

TEST_CASE("cli: serve answers RPC over HTTP until stopped") {
    Fixture f;
    const int port = 30000 + static_cast<int>(::getpid() % 20000);
    std::atomic<bool> stop{false};
    Run result;
    std::thread server([&] { result = runCli({"serve", "--port", std::to_string(port)}, &stop); });
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1, 0);
    bool answered = false;
    for (int i = 0; i < 50 && !answered; ++i) {
        auto res = client.Post("/rpc", R"({"method":"app.info","params":{}})", "application/json");
        if (res && res->status == 200) {
            answered = true;
            json r = json::parse(res->body);
            CHECK(r["result"]["name"] == "qstate-cli");
            CHECK(r["result"]["transport"] == "http");
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CHECK(answered);
    if (answered) {
        json open = {{"method", "workspace.open"}, {"params", {{"coreDir", f.core.str()}, {"stateDir", f.state.str()}}}};
        auto res = client.Post("/rpc", open.dump(), "application/json");
        REQUIRE(res);
        CHECK(json::parse(res->body)["result"]["contracts"].size() == 3);
    }
    stop = true;
    server.join();
    CHECK(result.code == 0);
}
