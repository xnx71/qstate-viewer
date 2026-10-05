// qstate-viewer: the desktop host. A window with the system webview that shows the React UI and gives it the
// two bridge functions of ui/src/rpc/contract.ts:
//
//   window.__qstate_invoke(method, params) -> Promise   (bound here; runs on the dispatcher's worker pool)
//   window.__qstate_emit(name, payload)                 (defined by the UI; called here with batched events)
//
// All logic lives in qstate_service / qstate_rpc / qstate_gui; this file only wires them to the webview.
//
// The executable takes no command line arguments. Test hooks (environment variables; not for users, documented in
// docs/HOST.md "Testing"):
//   QSTATE_SELFTEST=1              load the built-in bridge test page instead of the UI; exit status 0 / 1 = verdict
//   QSTATE_SELFTEST_HOLD_MS=<ms>   with QSTATE_SELFTEST: keep the window open this long after the verdict (screenshots)
//   QSTATE_SELFTEST_SCRIPT=<file>  inject this JavaScript file into the UI page before the page's own scripts; it can
//                                  call window.__qstate_log(text) and window.__qstate_exit(code) (real-webview e2e)
//   QSTATE_CONFIG_DIR=<dir>        settings.json lives here instead of the user's configuration directory
//   QSTATE_CACHE_DIR=<dir>         git mirrors and exported core sources live here instead of the user's cache directory
//   QSTATE_DEBUG=1                 enable the web inspector of the webview
// Threading: the webview must be used from the main thread only, except webview::dispatch / terminate / resolve
// which are thread-safe. Worker threads therefore only call w.resolve() and w.dispatch().
// Shutdown order matters (see the end of main): everything that may call into the webview is stopped first.

#include "qstate/gui/assets.h"
#include "qstate/gui/bridge.h"
#include "qstate/gui/event_pump.h"
#include "qstate/gui/selftest.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/framing.h"
#include "qstate/service/service.h"

#include <webview/webview.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <pthread.h>
#endif

#ifndef QSTATE_VERSION_STRING
#define QSTATE_VERSION_STRING "0.0.0"
#endif

namespace {

using namespace qstate;

// Joinable helper thread that can be asked to stop and woken up.
class StopSignal {
public:
    void request() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
    }
    bool requested() const {
        std::lock_guard lock(mutex_);
        return stop_;
    }
    // True when stop was requested within the timeout.
    bool waitFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return stop_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

#if !defined(_WIN32)
// SIGINT / SIGTERM must not run arbitrary code in a signal handler (the webview is not async-signal-safe).
// They are blocked in every thread (this is called first thing in main, before any thread exists) and
// collected by a dedicated thread with sigtimedwait.
sigset_t blockTerminationSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    return set;
}
#endif

