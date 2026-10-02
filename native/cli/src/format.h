// Internal: text rendering helpers.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace qstate::cli {

// Compact one-line text of a LeafValue / CellValue JSON.
std::string valueText(const nlohmann::json& value);
std::string hexOffset(std::uint64_t value);
std::string truncate(const std::string& text, std::size_t max);

struct TextTable {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    std::vector<bool> rightAlign; // per column, default left
    void print(std::ostream& out, const std::string& indent = "") const;
};

} // namespace qstate::cli
