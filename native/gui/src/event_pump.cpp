#include "qstate/gui/event_pump.h"

#include <algorithm>

namespace qstate::gui {

EventPump::EventPump(rpc::EventBus& bus, Deliver deliver, PumpOptions options)
    : deliver_(std::move(deliver)), options_(std::move(options)) {
    thread_ = std::thread([this] { run(); });
    subscription_ = bus.subscribeScoped(
        [this](const std::string& name, const nlohmann::json& payload) { push(name, payload); });
}

EventPump::~EventPump() {
    subscription_.reset(); // no new events; waits for a sink call in progress
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::size_t EventPump::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

void EventPump::push(const std::string& name, const nlohmann::json& payload) {
    // Serialize outside the lock: the payload may be large.
    QueuedEvent event{name, payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
    {
        std::lock_guard lock(mutex_);
        if (stop_) {
            return;
        }
        if (options_.latestWins.count(name) != 0) {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                        [&](const QueuedEvent& queued) { return queued.name == name; }),
                         queue_.end());
        }
        if (queue_.size() >= options_.maxQueued) {
            queue_.erase(queue_.begin());
            ++dropped_;
        }
        queue_.push_back(std::move(event));
    }
    cv_.notify_one();
}

void EventPump::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) {
            return;
        }
        // Let a burst accumulate, then deliver it as one batch.
        cv_.wait_for(lock, options_.interval, [this] { return stop_; });
        if (stop_) {
            return;
        }
        std::vector<QueuedEvent> batch;
        batch.swap(queue_);
        lock.unlock();
        if (!batch.empty()) {
            try {
                deliver_(makeEmitScript(batch));
            } catch (...) {
            }
        }
        lock.lock();
    }
}

} // namespace qstate::gui
