// Command line of qstate-viewer.
#pragma once

#include "qstate/service/service.h"

#include <optional>
#include <string>
#include <vector>

namespace qstate::gui {

struct Options {
    // --core <dir> --ref <ref> --state <dir> --epoch <n>: forwarded to AppInfo.startup.
    service::StartupRequest startup;
    // --dev-url <url>: navigate to a dev server (Vite) instead of the embedded UI.
    std::string devUrl;
    // --ui-dir <dir>: serve a built dist directory through a loopback HTTP server and load it from there.
    std::string uiDir;
    // --serve <port>: ALSO start the HTTP/SSE transport (port 0 = pick one) so a normal browser can be used.
    std::optional<int> servePort;
    // --token <t>: shared secret required by the --serve transport.
    std::string token;
    std::string title = "Qubic State Viewer";
    int width = 1360;
    int height = 860;
    // --debug: enable the web inspector of the webview.
    bool debug = false;
    // --workers <n>: size of the RPC worker pool.
    unsigned workers = 4;
    // --headless-selftest: load the built-in test page, check the bridge, exit 0 / 1.
    bool selftest = false;
    // --selftest-bench: additionally time 1 / 5 / 20 MB results through the bridge.
    bool selftestBench = false;
    // --selftest-hold <ms>: keep the window open this long after the verdict (for screenshots).
    int selftestHoldMs = 0;
    // --selftest-timeout <s>
    int selftestTimeoutSec = 30;
    // --selftest-script <file.js>: development / CI hook. The script is injected into the UI page (before the page's own
    // scripts, on every load) and can use window.__qstate_log(text) (printed to stderr as "[page] text") and
    // window.__qstate_exit(code) (closes the window; becomes the process exit code). It drives the REAL UI inside the
    // real webview, see ui/scripts/webview-e2e.js and scripts/webview-e2e.sh.
    std::string scriptFile;
    bool help = false;
    bool version = false;
};

struct ParseResult {
    Options options;
    // Non-empty when the arguments are invalid.
    std::string error;
};

// `args` excludes argv[0].
ParseResult parseArgs(const std::vector<std::string>& args);

std::string usage();

} // namespace qstate::gui
