#include "qstate/cpp/source.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace qstate::cpp {

std::string normalizePath(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t pos = 0;
    // A leading separator means an absolute path, which is outside the source root.
    if (!path.empty() && (path[0] == '/' || path[0] == '\\')) return {};
    while (pos <= path.size()) {
        std::size_t end = pos;
        while (end < path.size() && path[end] != '/' && path[end] != '\\') ++end;
        std::string_view part = path.substr(pos, end - pos);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
        } else {
            parts.push_back(part);
        }
        pos = end + 1;
    }
    std::string out;
    for (std::string_view part : parts) {
        if (!out.empty()) out.push_back('/');
        out.append(part);
    }
    return out;
}

std::string dirName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string joinPath(std::string_view dir, std::string_view rel) {
    if (dir.empty()) return normalizePath(rel);
    std::string joined(dir);
    joined.push_back('/');
    joined.append(rel);
    return normalizePath(joined);
}

namespace {

// Resolves `path` below `root`; empty when the path is not a valid relative path.
std::filesystem::path resolve(const std::filesystem::path& root, std::string_view path) {
    const std::string norm = normalizePath(path);
    if (norm.empty()) return {};
    return root / std::filesystem::path(norm);
}

} // namespace

std::optional<std::string> DiskSource::read(std::string_view path) const {
    const std::filesystem::path full = resolve(root_, path);
    if (full.empty()) return std::nullopt;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(full, ec) || ec) return std::nullopt;
    std::ifstream in(full, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string data;
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        in.read(data.data(), size);
        data.resize(static_cast<std::size_t>(in.gcount()));
    } else {
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return data;
}

bool DiskSource::exists(std::string_view path) const {
    const std::filesystem::path full = resolve(root_, path);
    if (full.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(full, ec) && !ec;
}

std::optional<std::string> MapSource::read(std::string_view path) const {
    const std::string norm = normalizePath(path);
    auto it = files_.find(norm);
    if (it == files_.end()) {
        // Tolerate keys that were added unnormalized.
        it = files_.find(std::string(path));
        if (it == files_.end()) return std::nullopt;
    }
    return it->second;
}

bool MapSource::exists(std::string_view path) const {
    return files_.count(normalizePath(path)) != 0 || files_.count(std::string(path)) != 0;
}

} // namespace qstate::cpp
