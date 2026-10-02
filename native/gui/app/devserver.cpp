// qstate-devserver: the RPC backend without a window, for developing the UI in a normal browser
// (Vite dev server on another port + `?api=http://127.0.0.1:8787`). Needs no GTK / WebKit.
//
//   qstate-devserver [--serve <port>] [--ui-dir <dist>] [--core <dir> --ref <ref> --state <dir> --epoch <n>]
//                    [--token <t>] [--workers <n>]
//
// Same service, same dispatcher and event bus as the desktop app; only the transport differs.
#include "qstate/gui/options.h"
#include "qstate/httpd/server.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/service/service.h"

#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <atomic>
#include <chrono>
#include <thread>
#else
#include <pthread.h>
#endif

int main(int argc, char** argv) {
    using namespace qstate;
#if !defined(_WIN32)
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr); // before any thread exists
#endif
    gui::ParseResult parsed = gui::parseArgs(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed.error.empty() || parsed.options.help) {
        std::cerr << (parsed.error.empty() ? "" : "qstate-devserver: " + parsed.error + "\n\n") << gui::usage();
        return parsed.error.empty() ? 0 : 2;
    }
    const gui::Options& opt = parsed.options;

    rpc::Dispatcher dispatcher(opt.workers);
    rpc::EventBus events;
    service::ServiceConfig config;
    config.transport = "http";
    config.startup = opt.startup;
    service::Service service(config);
    service.registerAll(dispatcher, events);

    httpd::Options so;
    so.port = opt.servePort.value_or(8787);
    so.token = opt.token;
    so.staticDir = opt.uiDir;
    httpd::Server server(dispatcher, events, so);
    int port = 0;
    try {
        port = server.start();
    } catch (const std::exception& e) {
        std::cerr << "qstate-devserver: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "qstate-devserver: listening on http://127.0.0.1:" << port << "/  (POST /rpc, GET /events)\n"
              << "  UI in a browser: http://localhost:5173/?api=http://127.0.0.1:" << port
              << "   (pnpm --dir ui dev)\n";

#if defined(_WIN32)
    std::atomic<bool> never{false};
    while (!never) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#else
    int sig = 0;
    sigwait(&signals, &sig);
    std::cerr << "qstate-devserver: signal " << sig << ", stopping\n";
#endif
    server.stop();
    dispatcher.shutdown();
    return 0;
}
