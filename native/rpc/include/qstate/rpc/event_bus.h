// Fan-out of native -> UI events to the registered sinks (the webview host).
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qstate::rpc {

// Thread safety: every member function may be called from any thread concurrently.
//
// * emit() calls each sink synchronously on the emitting thread. Calls into ONE sink are serialized (a sink
//   never runs concurrently with itself, so it needs no locking of its own), calls into different sinks may
//   overlap. Events emitted concurrently from several threads arrive in unspecified order; events emitted
//   by one thread arrive in emission order.
// * Sinks must be quick (queue the event and return): a slow sink delays the emitter. Exceptions thrown by
//   a sink are swallowed. A sink may emit, subscribe, and unsubscribe itself; it must not unsubscribe a different sink
//   (two sinks doing that to each other on different threads would deadlock).
// * After unsubscribe() / Subscription::reset() returns, the sink is not running and will never run again
//   (unless called from inside the sink itself), so state captured by the sink may be destroyed.
class EventBus {
public:
    using Sink = std::function<void(const std::string& name, const nlohmann::json& payload)>;
    using Id = std::uint64_t;

    // RAII handle; unsubscribes on destruction. Must not outlive the EventBus.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(EventBus& bus, Id id) : bus_(&bus), id_(id) {}
        Subscription(Subscription&& other) noexcept : bus_(other.bus_), id_(other.id_) { other.bus_ = nullptr; }
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { reset(); }

        void reset();
        explicit operator bool() const noexcept { return bus_ != nullptr; }

    private:
        EventBus* bus_ = nullptr;
        Id id_ = 0;
    };

    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    Id subscribe(Sink sink);
    // Idempotent. Blocks while the sink is running on another thread.
    void unsubscribe(Id id);
    Subscription subscribeScoped(Sink sink) { return Subscription(*this, subscribe(std::move(sink))); }

    void emit(const std::string& name, const nlohmann::json& payload) const;

    std::size_t subscriberCount() const;

private:
    struct Entry {
        Id id = 0;
        Sink sink;
        std::recursive_mutex running; // held while the sink executes
        bool removed = false;         // guarded by `running`
    };

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Entry>> entries_;
    Id nextId_ = 1;
};

} // namespace qstate::rpc
