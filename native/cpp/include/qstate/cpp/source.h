// Where source text comes from. All paths are relative to the source root, use '/' separators and are
// lexically normalized (no "." or ".." segments, no leading slash).
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace qstate::cpp {

class SourceProvider {
public:
    virtual ~SourceProvider() = default;
    // File content, or nullopt when the file does not exist. Must be thread-safe for concurrent reads.
    virtual std::optional<std::string> read(std::string_view path) const = 0;
    virtual bool exists(std::string_view path) const { return read(path).has_value(); }
};

// Files below a directory on disk.
class DiskSource final : public SourceProvider {
public:
    explicit DiskSource(std::filesystem::path root) : root_(std::move(root)) {}
    std::optional<std::string> read(std::string_view path) const override;
    bool exists(std::string_view path) const override;
    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
};

// In-memory files (tests).
class MapSource final : public SourceProvider {
public:
    MapSource() = default;
    explicit MapSource(std::map<std::string, std::string> files) : files_(std::move(files)) {}
    void add(std::string path, std::string content) { files_[std::move(path)] = std::move(content); }
    std::optional<std::string> read(std::string_view path) const override;
    bool exists(std::string_view path) const override;

private:
    std::map<std::string, std::string> files_;
};

// Lexically normalizes "a/b/../c/./d" -> "a/c/d". Returns "" when the path escapes the root ("..") .
std::string normalizePath(std::string_view path);

// Directory part of a normalized path ("a/b/c.h" -> "a/b", "c.h" -> "").
std::string dirName(std::string_view path);

// Joins two path parts and normalizes.
std::string joinPath(std::string_view dir, std::string_view rel);

} // namespace qstate::cpp
