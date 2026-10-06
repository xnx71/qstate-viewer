// Memory behaviour of the host process (docs/MEMORY.md).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace qstate::gui {

// Call first thing in main(), before any thread exists. glibc: allocations of 256 KiB and more are served by mmap and go
// straight back to the OS when freed (the default threshold floats up to 32 MiB after the first big free, which kept every
// sorted table order and parse buffer in the heap), and at most 2 malloc arenas exist (the worker pool otherwise leaves a
// fragmented arena per thread). Measured on the real epoch 229 files: 202 MB -> 84 MB after browsing the large tables.
// Other platforms: nothing (the Windows heap returns large blocks by itself).
void tuneAllocator();

// "Has the host been idle for `idle`?" without a clock of its own: due() answers true ONCE per idle period (any activity()
// re-arms it). Thread safe. The host calls Service::trimMemory when it is due.
class IdleTrigger {
public:
    using Clock = std::chrono::steady_clock;

    explicit IdleTrigger(std::chrono::milliseconds idle, Clock::time_point now = Clock::now())
        : idle_(idle), lastNs_(toNs(now)) {}

    // A request arrived or finished.
    void activity(Clock::time_point now = Clock::now()) {
        lastNs_.store(toNs(now), std::memory_order_relaxed);
        armed_.store(true, std::memory_order_release);
    }

    // True once when the last activity is `idle` or more in the past; false again until the next activity().
    bool due(Clock::time_point now = Clock::now()) {
        if (!armed_.load(std::memory_order_acquire)) return false;
        if (toNs(now) - lastNs_.load(std::memory_order_relaxed) < std::chrono::duration_cast<std::chrono::nanoseconds>(idle_).count()) return false;
        return armed_.exchange(false, std::memory_order_acq_rel);
    }

private:
    static std::int64_t toNs(Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }
    std::chrono::milliseconds idle_;
    std::atomic<std::int64_t> lastNs_;
    std::atomic<bool> armed_{true};
};

} // namespace qstate::gui
