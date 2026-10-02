// Internal platform layer of the support module (not installed, not part of the public API).
// platform_posix.cpp is verified on Linux; platform_win32.cpp is written against the Win32 API but untested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qstate::support::platform {

struct FileStat {
    bool exists = false;
    bool isDir = false;
    bool isRegular = false;
    uint64_t size = 0;
    int64_t mtimeNs = 0;  // nanoseconds since the Unix epoch
    uint64_t inode = 0;   // 0 where unavailable
    uint64_t device = 0;
    int error = 0;        // errno / GetLastError() when !exists (0 = plain "not found" or success)
};

// stat() of a path (follows symlinks unless asked otherwise). Never throws.
FileStat statPath(const std::string& path, bool followSymlinks = true);

std::string errorMessage(int error);

// Read-only random access handle. Thread-safe for concurrent readAt().
class NativeFile {
public:
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    ~NativeFile();

    // Returns nullptr and sets `error` (system error code) on failure.
    static std::unique_ptr<NativeFile> open(const std::string& path, int* error);

    struct ReadResult {
        size_t bytes = 0;  // bytes delivered; fewer than requested only at end of file or on error
        int error = 0;
    };
    ReadResult readAt(uint64_t offset, void* buffer, size_t length) const;
    FileStat stat() const;

private:
    intptr_t handle_ = -1;
};

// "~" of the current user ("" when unknown).
std::string homeDirectory();
// Per-user configuration base directory (XDG_CONFIG_HOME / ~/.config, %APPDATA%, ~/Library/Application Support).
std::string configBaseDirectory();
std::string currentDirectory();
std::string platformName();  // "linux" | "windows" | "macos"
unsigned long processId();

// Writes `data` to `path` atomically: temp file in the same directory, flush to disk, rename over the target.
bool writeFileAtomic(const std::string& path, const std::string& data, std::string* error);

struct ProcessSpec {
    std::vector<std::string> argv;  // argv[0] is looked up in PATH; no shell is involved
    std::vector<std::pair<std::string, std::string>> extraEnv;
    int timeoutMs = 30000;          // <= 0: no timeout
    // Receives stdout in chunks. Returning false aborts the process (it is stopped, `aborted` is set).
    std::function<bool(const char* data, size_t size)> onStdout;
    // Receives stderr in chunks as they arrive (git progress lines), in addition to ProcessResult::stderrText.
    std::function<void(const char* data, size_t size)> onStderr;
    // Polled about every 100 ms while the process runs; returning true stops it (`cancelled` is set).
    std::function<bool()> cancel;
    // ProcessResult::stderrText keeps the LAST maxStderrBytes of stderr.
    size_t maxStderrBytes = 64 * 1024;
};

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    bool aborted = false;
    bool cancelled = false;
    int exitCode = -1;         // valid when started and not killed
    std::string stderrText;
    std::string error;         // spawn / io failure description
};

// Runs the command to completion. A process that has to be stopped (timeout, abort, cancel) gets SIGTERM together with
// everything it spawned (own process group) and SIGKILL when it does not end within two seconds; the child is always
// reaped before this returns.
ProcessResult runProcess(const ProcessSpec& spec);

}  // namespace qstate::support::platform
