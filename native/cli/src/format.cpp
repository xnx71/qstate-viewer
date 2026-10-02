#include "format.h"

#include <algorithm>
#include <cstdio>
#include <ostream>

namespace qstate::cli {

using nlohmann::json;

std::string truncate(const std::string& text, std::size_t max) {
    if (text.size() <= max) return text;
    if (max <= 3) return text.substr(0, max);
    return text.substr(0, max - 3) + "...";
}

std::string hexOffset(std::uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

std::string valueText(const json& v) {
    if (!v.is_object() || !v.contains("k")) return v.is_null() ? "" : v.dump();
    const std::string k = v["k"];
    if (k == "int") {
        std::string s = v["v"];
        if (v.contains("text")) s += " (\"" + v["text"].get<std::string>() + "\")";
        return s;
    }
    if (k == "bool") return v["v"].get<bool>() ? "true" : "false";
    if (k == "char") return "'" + v["v"].get<std::string>() + "' (" + std::to_string(v["code"].get<int>()) + ")";
    if (k == "enum") return v.contains("name") ? v["name"].get<std::string>() + " (" + v["v"].get<std::string>() + ")" : v["v"].get<std::string>();
    if (k == "id") {
        std::string s = v["zero"].get<bool>() ? "0" : v["identity"].get<std::string>();
        if (v.contains("contract")) s += " [contract " + std::to_string(v["contract"]["index"].get<int>()) + " " + v["contract"]["name"].get<std::string>() + "]";
        if (v.contains("text")) s += " (\"" + v["text"].get<std::string>() + "\")";
        return s;
    }
    if (k == "u128" || k == "float") return v["v"].get<std::string>();
    if (k == "datetime") return v["text"].get<std::string>() + (v["valid"].get<bool>() ? "" : " (invalid)");
    if (k == "bits") return std::to_string(v["set"].get<std::uint64_t>()) + "/" + std::to_string(v["count"].get<std::uint64_t>()) + " bits set";
    if (k == "bytes") {
        std::string s = std::to_string(v["length"].get<std::uint64_t>()) + " bytes " + truncate(v["hex"].get<std::string>(), 40);
        if (v.contains("text")) s += " (\"" + v["text"].get<std::string>() + "\")";
        return s;
    }
    if (k == "ptr") return "ptr " + v["hex"].get<std::string>();
    if (k == "unavailable") return "<" + v["reason"].get<std::string>() + ">";
    if (k == "composite") return v["preview"].get<std::string>();
    return v.dump();
}

void TextTable::print(std::ostream& out, const std::string& indent) const {
    const std::size_t cols = header.size();
    std::vector<std::size_t> width(cols, 0);
    auto measure = [&](const std::vector<std::string>& row) {
        for (std::size_t c = 0; c < cols && c < row.size(); ++c) width[c] = std::max(width[c], row[c].size());
    };
    measure(header);
    for (const auto& r : rows) measure(r);
    auto line = [&](const std::vector<std::string>& row) {
        out << indent;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::string cell = c < row.size() ? row[c] : std::string();
            const bool right = c < rightAlign.size() && rightAlign[c];
            const std::size_t pad = width[c] - cell.size();
            if (right) out << std::string(pad, ' ') << cell;
            else out << cell << (c + 1 < cols ? std::string(pad, ' ') : std::string());
            if (c + 1 < cols) out << "  ";
        }
        out << "\n";
    };
    line(header);
    for (const auto& r : rows) line(r);
}

} // namespace qstate::cli
