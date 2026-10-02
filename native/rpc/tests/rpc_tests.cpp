#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/rpc/framing.h"
#include "qstate/rpc/params.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <thread>

using namespace qstate::rpc;
using nlohmann::json;

TEST_CASE("dispatch returns results") {
    Dispatcher d;
    d.registerMethod("echo", [](const json& params, CallContext&) { return params; });
    json r = d.dispatch("echo", json{{"a", 1}});
    CHECK(r == json{{"result", {{"a", 1}}}});
    CHECK(d.has("echo"));
    CHECK_FALSE(d.has("nope"));
    CHECK(d.methods() == std::vector<std::string>{"echo"});
}

TEST_CASE("errors are mapped to the error object") {
    Dispatcher d;
    d.registerMethod("bad", [](const json&, CallContext&) -> json {
        throw Error(Code::InvalidParams, "x is missing", json{{"field", "x"}});
    });
    d.registerMethod("nodata", [](const json&, CallContext&) -> json { throw Error(Code::NoWorkspace, "none"); });
    json r = d.dispatch("bad", json::object());
    CHECK(r["error"]["code"] == "invalid_params");
    CHECK(r["error"]["message"] == "x is missing");
    CHECK(r["error"]["data"]["field"] == "x");
    CHECK_FALSE(r.contains("result"));
    json r2 = d.dispatch("nodata", json::object());
    CHECK(r2["error"]["code"] == "no_workspace");
    CHECK_FALSE(r2["error"].contains("data"));
}

TEST_CASE("every code has its wire name") {
    CHECK(codeName(Code::InvalidParams) == "invalid_params");
    CHECK(codeName(Code::UnknownMethod) == "unknown_method");
    CHECK(codeName(Code::NotFound) == "not_found");
    CHECK(codeName(Code::NoWorkspace) == "no_workspace");
    CHECK(codeName(Code::IoError) == "io_error");
    CHECK(codeName(Code::SchemaError) == "schema_error");
    CHECK(codeName(Code::Internal) == "internal");
}

TEST_CASE("unknown methods and foreign exceptions never crash") {
    Dispatcher d;
    d.registerMethod("std", [](const json&, CallContext&) -> json { throw std::runtime_error("boom"); });
    d.registerMethod("json", [](const json& p, CallContext&) -> json { return p.at("missing"); });
    d.registerMethod("int", [](const json&, CallContext&) -> json { throw 42; });
    CHECK(d.dispatch("zzz", json::object())["error"]["code"] == "unknown_method");
    json r = d.dispatch("std", json::object());
    CHECK(r["error"]["code"] == "internal");
    CHECK(r["error"]["message"] == "boom");
    CHECK(d.dispatch("json", json::object())["error"]["code"] == "internal");
    CHECK(d.dispatch("int", json::object())["error"]["code"] == "internal");
}

TEST_CASE("handlers can be replaced while dispatching") {
    Dispatcher d;
    d.registerMethod("v", [](const json&, CallContext&) { return 1; });
    CHECK(d.dispatch("v", nullptr)["result"] == 1);
    d.registerMethod("v", [](const json&, CallContext&) { return 2; });
    CHECK(d.dispatch("v", nullptr)["result"] == 2);
}

TEST_CASE("dispatchAsync runs handlers on the pool") {
    Dispatcher d(3);
    d.registerMethod("tid", [](const json&, CallContext&) {
        std::ostringstream os;
        os << std::this_thread::get_id();
        return os.str();
    });
    std::promise<json> promise;
    d.dispatchAsync("tid", json::object(), [&](json response) { promise.set_value(std::move(response)); });
    json r = promise.get_future().get();
    std::ostringstream me;
    me << std::this_thread::get_id();
    CHECK(r["result"].get<std::string>() != me.str());
}

