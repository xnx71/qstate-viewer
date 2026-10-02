#include "qstate/rpc/event_bus.h"

#include <algorithm>

namespace qstate::rpc {

EventBus::Subscription& EventBus::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        bus_ = other.bus_;
        id_ = other.id_;
        other.bus_ = nullptr;
    }
    return *this;
}

void EventBus::Subscription::reset() {
    if (bus_ != nullptr) {
        bus_->unsubscribe(id_);
        bus_ = nullptr;
    }
}

EventBus::Id EventBus::subscribe(Sink sink) {
    auto entry = std::make_shared<Entry>();
    entry->sink = std::move(sink);
    std::lock_guard lock(mutex_);
    entry->id = nextId_++;
    entries_.push_back(entry);
    return entry->id;
}

void EventBus::unsubscribe(Id id) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [id](const auto& e) { return e->id == id; });
        if (it == entries_.end()) {
            return;
        }
        entry = std::move(*it);
        entries_.erase(it);
    }
    // Wait for a running invocation (recursive: unsubscribing from inside the sink itself is fine).
    std::lock_guard running(entry->running);
    entry->removed = true;
}

void EventBus::emit(const std::string& name, const nlohmann::json& payload) const {
    std::vector<std::shared_ptr<Entry>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = entries_;
    }
    for (const auto& entry : snapshot) {
        std::lock_guard running(entry->running);
        if (entry->removed) {
            continue;
        }
        try {
            entry->sink(name, payload);
        } catch (...) {
            // A misbehaving sink must not break the emitter or the other sinks.
        }
    }
}

std::size_t EventBus::subscriberCount() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

} // namespace qstate::rpc
