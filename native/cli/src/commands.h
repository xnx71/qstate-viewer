// Internal: the commands of qstate-cli. Each returns the exit code (see cli.h).
#pragma once

#include "args.h"

#include <atomic>
#include <iosfwd>

namespace qstate::cli {

int cmdVerify(const Args& args, std::ostream& out, std::ostream& err);
int cmdSchema(const Args& args, std::ostream& out, std::ostream& err);
int cmdDump(const Args& args, std::ostream& out, std::ostream& err);
int cmdTable(const Args& args, std::ostream& out, std::ostream& err);
int cmdSearch(const Args& args, std::ostream& out, std::ostream& err);
int cmdDigest(const Args& args, std::ostream& out, std::ostream& err);
int cmdServe(const Args& args, std::ostream& out, std::ostream& err, const std::atomic<bool>* stop);

} // namespace qstate::cli
