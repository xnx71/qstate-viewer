#include "qstate/gui/assets.h"
#include "qstate/gui/bridge.h"
#include "qstate/gui/event_pump.h"
#include "qstate/gui/memory.h"
#include "qstate/gui/selftest.h"
#include "qstate/rpc/framing.h"

#include <doctest/doctest.h>

#include <atomic>
#include <future>
#include <thread>

using namespace qstate;
using nlohmann::json;

TEST_CASE("bridge arguments") {
    gui::InvokeCall call;
    std::string error;
    REQUIRE(gui::parseInvokeArgs(R"(["state.node",{"contract":3,"id":""}])", call, error));
    CHECK(call.method == "state.node");
    CHECK(call.params == json{{"contract", 3}, {"id", ""}});
    REQUIRE(gui::parseInvokeArgs(R"(["app.info"])", call, error));
    CHECK(call.params == json::object());
    REQUIRE(gui::parseInvokeArgs(R"(["app.info",null])", call, error));
    CHECK(call.params == json::object());
    CHECK_FALSE(gui::parseInvokeArgs("{}", call, error));
    CHECK_FALSE(gui::parseInvokeArgs("[]", call, error));
    CHECK_FALSE(gui::parseInvokeArgs("[1]", call, error));
    CHECK_FALSE(gui::parseInvokeArgs(R"([""])", call, error));
    CHECK_FALSE(gui::parseInvokeArgs(R"(["a",{},3])", call, error));
    CHECK_FALSE(gui::parseInvokeArgs("not json", call, error));
}

TEST_CASE("bridge replies") {
    auto ok = gui::makeReply(rpc::makeResult(json{{"a", 1}}));
    CHECK(ok.status == 0);
    CHECK(json::parse(ok.json) == json{{"a", 1}});
    auto nullResult = gui::makeReply(rpc::makeResult(nullptr));
    CHECK(nullResult.status == 0);
    CHECK(nullResult.json == "null");
    auto err = gui::makeReply(rpc::makeError(rpc::Code::NotFound, "gone", json{{"k", 1}}));
    CHECK(err.status == 1);
    CHECK(json::parse(err.json) == json{{"code", "not_found"}, {"message", "gone"}, {"data", {{"k", 1}}}});
    auto bad = gui::makeReply(json{{"nothing", 1}});
    CHECK(bad.status == 1);
    CHECK(json::parse(bad.json)["code"] == "internal");
    // invalid UTF-8 must not throw
    CHECK_NOTHROW(gui::makeReply(rpc::makeResult(std::string("a\xff" "b"))));
}

TEST_CASE("js string literals are injection proof") {
    const std::string nasty = "q\" b\\ n\n r\r t\t </script> \xe2\x80\xa8 \xe2\x80\xa9 \x01 'é' `${x}`";
    std::string literal = gui::jsStringLiteral(nasty);
    CHECK(literal.front() == '"');
    CHECK(literal.back() == '"');
    CHECK(literal.find('\n') == std::string::npos);
    CHECK(literal.find('\r') == std::string::npos);
    CHECK(literal.find("\xe2\x80\xa8") == std::string::npos);
    CHECK(literal.find("\xe2\x80\xa9") == std::string::npos);
    CHECK(literal.find("\\u2028") != std::string::npos);
    // It is still valid JSON that decodes to the original text.
    CHECK(json::parse(literal).get<std::string>() == nasty);
    // The only unescaped double quotes are the delimiters.
    for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
        if (literal[i] == '"') {
            CHECK(literal[i - 1] == '\\');
        }
    }
}

