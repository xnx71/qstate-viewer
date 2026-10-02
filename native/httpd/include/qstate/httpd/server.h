// HTTP + Server-Sent-Events transport of the RPC contract, for browser based UI development
// (and for the desktop app's --serve / --ui-dir modes).
//
//   POST /rpc      {"method": "...", "params": {...}}  ->  {"result": ...} | {"error": {...}}
//   GET  /events   text/event-stream:  event: <name>\ndata: <json>\n\n   (": heartbeat" comments in between)
//   GET  /health   {"ok": true}
//   GET  /...      static files from Options::staticDir (when set)
//
// Security model (the server exposes the file system listing of the user, so it is locked down):
//   * binds to 127.0.0.1 by default;
//   * with a loopback bind the Host header must be localhost / 127.0.0.1 / [::1] (DNS rebinding protection);
//   * a request carrying an Origin header is rejected (403) unless the origin is http://localhost:*,
//     http://127.0.0.1:* or http://[::1]:* or listed in Options::allowedOrigins; allowed origins get CORS
//     headers (this is what lets the Vite dev server on another port talk to us);
//   * POST /rpc must have Content-Type application/json (forces a CORS preflight for foreign pages);
//   * optional shared token for /rpc and /events: header `X-Qstate-Token: <token>`, `Authorization: Bearer <token>`
//     or query parameter `?token=<token>` (EventSource cannot set headers).
#pragma once

#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace qstate::httpd {

struct Options {
    std::string host = "127.0.0.1";
    // 0 = pick a free port (see Server::port()).
    int port = 0;
    // Empty = no authentication.
    std::string token;
    // Directory served at "/" (the built UI). Empty = no static files.
    std::string staticDir;
    // Additional exact origins that get CORS access, e.g. "https://my-dev-box:5173".
    std::vector<std::string> allowedOrigins;
    // false: only /health and the static files are served (no /rpc, no /events).
    bool enableRpc = true;
    std::chrono::milliseconds heartbeat{15000};
    std::size_t maxBodyBytes = std::size_t(64) << 20;
    // Every SSE client occupies one server thread; further clients get 503.
    std::size_t maxEventStreams = 8;
};

// Thread safety: start() / stop() / port() may be called from any thread; start() and stop() are serialized.
// The Dispatcher and EventBus must outlive the Server. Handlers of /rpc run on the HTTP worker threads and
// therefore concurrently: the registered methods must be thread-safe (as for every transport).
class Server {
public:
    Server(rpc::Dispatcher& dispatcher, rpc::EventBus& events, Options options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts serving on a background thread. Returns the port. Throws std::runtime_error when the
    // address cannot be bound or staticDir does not exist. Calling it twice is an error.
    int start();

    // Ends all event streams, stops accepting requests and joins the server threads. Idempotent.
    void stop();

    int port() const noexcept;
    bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qstate::httpd
