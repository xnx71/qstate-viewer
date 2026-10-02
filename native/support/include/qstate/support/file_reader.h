// Read-only random access to a (possibly > 4 GB, possibly changing) file, plus bounded-memory scan helpers.
//
// Thread-safety: every const member function may be called concurrently from any number of threads
// (positional reads, no shared file offset). refresh() may run concurrently with reads; a read that started before
// refresh() completes uses the previous handle. The scan helpers allocate their own bounded buffers (<= ~4 MiB).
//
// Truncation: reading past the end of the file is not an error; read() returns fewer bytes than requested. Real I/O
// errors (EIO, ...) throw FileError. No mapping is created, so a FileReader never blocks the node from renaming or
// replacing the file on Windows (the handle is opened with full sharing); on POSIX a replaced file keeps being read
// through the old inode until refresh() re-opens the path.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qstate::support {

class FileError : public std::runtime_error {
public:
    FileError(const std::string& message, int systemError) : std::runtime_error(message), code_(systemError) {}
    int systemError() const { return code_; }

private:
    int code_;
};

struct FileStamp {
    uint64_t size = 0;
    int64_t mtimeNs = 0;
    uint64_t inode = 0;
    uint64_t device = 0;
    bool operator==(const FileStamp&) const = default;
};

enum class RefreshResult {
    Unchanged,  // same file, same size and mtime
    Changed,    // size / mtime changed, or the path now names another file (re-opened)
    Missing     // the path does not exist (or cannot be stat'ed); the old handle stays usable
};

inline constexpr uint64_t kToEnd = std::numeric_limits<uint64_t>::max();

class FileReader {
public:
    // Throws FileError when the file cannot be opened.
    explicit FileReader(std::string path);
    // Returns nullptr and fills *error (when given) instead of throwing.
    static std::unique_ptr<FileReader> tryOpen(const std::string& path, std::string* error = nullptr);

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    ~FileReader();

    const std::string& path() const { return path_; }

    // Size / mtime as of open() or the last refresh().
    uint64_t size() const;
    int64_t mtimeNs() const;
    FileStamp stamp() const;

    // Copies up to `length` bytes starting at `offset`; returns the number of bytes delivered (fewer than requested
    // only at end of file). The live file is read, not the cached size.
    size_t read(uint64_t offset, size_t length, void* buffer) const;
    std::vector<uint8_t> readBytes(uint64_t offset, size_t length) const;  // may be shorter than `length`
    std::string readString(uint64_t offset, size_t length) const;          // may be shorter than `length`

    // Re-stats the path. If it names a different file (replaced by rename) the new file is opened.
    RefreshResult refresh();

    // True when [offset, offset + length) lies completely inside the file and every byte is zero.
    bool isAllZero(uint64_t offset, uint64_t length) const;
    // Offset of the first non-zero byte in [offset, offset + length) (clamped to the end of the file), if any.
    std::optional<uint64_t> firstNonZero(uint64_t offset, uint64_t length = kToEnd) const;

    // Calls onMatch(fileOffset) for every occurrence (overlapping ones included) of `pattern` in
    // [begin, begin + length) whose file offset is a multiple of `alignment`, in ascending order. Matches may span
    // internal block borders. Stops when onMatch returns false. Returns the number of matches reported.
    uint64_t scan(std::string_view pattern, uint64_t begin, uint64_t length, uint64_t alignment,
                  const std::function<bool(uint64_t)>& onMatch) const;
    std::vector<uint64_t> findAll(std::string_view pattern, uint64_t begin = 0, uint64_t length = kToEnd,
                                  uint64_t alignment = 1, size_t maxMatches = std::numeric_limits<size_t>::max()) const;

private:
    struct Handle;
    std::shared_ptr<const Handle> current() const;

    std::string path_;
    mutable std::mutex mutex_;
    std::shared_ptr<const Handle> handle_;
};

} // namespace qstate::support
