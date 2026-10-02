// Access to the real-data locations used by integration tests.
// Tests that need them must skip (return early with a MESSAGE) when the value is empty.
#pragma once

#include <cstdlib>
#include <string>

namespace qstate::testing {

inline std::string envOr(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// A git clone of the Qubic core with its tags and a checked-out working tree (QSTATE_TEST_CORE_REPO), or "". Used as
// a local repository URL (no network) and, for the front-end tests, as the sources of the current HEAD.
inline std::string coreRepo() { return envOr("QSTATE_TEST_CORE_REPO"); }

// Plain snapshot of the core sources of v1.303.2, the version of the epoch-229 state files, or "".
inline std::string coreDir229() { return envOr("QSTATE_TEST_CORE_DIR_229"); }

// Directory with the epoch-229 contractNNNN.229 files, or "".
inline std::string stateDir() { return envOr("QSTATE_TEST_STATE_DIR"); }

// Root of the qstate-viewer source tree (for test fixtures checked into the repository).
inline std::string sourceDir() { return envOr("QSTATE_SOURCE_DIR"); }

// QSTATE_TEST_NETWORK=1 enables the tests that talk to https://github.com/qubic/core.
inline bool networkTests() { return envOr("QSTATE_TEST_NETWORK") == "1"; }

} // namespace qstate::testing
