#include "qstate/cli/cli.h"

#include "args.h"
#include "commands.h"
#include "session.h"

#include <ostream>

namespace qstate::cli {

std::string usage() {
    return R"(qstate-cli: inspect Qubic smart-contract state files

usage: qstate-cli <command> [options]

commands:
  verify  --core <dir> [--ref <ref|auto>] --state <dir> [--epoch N] [--define X]...
          check every state file against the layout derived from the core sources; exit code 0 only when
          every present file has the expected size
  schema  --core <dir> [--ref <ref|auto>] [--epoch N] [--contract N] [--depth N] [--json]
          list the contracts, or the memory layout (offsets, sizes, types) of one contract state
  dump    --core <dir> --state <dir> --contract N [--node <id>] [--limit n] [--offset n] [--raw] [--hide-empty]
          browse the state tree as text (the NODE column holds ids to pass to --node)
  table   --core <dir> --state <dir> --contract N --node <id> [--view v] [--sort col[:desc]] [--filter col:op[:value]]...
          [--limit n] [--offset n] [--hide-empty]        print a container as a table
  search  --core <dir> --state <dir> --contract N --query <text> [--mode auto|id|hex|int|text] [--limit n]
  digest  --core <dir> --state <dir> --contract N         KangarooTwelve digest of the state file
  serve   [--core <dir> --state <dir> ...] [--port 8787] [--ui-dir <dist>]
          HTTP + SSE transport of the full service (same as qstate-devserver); Ctrl-C to stop

common: --ref <tag|branch|sha|auto> reads the sources from git ("auto": newest tag whose EPOCH equals the state epoch),
        --epoch N picks the epoch of the state files, --define NAME[=VALUE] adds preprocessor defines,
        --help
)";
}

int run(const std::vector<std::string>& argsIn, std::ostream& out, std::ostream& err, const std::atomic<bool>* stop) {
    if (argsIn.empty()) {
        err << usage();
        return 2;
    }
    const std::string command = argsIn[0];
    if (command == "--help" || command == "-h" || command == "help") {
        out << usage();
        return 0;
    }
    const std::vector<std::string> rest(argsIn.begin() + 1, argsIn.end());
    try {
        const Args args = Args::parse(rest, kCommonValueOptions, kCommonFlags);
        if (args.flag("help")) {
            out << usage();
            return 0;
        }
        if (!args.positional().empty()) throw UsageError("unexpected argument '" + args.positional()[0] + "'");
        auto needContract = [&] {
            if (!args.integer("contract")) throw UsageError("missing required option --contract");
        };
        if (command == "verify") return cmdVerify(args, out, err);
        if (command == "schema") return cmdSchema(args, out, err);
        if (command == "dump") {
            needContract();
            return cmdDump(args, out, err);
        }
        if (command == "table") {
            needContract();
            return cmdTable(args, out, err);
        }
        if (command == "search") {
            needContract();
            return cmdSearch(args, out, err);
        }
        if (command == "digest") {
            needContract();
            return cmdDigest(args, out, err);
        }
        if (command == "serve") return cmdServe(args, out, err, stop);
        throw UsageError("unknown command '" + command + "'");
    } catch (const UsageError& e) {
        err << "qstate-cli: " << e.what() << "\n(run 'qstate-cli --help' for usage)\n";
        return 2;
    } catch (const RpcFailure& e) {
        err << "qstate-cli: " << e.code << ": " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        err << "qstate-cli: " << e.what() << "\n";
        return 2;
    }
}

} // namespace qstate::cli
