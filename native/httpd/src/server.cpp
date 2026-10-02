#include "qstate/httpd/server.h"

#include "qstate/rpc/framing.h"

#include <httplib/httplib.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace qstate::httpd {

namespace {

constexpr std::size_t kMaxQueuedEvents = 1000;

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// "host", "host:port", "[::1]", "[::1]:port" -> host part without brackets.
std::string hostOf(const std::string& hostHeader) {
    if (!hostHeader.empty() && hostHeader.front() == '[') {
        auto end = hostHeader.find(']');
        return end == std::string::npos ? hostHeader : hostHeader.substr(1, end - 1);
    }
    return hostHeader.substr(0, hostHeader.find(':'));
}

bool isLoopbackName(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

// http://localhost, http://localhost:5173, http://127.0.0.1:8080, http://[::1]:3000
bool isLoopbackOrigin(const std::string& origin) {
    if (!startsWith(origin, "http://")) {
        return false;
    }
    std::string rest = origin.substr(7);
    std::string host = hostOf(rest);
    if (!isLoopbackName(host)) {
        return false;
    }
    std::string afterHost = rest.substr(host.size() + (rest.front() == '[' ? 2 : 0));
    if (afterHost.empty()) {
        return true;
    }
    if (afterHost.front() != ':' || afterHost.size() == 1) {
        return false;
    }
    return std::all_of(afterHost.begin() + 1, afterHost.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

void setJson(httplib::Response& res, int status, const nlohmann::json& body) {
    res.status = status;
    res.set_content(rpc::serialize(body), "application/json");
}

} // namespace

struct Server::Impl {
    // One connected /events client.
    struct Stream {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> queue; // fully formatted SSE frames
        std::size_t dropped = 0;
        bool closed = false;
        rpc::EventBus::Subscription subscription;
    };

    Impl(rpc::Dispatcher& d, rpc::EventBus& e, Options o) : dispatcher(d), events(e), options(std::move(o)) {}

    rpc::Dispatcher& dispatcher;
    rpc::EventBus& events;
    Options options;
    httplib::Server http;
    std::thread thread;
    std::atomic<int> boundPort{0};
    std::atomic<bool> started{false};
    bool used = false; // guarded by lifecycle
    std::mutex lifecycle; // serializes start / stop

    std::mutex streamsMutex;
    std::set<std::shared_ptr<Stream>> streams;
    bool stopping = false; // guarded by streamsMutex

    bool originAllowed(const std::string& origin) const {
        if (isLoopbackOrigin(origin)) {
            return true;
        }
        return std::find(options.allowedOrigins.begin(), options.allowedOrigins.end(), origin) !=
               options.allowedOrigins.end();
    }

    void addCors(const httplib::Request& req, httplib::Response& res) const {
        std::string origin = req.get_header_value("Origin");
        if (origin.empty() || !originAllowed(origin)) {
            return;
        }
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Vary", "Origin");
    }

    bool authorized(const httplib::Request& req) const {
        if (options.token.empty()) {
            return true;
        }
        std::string given = req.get_header_value("X-Qstate-Token");
        if (given.empty()) {
            std::string auth = req.get_header_value("Authorization");
            if (startsWith(auth, "Bearer ")) {
                given = auth.substr(7);
            }
        }
        if (given.empty() && req.has_param("token")) {
            given = req.get_param_value("token");
        }
        return constantTimeEquals(given, options.token);
    }

    // Host / Origin / CORS preflight / token. Returns true when the request was answered here.
    bool guard(const httplib::Request& req, httplib::Response& res) {
        const bool loopbackBind = isLoopbackName(options.host);
        if (loopbackBind && !isLoopbackName(hostOf(req.get_header_value("Host")))) {
            setJson(res, 403, rpc::makeError(rpc::Code::InvalidParams, "forbidden host"));
            return true;
        }
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && !originAllowed(origin)) {
            // Same-origin requests of our own static UI carry a loopback origin and are allowed above.
            setJson(res, 403, rpc::makeError(rpc::Code::InvalidParams, "forbidden origin"));
            return true;
        }
        addCors(req, res);
        if (req.method == "OPTIONS") {
            res.status = origin.empty() ? 404 : 204;
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, X-Qstate-Token, Authorization");
            res.set_header("Access-Control-Max-Age", "600");
            return true;
        }
        const bool rpcPath = req.path == "/rpc" || req.path == "/events";
        if (rpcPath && !authorized(req)) {
            setJson(res, 401, rpc::makeError(rpc::Code::InvalidParams, "missing or wrong token"));
            return true;
        }
        return false;
    }

    void handleRpc(const httplib::Request& req, httplib::Response& res) {
        const std::string type = req.get_header_value("Content-Type");
        if (!startsWith(type, "application/json")) {
            setJson(res, 415, rpc::makeError(rpc::Code::InvalidParams, "Content-Type must be application/json"));
            return;
        }
        try {
            rpc::Request request = rpc::parseRequest(req.body);
            res.status = 200;
            res.set_content(rpc::serialize(dispatcher.dispatch(request.method, request.params, "http")), "application/json");
        } catch (const rpc::Error& e) {
            setJson(res, 400, rpc::makeError(e));
        } catch (const std::exception& e) {
            setJson(res, 500, rpc::makeError(rpc::Code::Internal, e.what()));
        }
    }

    static std::string frame(const std::string& name, const nlohmann::json& payload) {
        std::string data = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return "event: " + name + "\ndata: " + data + "\n\n";
    }

    void handleEvents(const httplib::Request&, httplib::Response& res) {
        auto stream = std::make_shared<Stream>();
        {
            std::lock_guard lock(streamsMutex);
            if (stopping || streams.size() >= options.maxEventStreams) {
                setJson(res, 503, rpc::makeError(rpc::Code::Internal, "too many event streams"));
                return;
            }
            streams.insert(stream);
        }
        // The sink only queues; it runs on the emitting thread.
        stream->subscription = events.subscribeScoped([stream](const std::string& name, const nlohmann::json& payload) {
            std::string text = frame(name, payload);
            {
                std::lock_guard lock(stream->mutex);
                if (stream->closed) {
                    return;
                }
                if (stream->queue.size() >= kMaxQueuedEvents) {
                    stream->queue.pop_front();
                    ++stream->dropped;
                }
                stream->queue.push_back(std::move(text));
            }
            stream->cv.notify_one();
        });

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        const auto heartbeat = options.heartbeat;
        res.set_chunked_content_provider(
            "text/event-stream",
            [stream, heartbeat, first = true](std::size_t, httplib::DataSink& sink) mutable {
                std::string out;
                if (first) {
                    first = false;
                    out = "retry: 2000\n: connected\n\n";
                } else {
                    std::unique_lock lock(stream->mutex);
                    stream->cv.wait_for(lock, heartbeat, [&] { return stream->closed || !stream->queue.empty(); });
                    if (stream->closed) {
                        return false;
                    }
                    if (stream->dropped > 0) {
                        out += ": dropped " + std::to_string(stream->dropped) + " events\n\n";
                        stream->dropped = 0;
                    }
                    for (auto& item : stream->queue) {
                        out += item;
                    }
                    stream->queue.clear();
                    if (out.empty()) {
                        out = ": heartbeat\n\n";
                    }
                }
                return sink.write(out.data(), out.size());
            },
            [this, stream](bool) {
                stream->subscription.reset(); // waits for a sink that is running right now
                {
                    std::lock_guard lock(stream->mutex);
                    stream->closed = true;
                }
                std::lock_guard lock(streamsMutex);
                streams.erase(stream);
            });
    }

    void closeStreams() {
        std::vector<std::shared_ptr<Stream>> snapshot;
        {
            std::lock_guard lock(streamsMutex);
            stopping = true;
            snapshot.assign(streams.begin(), streams.end());
        }
        for (auto& stream : snapshot) {
            {
                std::lock_guard lock(stream->mutex);
                stream->closed = true;
            }
            stream->cv.notify_all();
        }
    }

    void install() {
        http.set_payload_max_length(options.maxBodyBytes);
        // httplib's default is SO_REUSEPORT, which would let another local process share (and steal) our port.
        http.set_socket_options([](::socket_t sock) {
#ifdef _WIN32
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
        });
        http.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            return guard(req, res) ? httplib::Server::HandlerResponse::Handled
                                   : httplib::Server::HandlerResponse::Unhandled;
        });
        http.set_post_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            addCors(req, res);
        });
        http.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            setJson(res, 200, nlohmann::json{{"ok", true}});
        });
        if (options.enableRpc) {
            http.Post("/rpc", [this](const httplib::Request& req, httplib::Response& res) { handleRpc(req, res); });
            http.Get("/events", [this](const httplib::Request& req, httplib::Response& res) { handleEvents(req, res); });
        }
        if (!options.staticDir.empty()) {
            if (!http.set_mount_point("/", options.staticDir)) {
                throw std::runtime_error("httpd: cannot serve static directory: " + options.staticDir);
            }
        }
        http.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            setJson(res, 500, rpc::makeError(rpc::Code::Internal, "internal error"));
        });
    }
};