TEST_CASE("dispatchAsync runs calls concurrently and answers every call exactly once") {
    Dispatcher d(4);
    std::atomic<int> inFlight{0};
    std::atomic<int> maxInFlight{0};
    d.registerMethod("work", [&](const json& p, CallContext&) {
        int now = ++inFlight;
        int prev = maxInFlight.load();
        while (now > prev && !maxInFlight.compare_exchange_weak(prev, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        --inFlight;
        return p["n"];
    });
    constexpr int kCalls = 200;
    std::mutex m;
    std::multiset<int> seen;
    std::promise<void> done;
    std::atomic<int> count{0};
    for (int i = 0; i < kCalls; ++i) {
        d.dispatchAsync("work", json{{"n", i}}, [&](json response) {
            {
                std::lock_guard lock(m);
                seen.insert(response["result"].get<int>());
            }
            if (++count == kCalls) {
                done.set_value();
            }
        });
    }
    REQUIRE(done.get_future().wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    CHECK(seen.size() == static_cast<std::size_t>(kCalls));
    for (int i = 0; i < kCalls; ++i) {
        CHECK(seen.count(i) == 1);
    }
    CHECK(maxInFlight.load() > 1);
    CHECK(maxInFlight.load() <= 4);
}

TEST_CASE("concurrent dispatch from many threads while registering") {
    Dispatcher d;
    std::atomic<bool> stop{false};
    d.registerMethod("inc", [](const json& p, CallContext&) { return p.get<int>() + 1; });
    std::thread registrar([&] {
        int i = 0;
        while (!stop) {
            d.registerMethod("m" + std::to_string(i++ % 50), [](const json&, CallContext&) { return 0; });
        }
    });
    std::vector<std::thread> callers;
    std::atomic<int> bad{0};
    for (int t = 0; t < 4; ++t) {
        callers.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                if (d.dispatch("inc", i)["result"] != i + 1) {
                    ++bad;
                }
            }
        });
    }
    for (auto& c : callers) {
        c.join();
    }
    stop = true;
    registrar.join();
    CHECK(bad.load() == 0);
}

TEST_CASE("exceptions in async handlers and callbacks are contained") {
    Dispatcher d(2);
    d.registerMethod("throw", [](const json&, CallContext&) -> json { throw std::logic_error("x"); });
    d.registerMethod("ok", [](const json&, CallContext&) { return "fine"; });
    std::promise<json> p1, p2;
    d.dispatchAsync("throw", json::object(), [&](json r) { p1.set_value(std::move(r)); });
    CHECK(p1.get_future().get()["error"]["code"] == "internal");
    d.dispatchAsync("ok", json::object(), [&](json) { throw std::runtime_error("callback failure"); });
    d.dispatchAsync("ok", json::object(), [&](json r) { p2.set_value(std::move(r)); });
    CHECK(p2.get_future().get()["result"] == "fine"); // pool still alive
}

