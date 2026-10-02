#include "qstate/gui/options.h"

#include <charconv>
#include <sstream>

namespace qstate::gui {

namespace {

bool parseInt(const std::string& text, long long& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc() && ptr == end;
}

} // namespace

std::string usage() {
    return R"(qstate-viewer: viewer for Qubic smart-contract state files

usage: qstate-viewer [options]

workspace (forwarded to the UI as app.info.startup):
  --core <dir>        Qubic core repository (contains src/contract_core/contract_def.h)
  --ref <ref>         git ref (tag / branch / sha / "auto") to read the core sources from
  --state <dir>       directory with contractNNNN.EEE files
  --epoch <n>         epoch to show when the directory holds several

UI source (default: the UI embedded in the executable):
  --dev-url <url>     load a dev server, e.g. http://localhost:5173 (Vite); the webview bridge stays available
  --ui-dir <dir>      load a built UI directory (ui/dist) through a loopback HTTP server

transports:
  --serve <port>      ALSO serve the HTTP + SSE transport on 127.0.0.1:<port> (0 = any free port), for a normal
                      browser; with --ui-dir the same server serves the UI
  --token <secret>    require this token (X-Qstate-Token header or ?token=) on /rpc and /events

window / misc:
  --title <text>      window title
  --size <WxH>        initial window size (default 1360x860)
  --workers <n>       RPC worker threads (default 4)
  --debug             enable the web inspector
  --headless-selftest run the built-in bridge self test and exit 0 (pass) / 1 (fail); run under xvfb in CI
  --selftest-bench    also time 1 / 5 / 20 MB results through the bridge
  --selftest-hold <ms>    keep the window open after the verdict (screenshots)
  --selftest-timeout <s>  fail the self test after this long (default 30)
  --selftest-script <file.js>  inject a script into the UI page that drives it (window.__qstate_log / __qstate_exit);
                      development / CI hook, see scripts/webview-e2e.sh
  --version, --help
)";
}

ParseResult parseArgs(const std::vector<std::string>& args) {
    ParseResult result;
    Options& o = result.options;
    auto fail = [&](const std::string& message) {
        result.error = message;
        return result;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        std::string inlineValue;
        bool hasInline = false;
        std::string name = arg;
        if (arg.rfind("--", 0) == 0) {
            auto eq = arg.find('=');
            if (eq != std::string::npos) {
                name = arg.substr(0, eq);
                inlineValue = arg.substr(eq + 1);
                hasInline = true;
            }
        }
        auto value = [&](std::string& out) {
            if (hasInline) {
                out = inlineValue;
                return true;
            }
            if (i + 1 >= args.size()) {
                return false;
            }
            out = args[++i];
            return true;
        };
        auto intValue = [&](long long& out, std::string& err) {
            std::string text;
            if (!value(text)) {
                err = name + " needs a value";
                return false;
            }
            if (!parseInt(text, out)) {
                err = name + ": not an integer: " + text;
                return false;
            }
            return true;
        };
        std::string text;
        long long number = 0;
        std::string err;
        if (name == "--help" || name == "-h") {
            o.help = true;
        } else if (name == "--version") {
            o.version = true;
        } else if (name == "--debug") {
            o.debug = true;
        } else if (name == "--headless-selftest") {
            o.selftest = true;
        } else if (name == "--selftest-bench") {
            o.selftestBench = true;
        } else if (name == "--core" || name == "--ref" || name == "--state" || name == "--dev-url" ||
                   name == "--ui-dir" || name == "--title" || name == "--token" || name == "--selftest-script") {
            if (!value(text)) {
                return fail(name + " needs a value");
            }
            if (name == "--core") o.startup.coreDir = text;
            else if (name == "--ref") o.startup.coreRef = text;
            else if (name == "--state") o.startup.stateDir = text;
            else if (name == "--dev-url") o.devUrl = text;
            else if (name == "--ui-dir") o.uiDir = text;
            else if (name == "--title") o.title = text;
            else if (name == "--selftest-script") o.scriptFile = text;
            else o.token = text;
        } else if (name == "--epoch") {
            if (!intValue(number, err)) return fail(err);
            if (number < 0) return fail("--epoch must not be negative");
            o.startup.epoch = number;
        } else if (name == "--serve") {
            if (!intValue(number, err)) return fail(err);
            if (number < 0 || number > 65535) return fail("--serve: port out of range");
            o.servePort = static_cast<int>(number);
        } else if (name == "--workers") {
            if (!intValue(number, err)) return fail(err);
            if (number < 1 || number > 256) return fail("--workers must be 1..256");
            o.workers = static_cast<unsigned>(number);
        } else if (name == "--selftest-hold") {
            if (!intValue(number, err)) return fail(err);
            if (number < 0 || number > 600000) return fail("--selftest-hold: 0..600000 ms");
            o.selftestHoldMs = static_cast<int>(number);
        } else if (name == "--selftest-timeout") {
            if (!intValue(number, err)) return fail(err);
            if (number < 1 || number > 3600) return fail("--selftest-timeout: 1..3600 s");
            o.selftestTimeoutSec = static_cast<int>(number);
        } else if (name == "--size") {
            if (!value(text)) return fail("--size needs a value");
            auto x = text.find('x');
            long long w = 0;
            long long h = 0;
            if (x == std::string::npos || !parseInt(text.substr(0, x), w) || !parseInt(text.substr(x + 1), h) ||
                w < 200 || h < 150 || w > 16000 || h > 16000) {
                return fail("--size expects <width>x<height>, e.g. 1360x860");
            }
            o.width = static_cast<int>(w);
            o.height = static_cast<int>(h);
        } else {
            return fail("unknown argument: " + arg);
        }
    }
    if (!o.devUrl.empty() && !o.uiDir.empty()) {
        return fail("--dev-url and --ui-dir are mutually exclusive");
    }
    if (o.selftest && (!o.devUrl.empty() || !o.uiDir.empty())) {
        return fail("--headless-selftest uses its own page: do not combine it with --dev-url / --ui-dir");
    }
    if (o.selftest && !o.scriptFile.empty()) {
        return fail("--selftest-script drives the real UI: do not combine it with --headless-selftest");
    }
    if (!o.devUrl.empty() && o.devUrl.rfind("http://", 0) != 0 && o.devUrl.rfind("https://", 0) != 0) {
        return fail("--dev-url must start with http:// or https://");
    }
    return result;
}

} // namespace qstate::gui
