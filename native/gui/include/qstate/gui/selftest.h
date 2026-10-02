// Server side of the bridge self test (QSTATE_SELFTEST=1): a few extra RPC methods used by the built-in test page
// (src/selftest_page.cpp) and the verdict the page reports.
#pragma once

#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"

#include <condition_variable>
#include <mutex>
#include <string>

namespace qstate::gui {

// Registered methods (all prefixed "selftest."):
//   echo {value}                   -> value
//   slow {ms}                      -> "done" after sleeping ms (cancellable)
//   fail {}                        -> rpc::Error(invalid_params, "expected failure", {"detail": 42})
//   blob {bytes}                   -> string of that many characters
//   emit {name, payload}           -> null; emits the event on the bus (from the worker thread)
//   report {ok, checks, timings}   -> null; stores the verdict
//
// Thread safety: all members may be called from any thread.
class SelftestSession {
public:
    SelftestSession(rpc::Dispatcher& dispatcher, rpc::EventBus& events);

    // Blocks until the page reported or `timeoutSec` elapsed. Returns true when a report arrived.
    bool waitForReport(int timeoutSec);
    bool reported() const;
    bool passed() const;
    // Human readable summary (checks and timings) for stdout.
    std::string summary() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool reported_ = false;
    bool passed_ = false;
    nlohmann::json report_;
};

} // namespace qstate::gui