TEST_CASE("cancellation") {
    Dispatcher d(1);
    std::promise<void> started;
    std::atomic<bool> sawCancel{false};
    d.registerMethod("loop", [&](const json&, CallContext& ctx) -> json {
        started.set_value();
        for (int i = 0; i < 5000; ++i) {
            if (ctx.cancelled()) {
                sawCancel = true;
                ctx.throwIfCancelled();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return "finished";
    });
    d.registerMethod("quick", [](const json&, CallContext&) { return 1; });
    std::promise<json> loopDone, queuedDone;
    auto token = d.dispatchAsync("loop", json::object(), [&](json r) { loopDone.set_value(std::move(r)); });
    started.get_future().wait();
    // The single worker is busy: this one stays queued and is cancelled before it starts.
    auto queued = d.dispatchAsync("quick", json::object(), [&](json r) { queuedDone.set_value(std::move(r)); });
    queued->cancel();
    token->cancel();
    json r = loopDone.get_future().get();
    CHECK(sawCancel.load());
    CHECK(r["error"]["message"] == "cancelled");
    json q = queuedDone.get_future().get();
    CHECK(q["error"]["message"] == "cancelled");
}

TEST_CASE("the cancellation flag can be polled by APIs that take a std::atomic<bool>*") {
    auto token = std::make_shared<CancelToken>();
    CallContext ctx("m", token);
    const std::atomic<bool>* flag = ctx.cancelFlag();
    REQUIRE(flag != nullptr);
    CHECK_FALSE(flag->load());
    token->cancel();
    CHECK(flag->load());
    CHECK(ctx.cancelled());
    CHECK(&token->flag() == flag);
}

TEST_CASE("shutdown cancels running calls, rejects queued ones, and later calls") {
    auto d = std::make_unique<Dispatcher>(1);
    std::promise<void> started;
    d->registerMethod("loop", [&](const json&, CallContext& ctx) -> json {
        started.set_value();
        while (!ctx.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return "stopped";
    });
    std::promise<json> a, b;
    d->dispatchAsync("loop", json::object(), [&](json r) { a.set_value(std::move(r)); });
    started.get_future().wait();
    d->dispatchAsync("loop", json::object(), [&](json r) { b.set_value(std::move(r)); });
    d->shutdown();
    CHECK(a.get_future().get()["result"] == "stopped");
    CHECK(b.get_future().get()["error"]["message"] == "cancelled");
    std::promise<json> c;
    d->dispatchAsync("loop", json::object(), [&](json r) { c.set_value(std::move(r)); });
    CHECK(c.get_future().get()["error"]["message"] == "cancelled");
    d->shutdown(); // idempotent
    d.reset();
}

TEST_CASE("destroying an idle or never used dispatcher is fine") {
    { Dispatcher d(8); }
    {
        Dispatcher d(2);
        d.registerMethod("a", [](const json&, CallContext&) { return 1; });
        std::promise<json> p;
        d.dispatchAsync("a", json::object(), [&](json r) { p.set_value(std::move(r)); });
        p.get_future().get();
    }
}

TEST_CASE("framing") {
    Dispatcher d;
    d.registerMethod("sum", [](const json& p, CallContext&) { return p.at("a").get<int>() + p.at("b").get<int>(); });
    d.registerMethod("noparams", [](const json& p, CallContext&) { return p.is_object() && p.empty(); });

    CHECK(json::parse(handleRequest(d, R"({"method":"sum","params":{"a":2,"b":3}})")) == json{{"result", 5}});
    CHECK(json::parse(handleRequest(d, R"({"method":"noparams"})"))["result"] == true);
    CHECK(json::parse(handleRequest(d, R"({"method":"noparams","params":null})"))["result"] == true);
    CHECK(json::parse(handleRequest(d, "not json"))["error"]["code"] == "invalid_params");
    CHECK(json::parse(handleRequest(d, "[1]"))["error"]["code"] == "invalid_params");
    CHECK(json::parse(handleRequest(d, R"({"params":{}})"))["error"]["code"] == "invalid_params");
    CHECK(json::parse(handleRequest(d, R"({"method":7})"))["error"]["code"] == "invalid_params");
    CHECK(json::parse(handleRequest(d, R"({"method":""})"))["error"]["code"] == "invalid_params");
    CHECK(json::parse(handleRequest(d, R"({"method":"x"})"))["error"]["code"] == "unknown_method");
    CHECK(json::parse(handleRequest(d, R"({"method":"sum","params":{}})"))["error"]["code"] == "internal");
    CHECK(isError(makeError(Code::NotFound, "n")));
    CHECK_FALSE(isError(makeResult(1)));
}

TEST_CASE("serialize survives invalid UTF-8") {
    std::string bad = "ab\xff\xfe" "cd";
    std::string text = serialize(makeResult(json(bad)));
    json parsed = json::parse(text);
    CHECK(parsed["result"].get<std::string>().find("ab") == 0);
}

TEST_CASE("event bus fans out to all sinks in order") {
    EventBus bus;
    std::vector<std::string> a, b;
    auto sa = bus.subscribeScoped([&](const std::string& n, const json& p) { a.push_back(n + p.dump()); });
    auto sb = bus.subscribeScoped([&](const std::string& n, const json& p) { b.push_back(n + p.dump()); });
    CHECK(bus.subscriberCount() == 2);
    bus.emit("one", 1);
    bus.emit("two", json{{"k", "v"}});
    CHECK(a == std::vector<std::string>{"one1", R"(two{"k":"v"})"});
    CHECK(a == b);
    sb.reset();
    bus.emit("three", 3);
    CHECK(a.size() == 3);
    CHECK(b.size() == 2);
    CHECK(bus.subscriberCount() == 1);
}

TEST_CASE("event bus survives sink exceptions and self-unsubscribe") {
    EventBus bus;
    int good = 0;
    bus.subscribe([](const std::string&, const json&) { throw std::runtime_error("bad sink"); });
    bus.subscribe([&](const std::string&, const json&) { ++good; });
    EventBus::Id once = 0;
    int onceCalls = 0;
    once = bus.subscribe([&](const std::string&, const json&) {
        ++onceCalls;
        bus.unsubscribe(once);
    });
    bus.emit("x", nullptr);
    bus.emit("x", nullptr);
    CHECK(good == 2);
    CHECK(onceCalls == 1);
    bus.unsubscribe(9999); // unknown id: no-op
}

TEST_CASE("event bus is thread safe and sinks never run concurrently with themselves") {
    EventBus bus;
    std::atomic<int> inSink{0};
    std::atomic<int> overlap{0};
    long count = 0; // deliberately unsynchronized: protected by the per-sink serialization
    auto sub = bus.subscribeScoped([&](const std::string&, const json&) {
        if (++inSink > 1) {
            ++overlap;
        }
        ++count;
        --inSink;
    });
    std::atomic<bool> churn{true};
    std::thread churner([&] {
        while (churn) {
            auto s = bus.subscribeScoped([](const std::string&, const json&) {});
        }
    });
    std::vector<std::thread> emitters;
    for (int t = 0; t < 4; ++t) {
        emitters.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) {
                bus.emit("e", i);
            }
        });
    }
    for (auto& e : emitters) {
        e.join();
    }
    churn = false;
    churner.join();
    CHECK(overlap.load() == 0);
    CHECK(count == 20000);
}

