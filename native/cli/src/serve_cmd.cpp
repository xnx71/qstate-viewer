#include "commands.h"
#include "qstate/httpd/server.h"
#include "session.h"

#include <chrono>
#include <ostream>
#include <thread>

namespace qstate::cli {

int cmdServe(const Args& args, std::ostream& out, std::ostream& err, const std::atomic<bool>* stop) {
    const long long port = args.integer("port").value_or(8787);
    if (port < 0 || port > 65535) throw UsageError("--port must be between 0 and 65535");
    Session session(/*watch=*/true, startupFrom(args));
    httpd::Options options;
    options.port = static_cast<int>(port);
    options.staticDir = args.get("ui-dir").value_or("");
    httpd::Server server(session.dispatcher(), session.events(), options);
    int bound = 0;
    try {
        bound = server.start();
    } catch (const std::exception& e) {
        err << "qstate-cli: " << e.what() << "\n";
        return 2;
    }
    out << "qstate-cli: listening on http://127.0.0.1:" << bound << "/  (POST /rpc, GET /events)\n" << std::flush;
    while (stop == nullptr || !stop->load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();
    session.dispatcher().shutdown();
    return 0;
}

} // namespace qstate::cli