std::string envString(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

bool envFlag(const char* name) {
    const std::string value = envString(name);
    return !value.empty() && value != "0";
}

constexpr const char* kTitle = "Qubic State Viewer";
constexpr int kWidth = 1360;
constexpr int kHeight = 860;
constexpr unsigned kWorkers = 4;
constexpr int kSelftestTimeoutSec = 60;

} // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    const sigset_t termSignals = blockTerminationSignals();
#endif
    if (argc > 1) {
        std::cerr << "qstate-viewer " << QSTATE_VERSION_STRING << ": this program takes no arguments\n";
        return 2;
    }
    const bool selftestMode = envFlag("QSTATE_SELFTEST");
    const std::string scriptFile = envString("QSTATE_SELFTEST_SCRIPT");
    const std::string holdText = envString("QSTATE_SELFTEST_HOLD_MS");
    const int selftestHoldMs = holdText.empty() ? 0 : (std::max)(0, std::atoi(holdText.c_str()));

    // ---- backend -----------------------------------------------------------------------------------------
    rpc::Dispatcher dispatcher(kWorkers);
    rpc::EventBus events;
    service::ServiceConfig serviceConfig;
    if (const std::string dir = envString("QSTATE_CONFIG_DIR"); !dir.empty()) serviceConfig.settingsPath = dir + "/settings.json";
    serviceConfig.cacheDir = envString("QSTATE_CACHE_DIR");
    service::Service service(serviceConfig);
    service.registerAll(dispatcher, events);

    std::unique_ptr<gui::SelftestSession> selftest;
    if (selftestMode) {
        selftest = std::make_unique<gui::SelftestSession>(dispatcher, events);
    }

    // ---- window ------------------------------------------------------------------------------------------------
    std::unique_ptr<webview::webview> window;
    try {
        window = std::make_unique<webview::webview>(envFlag("QSTATE_DEBUG"), nullptr);
    } catch (const std::exception& e) {
        std::cerr << "qstate-viewer: cannot create the webview window: " << e.what()
                  << "\n  (is a display available? under CI use xvfb-run; see docs/HOST.md)\n";
        return 3;
    }
    webview::webview& w = *window;
    w.set_title(kTitle);
    w.set_size(kWidth, kHeight, WEBVIEW_HINT_NONE);

    // window.__qstate_invoke(method, params) -> Promise. The request is parsed on the UI thread (small); the
    // handler runs on a pool thread, which resolves the promise (w.resolve is thread-safe).
    w.bind(
        gui::kInvokeBinding,
        [&w, &dispatcher](const std::string& id, const std::string& req, void*) {
            gui::InvokeCall call;
            std::string error;
            if (!gui::parseInvokeArgs(req, call, error)) {
                gui::BridgeReply reply = gui::makeReply(rpc::makeError(rpc::Code::InvalidParams, error));
                w.resolve(id, reply.status, reply.json);
                return;
            }
            dispatcher.dispatchAsync(
                call.method, std::move(call.params),
                [&w, id](nlohmann::json response) {
                    gui::BridgeReply reply = gui::makeReply(response);
                    w.resolve(id, reply.status, reply.json);
                });
        },
        nullptr);

    // QSTATE_SELFTEST_SCRIPT: a script injected into the page that drives the real UI (CI hook).
    std::atomic<int> scriptExitCode{0};
    if (!scriptFile.empty()) {
        std::ifstream in(scriptFile, std::ios::binary);
        if (!in) {
            std::cerr << "qstate-viewer: cannot read " << scriptFile << "\n";
            return 2;
        }
        std::stringstream text;
        text << in.rdbuf();
        w.bind(
            "__qstate_log",
            [&w](const std::string& id, const std::string& req, void*) {
                // req is a JSON array of the call arguments: ["text"]
                auto call = nlohmann::json::parse(req, nullptr, false);
                const std::string msg = call.is_array() && !call.empty() && call[0].is_string() ? call[0].get<std::string>() : req;
                std::cerr << "[page] " << msg << std::endl;
                w.resolve(id, 0, "null");
            },
            nullptr);
        w.bind(
            "__qstate_exit",
            [&w, &scriptExitCode](const std::string& id, const std::string& req, void*) {
                auto call = nlohmann::json::parse(req, nullptr, false);
                scriptExitCode = call.is_array() && !call.empty() && call[0].is_number_integer() ? call[0].get<int>() : 1;
                w.resolve(id, 0, "null");
                w.dispatch([&w] { w.terminate(); });
            },
            nullptr);
        w.init(text.str());
    }

    // Events: batched on the pump thread, evaluated on the UI thread.
    auto pump = std::make_unique<gui::EventPump>(events, [&w](std::string script) {
        w.dispatch([&w, script = std::move(script)] { w.eval(script); });
    });

    if (selftest) {
        w.set_html(std::string(gui::selftestPageHtml()));
    } else {
        if (gui::embeddedIndexIsPlaceholder()) {
            std::cerr << "qstate-viewer: note: this build embeds the placeholder page (ui/dist/index.html did not exist at "
                         "build time). Run `pnpm build` in ui/ and rebuild.\n";
        }
        w.set_html(std::string(gui::embeddedIndexHtml()));
    }

    // ---- helper threads: termination signals, self test supervision --------------------------------------------
    StopSignal stopHelpers;
    auto terminate = [&w] { w.dispatch([&w] { w.terminate(); }); };

    std::thread signalThread;
#if !defined(_WIN32)
    signalThread = std::thread([&] {
        while (!stopHelpers.requested()) {
            timespec timeout{0, 200 * 1000 * 1000};
            const int sig = sigtimedwait(&termSignals, nullptr, &timeout);
            if (sig > 0) {
                std::cerr << "qstate-viewer: signal " << sig << ", closing\n";
                terminate();
                return;
            }
        }
    });
#endif

    std::atomic<bool> selftestTimedOut{false};
    std::thread selftestThread;
    if (selftest) {
        selftestThread = std::thread([&] {
            const bool reported = selftest->waitForReport(kSelftestTimeoutSec);
            if (!reported) {
                selftestTimedOut = true;
                std::cerr << "qstate-viewer: selftest TIMEOUT after " << kSelftestTimeoutSec << " s\n";
            } else if (selftestHoldMs > 0) {
                stopHelpers.waitFor(std::chrono::milliseconds(selftestHoldMs));
            }
            terminate();
            if (!reported) {
                // The UI thread may be wedged: do not hang CI forever.
                if (!stopHelpers.waitFor(std::chrono::seconds(10))) {
                    std::cerr << "qstate-viewer: window did not close, aborting\n";
                    std::_Exit(1);
                }
            }
        });
    }

    // ---- run ---------------------------------------------------------------------------------------------------
    w.run();

    // ---- shutdown: stop everything that can touch the webview, then destroy it -----------------------------------
    stopHelpers.request();
    if (signalThread.joinable()) {
        signalThread.join();
    }
    if (selftestThread.joinable()) {
        selftestThread.join();
    }
    pump.reset();                // no more dispatch() of events
    dispatcher.shutdown();       // cancels and joins the workers: no resolve() after this point
    window.reset();              // now the webview can go

    if (selftest) {
        std::cout << selftest->summary() << std::flush;
        return (selftest->passed() && !selftestTimedOut) ? 0 : 1;
    }
    return scriptExitCode.load();
}
