#include "qstate/httpd/server.h"

#include <doctest/doctest.h>
#include <httplib/httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace qstate;
using nlohmann::json;

namespace {

struct Fixture {
    rpc::Dispatcher dispatcher{2};
    rpc::EventBus bus;
    std::unique_ptr<httpd::Server> server;

    explicit Fixture(httpd::Options options = {}) {
        dispatcher.registerMethod("echo", [](const json& p, rpc::CallContext&) { return p; });
        dispatcher.registerMethod("fail", [](const json&, rpc::CallContext&) -> json {
            throw rpc::Error(rpc::Code::NotFound, "nothing here", json{{"why", "test"}});
        });
        options.heartbeat = std::chrono::milliseconds(100);
        server = std::make_unique<httpd::Server>(dispatcher, bus, std::move(options));
        server->start();
    }
    httplib::Client client() const {
        httplib::Client c("127.0.0.1", server->port());
        c.set_read_timeout(5, 0);
        return c;
    }
};

json postRpc(httplib::Client& c, const std::string& body, httplib::Headers headers = {}) {
    auto res = c.Post("/rpc", headers, body, "application/json");
    REQUIRE(res);
    return json::parse(res->body);
}

std::string readSome(httplib::Client& c, const std::string& path, std::size_t minBytes,
                     const std::function<void()>& afterConnect = {}, const std::string& must = {}) {
    std::string collected;
    bool triggered = false;
    c.Get(path, [&](const httplib::Response&) { return true; },
          [&](const char* data, std::size_t len) {
              collected.append(data, len);
              if (!triggered && afterConnect) {
                  triggered = true;
                  afterConnect();
              }
              return collected.size() < minBytes || (!must.empty() && collected.find(must) == std::string::npos);
          });
    return collected;
}

} // namespace

TEST_CASE("rpc round trip over http") {
    Fixture f;
    auto c = f.client();
    CHECK(postRpc(c, R"({"method":"echo","params":{"x":[1,2,3]}})") == json{{"result", {{"x", {1, 2, 3}}}}});
    json err = postRpc(c, R"({"method":"fail","params":{}})");
    CHECK(err["error"]["code"] == "not_found");
    CHECK(err["error"]["data"]["why"] == "test");
    CHECK(postRpc(c, R"({"method":"nope"})")["error"]["code"] == "unknown_method");
    auto bad = c.Post("/rpc", "{", "application/json");
    REQUIRE(bad);
    CHECK(bad->status == 400);
    CHECK(json::parse(bad->body)["error"]["code"] == "invalid_params");
    auto wrongType = c.Post("/rpc", R"({"method":"echo"})", "text/plain");
    REQUIRE(wrongType);
    CHECK(wrongType->status == 415);
    auto health = c.Get("/health");
    REQUIRE(health);
    CHECK(json::parse(health->body)["ok"] == true);
}