Server::Server(rpc::Dispatcher& dispatcher, rpc::EventBus& events, Options options)
    : impl_(std::make_unique<Impl>(dispatcher, events, std::move(options))) {}

Server::~Server() {
    stop();
}

int Server::start() {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->used) {
        throw std::logic_error("httpd::Server::start can be called only once");
    }
    impl_->used = true;
    if (!impl_->options.staticDir.empty() && !std::filesystem::is_directory(impl_->options.staticDir)) {
        throw std::runtime_error("httpd: static directory does not exist: " + impl_->options.staticDir);
    }
    impl_->install();
    int port = impl_->options.port;
    if (port == 0) {
        port = impl_->http.bind_to_any_port(impl_->options.host);
        if (port <= 0) {
            throw std::runtime_error("httpd: cannot bind " + impl_->options.host);
        }
    } else if (!impl_->http.bind_to_port(impl_->options.host, port)) {
        throw std::runtime_error("httpd: cannot bind " + impl_->options.host + ":" + std::to_string(port));
    }
    impl_->boundPort = port;
    impl_->started = true;
    impl_->thread = std::thread([impl = impl_.get()] { impl->http.listen_after_bind(); });
    impl_->http.wait_until_ready();
    return port;
}

void Server::stop() {
    std::lock_guard lock(impl_->lifecycle);
    if (!impl_->started.exchange(false)) {
        return;
    }
    impl_->closeStreams();
    impl_->http.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

int Server::port() const noexcept {
    return impl_->boundPort.load();
}

bool Server::running() const noexcept {
    return impl_->started.load();
}

} // namespace qstate::httpd
