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

// Qubic core checkout (repository root), or "" when not configured.
inline std::string coreDir() { return envOr("QSTATE_TEST_CORE_DIR"); }

// Directory with contractNNNN.EEE files matching coreDir(), or "" when not configured.
inline std::string stateDir() { return envOr("QSTATE_TEST_STATE_DIR"); }

// Root of the qstate-viewer source tree (for test fixtures checked into the repository).
inline std::string sourceDir() { return envOr("QSTATE_SOURCE_DIR"); }

} // namespace qstate::testing
