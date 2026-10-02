#include "qstate/cli/cli.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::atomic<bool> gStop{false};
extern "C" void onSignal(int) { gStop.store(true); }
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return qstate::cli::run(std::vector<std::string>(argv + 1, argv + argc), std::cout, std::cerr, &gStop);
}
