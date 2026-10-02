// qstate-cli: command line front end. All commands go through the same RPC methods as the UI (qstate::service);
// `run` is a plain function so the commands can be unit tested.
#pragma once

#include <atomic>
#include <iosfwd>
#include <string>
#include <vector>

namespace qstate::cli {

// `args` excludes argv[0]. Returns the process exit code: 0 = success, 1 = the check failed (verify: some file is not
// ok; schema: a contract layout could not be computed), 2 = usage or runtime error.
// `serve` blocks until `*stop` becomes true (when `stop` is null it never returns).
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, const std::atomic<bool>* stop = nullptr);

std::string usage();

} // namespace qstate::cli