TEST_CASE("emit script") {
    std::string script = gui::makeEmitScript({{"workspace.updated", R"({"id":1,"s":"</script>"})"}, {"contracts.changed", "[]"}});
    CHECK(script.find("window.__qstate_emit") != std::string::npos);
    CHECK(script.find("JSON.parse") != std::string::npos);
    CHECK(script.find(R"(["workspace.updated","{\"id\":1,\"s\":\"</script>\"}"])") != std::string::npos);
    CHECK(script.find(R"(["contracts.changed","[]"])") != std::string::npos);
}

TEST_CASE("event pump batches, coalesces and stops cleanly") {
    rpc::EventBus bus;
    std::mutex m;
    std::vector<std::string> scripts;
    std::promise<void> gotSecond;
    std::atomic<int> deliveries{0};
    {
        gui::PumpOptions options;
        options.interval = std::chrono::milliseconds(50);
        gui::EventPump pump(bus, [&](std::string script) {
            std::lock_guard lock(m);
            scripts.push_back(std::move(script));
            if (++deliveries == 2) {
                gotSecond.set_value();
            }
        }, options);
        // a burst: 30 workspace.updated (only the newest survives) interleaved with 5 contract events
        for (int i = 0; i < 30; ++i) {
            bus.emit("workspace.updated", json{{"n", i}});
            if (i % 6 == 0) {
                bus.emit("contracts.changed", json{{"i", i}});
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bus.emit("contracts.changed", json{{"i", 99}});
        REQUIRE(gotSecond.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
    std::lock_guard lock(m);
    REQUIRE(scripts.size() == 2);
    CHECK(scripts[0].find(R"(\"n\":29)") != std::string::npos);
    CHECK(scripts[0].find(R"(\"n\":0})") == std::string::npos);
    std::size_t count = 0;
    for (auto pos = scripts[0].find("contracts.changed"); pos != std::string::npos; pos = scripts[0].find("contracts.changed", pos + 1)) {
        ++count;
    }
    CHECK(count == 5);
    CHECK(scripts[1].find(R"(\"i\":99)") != std::string::npos);
    CHECK(bus.subscriberCount() == 0);
}

TEST_CASE("event pump never delivers after destruction and drops the oldest when flooded") {
    rpc::EventBus bus;
    std::atomic<bool> destroyed{false};
    std::atomic<int> late{0};
    std::size_t dropped = 0;
    {
        gui::PumpOptions options;
        options.interval = std::chrono::milliseconds(1000); // nothing is delivered during the test body
        options.maxQueued = 10;
        gui::EventPump pump(bus, [&](std::string) {
            if (destroyed) {
                ++late;
            }
        }, options);
        for (int i = 0; i < 100; ++i) {
            bus.emit("flood", i);
        }
        dropped = pump.dropped();
    }
    destroyed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(dropped == 90);
    CHECK(late.load() == 0);
}

TEST_CASE("selftest methods") {
    rpc::Dispatcher d;
    rpc::EventBus bus;
    gui::SelftestSession session(d, bus);
    CHECK(d.dispatch("selftest.echo", json{{"value", "x"}})["result"] == "x");
    CHECK(d.dispatch("selftest.fail", json::object())["error"]["data"]["detail"] == 42);
    CHECK(d.dispatch("selftest.blob", json{{"bytes", 1000}})["result"].get<std::string>().size() == 1000);
    CHECK(d.dispatch("selftest.blob", json{{"bytes", "no"}})["error"]["code"] == "invalid_params");
    CHECK(d.dispatch("selftest.slow", json{{"ms", 1}})["result"] == "done");
    std::vector<std::string> seen;
    auto sub = bus.subscribeScoped([&](const std::string& name, const json&) { seen.push_back(name); });
    CHECK(d.dispatch("selftest.emit", json{{"name", "e1"}, {"payload", 1}}).contains("result"));
    CHECK(seen == std::vector<std::string>{"e1"});

    CHECK_FALSE(session.reported());
    CHECK_FALSE(session.waitForReport(0));
    CHECK(session.summary().find("no report") != std::string::npos);
    d.dispatch("selftest.report", json{{"ok", true}, {"checks", {{{"name", "c"}, {"ok", true}}}}});
    CHECK(session.waitForReport(1));
    CHECK(session.passed());
    CHECK(session.summary().find("PASS") != std::string::npos);
}

TEST_CASE("embedded assets") {
    auto index = gui::embeddedIndexHtml();
    CHECK(index.size() > 100);
    std::string head(index.substr(0, 15));
    for (char& c : head) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(head == "<!doctype html>");
    CHECK(index.find("</html>") != std::string_view::npos);
    CHECK(gui::selftestPageHtml().find("__qstate_invoke") != std::string_view::npos);
    // The placeholder is only used while ui/dist/index.html does not exist.
    if (gui::embeddedIndexIsPlaceholder()) {
        CHECK(index.find("placeholder") != std::string_view::npos);
    }
}

TEST_CASE("idle trigger fires once per idle period") {
    using namespace std::chrono_literals;
    const auto t0 = gui::IdleTrigger::Clock::now();
    gui::IdleTrigger idle(60s, t0);
    CHECK_FALSE(idle.due(t0 + 59s));
    CHECK(idle.due(t0 + 60s));
    CHECK_FALSE(idle.due(t0 + 61s));   // once
    CHECK_FALSE(idle.due(t0 + 3600s)); // still disarmed
    idle.activity(t0 + 100s);          // a request re-arms it
    CHECK_FALSE(idle.due(t0 + 159s));
    CHECK(idle.due(t0 + 160s));
    idle.activity(t0 + 200s);
    idle.activity(t0 + 230s); // later activity moves the deadline
    CHECK_FALSE(idle.due(t0 + 260s));
    CHECK(idle.due(t0 + 290s));
}

TEST_CASE("allocator tuning is harmless") {
    // glibc only changes malloc parameters; elsewhere it is a no-op. Allocation still works afterwards.
    gui::tuneAllocator();
    std::vector<char> big(8u << 20, 1);
    CHECK(big.back() == 1);
}