TEST_CASE("large payloads and concurrent clients") {
    Fixture f;
    f.dispatcher.registerMethod("blob", [](const json& p, rpc::CallContext&) {
        return std::string(p.at("n").get<std::size_t>(), 'x');
    });
    auto c = f.client();
    json r = postRpc(c, R"({"method":"blob","params":{"n":5000000}})");
    CHECK(r["result"].get<std::string>().size() == 5000000);

    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            auto cl = f.client();
            for (int i = 0; i < 50; ++i) {
                auto res = cl.Post("/rpc", json{{"method", "echo"}, {"params", {{"t", t}, {"i", i}}}}.dump(),
                                   "application/json");
                if (res && json::parse(res->body)["result"]["i"] == i) {
                    ++ok;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(ok.load() == 300);
}

TEST_CASE("only loopback hosts and origins are accepted") {
    Fixture f;
    auto c = f.client();
    // CORS for the Vite dev server
    auto pre = c.Options("/rpc", {{"Origin", "http://localhost:5173"}, {"Access-Control-Request-Method", "POST"}});
    REQUIRE(pre);
    CHECK(pre->status == 204);
    CHECK(pre->get_header_value("Access-Control-Allow-Origin") == "http://localhost:5173");
    CHECK(pre->get_header_value("Access-Control-Allow-Headers").find("Content-Type") != std::string::npos);

    auto ok = c.Post("/rpc", {{"Origin", "http://127.0.0.1:5173"}}, R"({"method":"echo","params":1})", "application/json");
    REQUIRE(ok);
    CHECK(ok->status == 200);
    CHECK(ok->get_header_value("Access-Control-Allow-Origin") == "http://127.0.0.1:5173");

    for (const char* origin : {"http://evil.example", "https://localhost:5173", "http://localhost.evil.com",
                               "http://localhost:abc", "null"}) {
        auto res = c.Post("/rpc", {{"Origin", origin}}, R"({"method":"echo","params":1})", "application/json");
        REQUIRE(res);
        CHECK_MESSAGE(res->status == 403, origin);
        CHECK(res->get_header_value("Access-Control-Allow-Origin").empty());
    }
    auto badHost = c.Post("/rpc", {{"Host", "evil.example"}}, R"({"method":"echo","params":1})", "application/json");
    REQUIRE(badHost);
    CHECK(badHost->status == 403);
}

TEST_CASE("extra allowed origins") {
    httpd::Options o;
    o.allowedOrigins = {"https://dev.example:5173"};
    Fixture f(o);
    auto c = f.client();
    auto res = c.Post("/rpc", {{"Origin", "https://dev.example:5173"}}, R"({"method":"echo","params":1})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
}

TEST_CASE("token check") {
    httpd::Options o;
    o.token = "s3cret";
    Fixture f(o);
    auto c = f.client();
    auto none = c.Post("/rpc", R"({"method":"echo","params":1})", "application/json");
    REQUIRE(none);
    CHECK(none->status == 401);
    auto wrong = c.Post("/rpc", {{"X-Qstate-Token", "nope"}}, R"({"method":"echo","params":1})", "application/json");
    REQUIRE(wrong);
    CHECK(wrong->status == 401);
    CHECK(postRpc(c, R"({"method":"echo","params":1})", {{"X-Qstate-Token", "s3cret"}})["result"] == 1);
    CHECK(postRpc(c, R"({"method":"echo","params":2})", {{"Authorization", "Bearer s3cret"}})["result"] == 2);
    auto events = c.Get("/events");
    REQUIRE(events);
    CHECK(events->status == 401);
    auto health = c.Get("/health"); // never protected
    REQUIRE(health);
    CHECK(health->status == 200);
}

TEST_CASE("server sent events") {
    Fixture f;
    auto c = f.client();
    std::string text = readSome(
        c, "/events", 1,
        [&] {
            std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                f.bus.emit("workspace.updated", json{{"id", 7}});
                f.bus.emit("contracts.changed", json{{"workspaceId", 7}});
            }).detach();
        },
        "contracts.changed");
    CHECK(text.find(": connected") != std::string::npos);
    CHECK(text.find("event: workspace.updated\ndata: {\"id\":7}\n\n") != std::string::npos);
    CHECK(text.find("event: contracts.changed\ndata: {\"workspaceId\":7}\n\n") != std::string::npos);
    // The subscription of the closed stream is released.
    for (int i = 0; i < 100 && f.bus.subscriberCount() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(f.bus.subscriberCount() == 0);
}

TEST_CASE("sse heartbeat") {
    Fixture f;
    auto c = f.client();
    std::string text = readSome(c, "/events", 1, {}, ": heartbeat");
    CHECK(text.find(": heartbeat") != std::string::npos);
}

TEST_CASE("clean shutdown with connected event streams") {
    auto f = std::make_unique<Fixture>();
    std::atomic<int> connected{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 3; ++i) {
        clients.emplace_back([&] {
            auto c = f->client();
            c.set_read_timeout(30, 0);
            c.Get("/events", [&](const httplib::Response&) { return true; },
                  [&](const char*, std::size_t) {
                      ++connected;
                      return true;
                  });
        });
    }
    for (int i = 0; i < 200 && connected.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(f->bus.subscriberCount() == 3);
    auto start = std::chrono::steady_clock::now();
    f->server->stop();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(f->bus.subscriberCount() == 0);
    for (auto& t : clients) {
        t.join(); // the clients see the end of the stream
    }
    CHECK_FALSE(f->server->running());
    f->server->stop(); // idempotent
}

TEST_CASE("static files") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("qstate_httpd_static_" + std::to_string(::getpid()));
    fs::create_directories(dir / "assets");
    std::ofstream(dir / "index.html") << "<html>hello</html>";
    std::ofstream(dir / "assets" / "app.js") << "console.log(1)";
    {
        httpd::Options o;
        o.staticDir = dir.string();
        Fixture f(o);
        auto c = f.client();
        auto index = c.Get("/");
        REQUIRE(index);
        CHECK(index->status == 200);
        CHECK(index->body == "<html>hello</html>");
        auto js = c.Get("/assets/app.js");
        REQUIRE(js);
        CHECK(js->body == "console.log(1)");
        auto missing = c.Get("/nope.txt");
        REQUIRE(missing);
        CHECK(missing->status == 404);
        auto traversal = c.Get("/../../etc/passwd");
        REQUIRE(traversal);
        CHECK(traversal->status != 200);
        // rpc still there
        CHECK(postRpc(c, R"({"method":"echo","params":3})")["result"] == 3);
    }
    {
        httpd::Options o;
        o.staticDir = dir.string();
        o.enableRpc = false;
        Fixture f(o);
        auto c = f.client();
        auto res = c.Post("/rpc", R"({"method":"echo"})", "application/json");
        REQUIRE(res);
        CHECK(res->status != 200);
        auto events = c.Get("/events");
        REQUIRE(events);
        CHECK(events->status == 404);
    }
    fs::remove_all(dir);
}

TEST_CASE("start errors") {
    rpc::Dispatcher d;
    rpc::EventBus bus;
    {
        httpd::Options o;
        o.staticDir = "/definitely/not/here";
        httpd::Server s(d, bus, o);
        CHECK_THROWS_AS(s.start(), std::runtime_error);
    }
    {
        httpd::Server a(d, bus);
        int port = a.start();
        httpd::Options o;
        o.port = port;
        httpd::Server b(d, bus, o);
        CHECK_THROWS_AS(b.start(), std::runtime_error);
        CHECK_THROWS_AS(a.start(), std::logic_error);
    }
}
