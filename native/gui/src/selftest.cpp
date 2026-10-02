#include "qstate/gui/selftest.h"

#include "qstate/rpc/params.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace qstate::gui {

SelftestSession::SelftestSession(rpc::Dispatcher& dispatcher, rpc::EventBus& events) {
    using nlohmann::json;
    dispatcher.registerMethod("selftest.echo", [](const json& params, rpc::CallContext&) {
        auto it = params.find("value");
        return it == params.end() ? json(nullptr) : *it;
    });
    dispatcher.registerMethod("selftest.slow", [](const json& params, rpc::CallContext& ctx) {
        auto ms = rpc::requireParam<int>(params, "ms");
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            ctx.throwIfCancelled();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return json("done");
    });
    dispatcher.registerMethod("selftest.fail", [](const json&, rpc::CallContext&) -> json {
        throw rpc::Error(rpc::Code::InvalidParams, "expected failure", json{{"detail", 42}});
    });
    dispatcher.registerMethod("selftest.blob", [](const json& params, rpc::CallContext&) {
        auto bytes = rpc::requireParam<std::size_t>(params, "bytes");
        if (bytes > (std::size_t(64) << 20)) {
            throw rpc::Error(rpc::Code::InvalidParams, "bytes too large");
        }
        // Printable, non-repeating-ish content (like a hex dump), so nothing is trivially compressible.
        std::string text(bytes, 'a');
        unsigned state = 12345;
        for (char& c : text) {
            state = state * 1664525u + 1013904223u;
            c = static_cast<char>('a' + ((state >> 24) % 26));
        }
        return json(std::move(text));
    });
    dispatcher.registerMethod("selftest.emit", [&events](const json& params, rpc::CallContext&) {
        auto name = rpc::requireParam<std::string>(params, "name");
        auto it = params.find("payload");
        events.emit(name, it == params.end() ? json(nullptr) : *it);
        return json(nullptr);
    });
    dispatcher.registerMethod("selftest.report", [this](const json& params, rpc::CallContext&) {
        {
            std::lock_guard lock(mutex_);
            report_ = params;
            passed_ = params.value("ok", false);
            reported_ = true;
        }
        cv_.notify_all();
        return json(nullptr);
    });
}

bool SelftestSession::waitForReport(int timeoutSec) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(timeoutSec), [this] { return reported_; });
}

bool SelftestSession::reported() const {
    std::lock_guard lock(mutex_);
    return reported_;
}

bool SelftestSession::passed() const {
    std::lock_guard lock(mutex_);
    return reported_ && passed_;
}

std::string SelftestSession::summary() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    if (!reported_) {
        out << "selftest: no report received from the page\n";
        return out.str();
    }
    out << "selftest: " << (passed_ ? "PASS" : "FAIL") << "\n";
    for (const auto& check : report_.value("checks", nlohmann::json::array())) {
        out << "  [" << (check.value("ok", false) ? "ok" : "FAIL") << "] " << check.value("name", std::string("?"));
        std::string detail = check.value("detail", std::string());
        if (!detail.empty()) {
            out << ": " << detail;
        }
        out << "\n";
    }
    auto timings = report_.find("timings");
    if (timings != report_.end() && timings->is_object() && !timings->empty()) {
        out << "  timings:\n";
        for (auto it = timings->begin(); it != timings->end(); ++it) {
            out << "    " << it.key() << ": " << it.value().dump() << "\n";
        }
    }
    return out.str();
}

} // namespace qstate::gui
