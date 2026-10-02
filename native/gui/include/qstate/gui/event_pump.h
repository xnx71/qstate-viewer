// Batches EventBus events for delivery to the UI thread.
#pragma once

#include "qstate/gui/bridge.h"
#include "qstate/rpc/event_bus.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace qstate::gui {

struct PumpOptions {
    // Events arriving within this window are delivered in one batch (at most 1000 / interval batches per second).
    std::chrono::milliseconds interval{20};
    // For these events only the newest of a batch survives (the payload is a full snapshot).
    std::set<std::string> latestWins{"workspace.updated"};
    // Safety valve: when the UI cannot keep up, the oldest events beyond this many are dropped.
    std::size_t maxQueued = 4096;
};

// Subscribes to the bus, collects events on a private thread and hands each batch to `deliver` as ONE script.
// `deliver` runs on the pump thread: the webview host implements it with webview::dispatch + eval.
//
// Thread safety: construct / destroy from one thread; the sink part is called from any thread (EventBus
// semantics). The destructor unsubscribes, drops pending events and joins: `deliver` is never called after
// it returns, so everything `deliver` references must outlive the pump.
class EventPump {
public:
    using Deliver = std::function<void(std::string script)>;

    EventPump(rpc::EventBus& bus, Deliver deliver, PumpOptions options = {});
    ~EventPump();
    EventPump(const EventPump&) = delete;
    EventPump& operator=(const EventPump&) = delete;

    // Number of events dropped because of maxQueued.
    std::size_t dropped() const;

private:
    void run();
    void push(const std::string& name, const nlohmann::json& payload);

    Deliver deliver_;
    PumpOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<QueuedEvent> queue_;
    std::size_t dropped_ = 0;
    bool stop_ = false;
    rpc::EventBus::Subscription subscription_;
    std::thread thread_;
};

} // namespace qstate::gui
