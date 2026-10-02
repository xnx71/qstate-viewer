#include "qstate/rpc/dispatcher.h"

#include "qstate/rpc/framing.h"

#include <algorithm>

namespace qstate::rpc {

namespace {

nlohmann::json cancelledResponse() {
    return makeError(Code::Internal, "cancelled");
}

} // namespace

Dispatcher::Dispatcher(unsigned workers) : workerCount_(std::max(1u, workers)) {}

Dispatcher::~Dispatcher() {
    shutdown();
}

void Dispatcher::registerMethod(const std::string& name, Handler handler) {
    std::unique_lock lock(methodsMutex_);
    methods_[name] = std::move(handler);
}

bool Dispatcher::has(const std::string& method) const {
    std::shared_lock lock(methodsMutex_);
    return methods_.find(method) != methods_.end();
}

std::vector<std::string> Dispatcher::methods() const {
    std::vector<std::string> names;
    {
        std::shared_lock lock(methodsMutex_);
        names.reserve(methods_.size());
        for (const auto& entry : methods_) {
            names.push_back(entry.first);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

nlohmann::json Dispatcher::dispatch(const std::string& method, const nlohmann::json& params) const {
    CallContext ctx(method, std::make_shared<CancelToken>());
    return dispatch(method, params, ctx);
}

nlohmann::json Dispatcher::dispatch(const std::string& method, const nlohmann::json& params, CallContext& ctx) const {
    try {
        Handler handler;
        {
            std::shared_lock lock(methodsMutex_);
            auto it = methods_.find(method);
            if (it != methods_.end()) {
                handler = it->second; // copy: the handler may be replaced while it runs
            }
        }
        if (!handler) {
            return makeError(Code::UnknownMethod, "unknown method: " + method);
        }
        return makeResult(handler(params, ctx));
    } catch (const Error& e) {
        return makeError(e);
    } catch (const Cancelled&) {
        return cancelledResponse();
    } catch (const std::exception& e) {
        return makeError(Code::Internal, e.what());
    } catch (...) {
        return makeError(Code::Internal, "unknown failure");
    }
}

std::shared_ptr<CancelToken> Dispatcher::dispatchAsync(const std::string& method, nlohmann::json params,
                                                       Callback callback) {
    auto token = std::make_shared<CancelToken>();
    Job job{method, std::move(params), std::move(callback), token};
    bool rejected = false;
    {
        std::lock_guard lock(poolMutex_);
        if (stopping_) {
            rejected = true;
        } else {
            queue_.push_back(std::move(job));
        }
    }
    if (rejected) {
        token->cancel();
        // `job` is only moved from when it is queued, so it is intact here.
        if (job.callback) {
            try {
                job.callback(cancelledResponse());
            } catch (...) {
            }
        }
        return token;
    }
    ensureWorkers();
    poolCv_.notify_one();
    return token;
}

void Dispatcher::ensureWorkers() {
    std::lock_guard lock(poolMutex_);
    if (started_ || stopping_) {
        return;
    }
    started_ = true;
    workers_.reserve(workerCount_);
    for (unsigned i = 0; i < workerCount_; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void Dispatcher::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(poolMutex_);
            poolCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return; // stopping and drained
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            if (!job.token->cancelled()) {
                running_.push_back(job.token);
            }
        }
        nlohmann::json response;
        if (job.token->cancelled()) {
            response = cancelledResponse();
        } else {
            CallContext ctx(job.method, job.token);
            response = dispatch(job.method, job.params, ctx);
            std::lock_guard lock(poolMutex_);
            running_.erase(std::remove(running_.begin(), running_.end(), job.token), running_.end());
        }
        if (job.callback) {
            try {
                job.callback(std::move(response));
            } catch (...) {
            }
        }
    }
}

void Dispatcher::shutdown() {
    std::lock_guard shutdownLock(shutdownMutex_); // a second caller waits until the first has joined
    std::vector<std::thread> workers;
    {
        std::lock_guard lock(poolMutex_);
        stopping_ = true;
        for (auto& job : queue_) {
            job.token->cancel();
        }
        for (auto& token : running_) {
            token->cancel();
        }
        workers.swap(workers_);
    }
    poolCv_.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace qstate::rpc