TEST_CASE("unsubscribe waits for a running sink") {
    EventBus bus;
    std::promise<void> inside;
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    auto id = bus.subscribe([&](const std::string&, const json&) {
        inside.set_value();
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        finished = true;
    });
    std::thread emitter([&] { bus.emit("x", nullptr); });
    inside.get_future().wait();
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release = true;
    });
    bus.unsubscribe(id);
    CHECK(finished.load()); // unsubscribe returned only after the sink completed
    emitter.join();
    releaser.join();
}

TEST_CASE("the transport reaches the handler") {
    Dispatcher d(1);
    d.registerMethod("t", [](const json&, CallContext& ctx) { return ctx.transport() + "/" + ctx.method(); });
    CHECK(d.dispatch("t", nullptr)["result"] == "/t");
    CHECK(d.dispatch("t", nullptr, "http")["result"] == "http/t");
    std::promise<json> p;
    d.dispatchAsync("t", nullptr, [&](json r) { p.set_value(std::move(r)); }, "webview");
    CHECK(p.get_future().get()["result"] == "webview/t");
}

TEST_CASE("param helpers") {
    json p = {{"s", "x"}, {"i", 5}, {"b", true}, {"n", nullptr}, {"f", 1.5}};
    CHECK(requireParam<std::string>(p, "s") == "x");
    CHECK(requireParam<int>(p, "i") == 5);
    CHECK(requireParam<bool>(p, "b"));
    CHECK_THROWS_AS(requireParam<std::string>(p, "i"), Error);
    CHECK_THROWS_AS(requireParam<int>(p, "s"), Error);
    CHECK_THROWS_AS(requireParam<int>(p, "f"), Error);
    CHECK_THROWS_AS(requireParam<int>(p, "n"), Error);
    CHECK_THROWS_AS(requireParam<int>(p, "missing"), Error);
    CHECK_THROWS_AS(requireParam<int>(json(3), "i"), Error);
    CHECK_THROWS_AS(requireParam<unsigned>(json{{"u", -1}}, "u"), Error);
    CHECK_THROWS_AS(requireParam<int>(json{{"u", 3000000000LL}}, "u"), Error);
    CHECK_THROWS_AS(requireParam<int>(json{{"u", 18446744073709551615ULL}}, "u"), Error);
    CHECK(requireParam<std::int64_t>(json{{"u", -5}}, "u") == -5);
    CHECK(requireParam<std::uint64_t>(json{{"u", 18446744073709551615ULL}}, "u") == 18446744073709551615ULL);
    CHECK(requireParam<std::uint8_t>(json{{"u", 255}}, "u") == 255);
    CHECK_THROWS_AS(requireParam<std::uint8_t>(json{{"u", 256}}, "u"), Error);
    CHECK(optionalParam<int>(p, "missing") == std::nullopt);
    CHECK(optionalParam<int>(p, "n") == std::nullopt);
    CHECK(optionalParam<int>(json(nullptr), "i") == std::nullopt);
    CHECK(optionalParam<int>(p, "i") == 5);
    CHECK_THROWS_AS(optionalParam<int>(p, "s"), Error);
}
