// Transport independent method dispatcher with an optional worker pool.
#pragma once

#include "qstate/rpc/error.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace qstate::rpc {

// Cooperative cancellation flag shared between the caller of dispatchAsync() and the running handler.
class CancelToken {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }
    // The flag itself, for APIs that poll a `const std::atomic<bool>*` (e.g. the decoder's Query::cancel).
    const std::atomic<bool>& flag() const noexcept { return cancelled_; }

private:
    std::atomic<bool> cancelled_{false};
};

// Thrown by CallContext::throwIfCancelled(); reported as Code::Internal "cancelled".
class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("cancelled") {}
};

// Passed to every handler.
class CallContext {
public:
    CallContext(std::string method, std::shared_ptr<CancelToken> token) : method_(std::move(method)), token_(std::move(token)) {}

    const std::string& method() const noexcept { return method_; }
    // True when the caller gave up (dispatchAsync token cancelled) or the dispatcher is shutting down.
    // Long running handlers should poll it (or call throwIfCancelled) between chunks of work.
    bool cancelled() const noexcept { return token_->cancelled(); }
    // Pointer to the cancellation flag; valid as long as this context lives.
    const std::atomic<bool>* cancelFlag() const noexcept { return &token_->flag(); }
    void throwIfCancelled() const {
        if (cancelled()) {
            throw Cancelled();
        }
    }

private:
    std::string method_;
    std::shared_ptr<CancelToken> token_;
};

// Response object: {"result": ...} or {"error": {code, message, data?}}.
//
// Thread safety: registerMethod / has / dispatch / dispatchAsync / shutdown may be called from any thread.
// Handlers run concurrently when dispatched from several threads: they must be thread-safe.
// Registration is normally done once before dispatching, but it is safe at any time.
class Dispatcher {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json& params, CallContext& ctx)>;
    // Called exactly once per dispatchAsync(), on a worker thread (or on the calling thread when the
    // dispatcher is already shut down). Must not throw (exceptions are swallowed).
    using Callback = std::function<void(nlohmann::json response)>;

    // `workers` threads are started lazily on the first dispatchAsync() call.
    explicit Dispatcher(unsigned workers = 4);
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Replaces a handler registered under the same name.
    void registerMethod(const std::string& name, Handler handler);
    bool has(const std::string& method) const;
    std::vector<std::string> methods() const;

    // Runs the handler on the calling thread. Never throws. Unknown method => "unknown_method";
    // rpc::Error => its code; anything else => "internal".
    nlohmann::json dispatch(const std::string& method, const nlohmann::json& params) const;
    nlohmann::json dispatch(const std::string& method, const nlohmann::json& params, CallContext& ctx) const;

    // Queues the call for the worker pool and returns immediately. The callback receives the response.
    // Ordering between calls is not guaranteed (a pool runs them concurrently).
    // Cancelling the returned token makes ctx.cancelled() true; a call cancelled before it started is not
    // run: the callback receives {"error": {"code": "internal", "message": "cancelled"}}.
    std::shared_ptr<CancelToken> dispatchAsync(const std::string& method, nlohmann::json params, Callback callback);

    // Cancels queued and running calls, rejects the queued ones through their callbacks, joins the workers.
    // After it returns dispatchAsync() answers every call with the "cancelled" error synchronously.
    // Idempotent; called by the destructor. Must not be called from inside a handler.
    void shutdown();

private:
    struct Job {
        std::string method;
        nlohmann::json params;
        Callback callback;
        std::shared_ptr<CancelToken> token;
    };

    void workerLoop();
    void ensureWorkers();

    unsigned workerCount_;

    mutable std::shared_mutex methodsMutex_;
    std::unordered_map<std::string, Handler> methods_;

    std::mutex shutdownMutex_;
    std::mutex poolMutex_;
    std::condition_variable poolCv_;
    std::deque<Job> queue_;
    std::vector<std::thread> workers_;
    std::vector<std::shared_ptr<CancelToken>> running_; // tokens of calls in flight
    bool started_ = false;
    bool stopping_ = false;
};

} // namespace qstate::rpc
