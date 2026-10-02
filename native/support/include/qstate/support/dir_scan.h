// Directory scanning for the viewer: recognises Qubic state files (report 06 sections 2 and 12), produces the data
// behind `fs.list` (FsListing / PathHints of contract.ts) and offers small path helpers.
//
// Thread-safety: all functions are reentrant and keep no state. Permission errors and vanishing files never throw:
// unreadable entries are skipped, an unreadable directory is reported through `error` / `readable`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qstate::support {

// ----------------------------------------------------------------------------------------------------------------
// File name classification
// ----------------------------------------------------------------------------------------------------------------

enum class FileNameKind {
    ContractState,     // contractNNNN.EEE
    Spectrum,          // spectrum.EEE
    Universe,          // universe.EEE
    ContractExecFees,  // contract_exec_fees_rec.EEE / contract_exec_fees_acc.EEE
    System,            // system, system.eoe, system.snp
    Archive,           // *.zip
    Ignored,           // node housekeeping files (bpp9000.task, debug.log, *.efi, page directories, ...)
    Unknown
};

struct ParsedFileName {
    FileNameKind kind = FileNameKind::Unknown;
    uint32_t contractIndex = 0;      // ContractState only
    std::optional<uint32_t> ext;     // the EEE extension as a number ("000" -> 0), when the name has one
};

// Classifies a bare file or directory name (no path).
ParsedFileName parseStateFileName(std::string_view name);

// ----------------------------------------------------------------------------------------------------------------
// State directory scan
// ----------------------------------------------------------------------------------------------------------------

struct StateFileEntry {
    uint32_t index = 0;
    uint32_t ext = 0;
    std::string name;
    std::string path;  // absolute
    uint64_t size = 0;
    int64_t mtimeMs = 0;
};

enum class OtherFileKind { Spectrum, Universe, ContractExecFees, Archive, Unknown };

struct OtherFileEntry {
    std::string name;
    std::string path;
    uint64_t size = 0;
    int64_t mtimeMs = 0;
    OtherFileKind kind = OtherFileKind::Unknown;
    std::optional<uint32_t> ext;
};

// contractNNNN.EEE files sharing the extension EEE.
struct EpochFileSet {
    uint32_t epoch = 0;                       // EEE (epoch % 1000; 0 = saved on demand)
    std::vector<StateFileEntry> contracts;    // ascending by index
    // True when the indices are exactly 0..N-1; otherwise `missingIndices` lists the gaps below the maximum index.
    bool contiguous = true;
    std::vector<uint32_t> missingIndices;
};

// A snapshot sub directory ep<E>, ep<E>.tmp or ep<E>.old.
struct SnapshotDir {
    std::string name;
    std::string path;
    uint32_t epoch = 0;
    bool isTmp = false;
    bool isOld = false;
};

struct StateDirScan {
    std::string dir;                          // normalized absolute path
    bool readable = false;                    // false: directory missing or not listable, see `error`
    std::string error;
    std::vector<EpochFileSet> epochs;         // ascending by epoch
    std::vector<OtherFileEntry> others;       // sibling files, archives and unknown files (ignored files omitted)
    std::vector<SnapshotDir> snapshots;       // ascending by epoch

    const EpochFileSet* find(uint32_t epoch) const;
    std::vector<uint32_t> epochNumbers() const;              // ascending
    std::optional<uint32_t> newestEpoch() const;             // highest epoch; epoch 0 only when nothing else exists
    // Other files that belong to `epoch` (same extension) or have no epoch extension.
    std::vector<OtherFileEntry> othersOfEpoch(uint32_t epoch) const;
};

// Scans one directory level (plus the names of ep<E> sub directories).
StateDirScan scanStateDir(const std::string& dir);

// ----------------------------------------------------------------------------------------------------------------
// fs.list
// ----------------------------------------------------------------------------------------------------------------

struct FsEntry {
    std::string name;
    std::string path;  // absolute
    bool isDir = false;
    std::optional<uint64_t> size;
    std::optional<int64_t> mtimeMs;
    // Files named contractNNNN.EEE: contract index and epoch extension.
    std::optional<std::pair<uint32_t, uint32_t>> state;
};

struct PathHints {
    std::vector<uint32_t> stateEpochs;  // epochs of contractNNNN.EEE files directly inside, ascending
};

struct FsListing {
    std::string path;                    // normalized absolute path of the listed directory
    std::optional<std::string> parent;   // nullopt at a file system root
    std::vector<FsEntry> entries;        // directories first, then files; each group by name, case-insensitive
    PathHints hints;
};

struct FsListResult {
    bool ok = false;
    std::string error;  // when !ok: why the directory could not be listed
    FsListing listing;  // path / parent are filled even on failure
};

FsListResult listDirectory(const std::string& path, bool showHidden = false);

// ----------------------------------------------------------------------------------------------------------------
// Paths
// ----------------------------------------------------------------------------------------------------------------

// Expands a leading "~", makes the path absolute (relative to the current directory) and normalizes "." / ".."
// lexically; no symlink resolution; no trailing separator except for roots. Empty input -> empty output.
std::string normalizePath(const std::string& path);
std::optional<std::string> parentPath(const std::string& normalizedPath);
std::string homeDir();
// Per-user cache directory of the application: <cache base>/qstate-viewer (Linux: $XDG_CACHE_HOME or ~/.cache;
// Windows: %LOCALAPPDATA%; macOS: ~/Library/Caches), a directory below the temp directory when nothing else is known.
std::string defaultCacheDir();
std::string currentDir();
std::string platformName();   // "linux" | "windows" | "macos"
char pathSeparator();
bool pathExists(const std::string& path);
bool isDirectory(const std::string& path);
// Case-insensitive (ASCII) "a < b" used for listings.
bool lessIgnoreCase(const std::string& a, const std::string& b);

} // namespace qstate::support
