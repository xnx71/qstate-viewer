// Diagnostics shared by all front-end stages.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qstate::cpp {

struct Diag {
    enum class Severity : std::uint8_t { Note, Warning, Error };
    Severity severity = Severity::Warning;
    std::string message;
    // Path relative to the source root ('/' separators); empty when not applicable.
    std::string file;
    std::uint32_t line = 0;
};

using Diags = std::vector<Diag>;

} // namespace qstate::cpp
