#include "qstate/support/dir_scan.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

#include "platform.h"

namespace qstate::support {

namespace fs = std::filesystem;

namespace {

bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

uint32_t toNumber(std::string_view s) {
    uint32_t v = 0;
    for (char c : s) v = v * 10 + static_cast<uint32_t>(c - '0');
    return v;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// "^name\.\d{3}$": returns the extension number.
std::optional<uint32_t> epochExtension(std::string_view s, std::string_view stem) {
    if (s.size() != stem.size() + 4 || !startsWith(s, stem) || s[stem.size()] != '.') return std::nullopt;
    const auto digits = s.substr(stem.size() + 1);
    if (digits.size() != 3 || !allDigits(digits)) return std::nullopt;
    return toNumber(digits);
}

bool isPageDirName(std::string_view s) {
    // core-lite swap / virtual memory directories: four lower case letters, a dot, three digits (logs.233, pmap.233)
    return s.size() == 8 && s[4] == '.' && allDigits(s.substr(5)) &&
           std::all_of(s.begin(), s.begin() + 4, [](char c) { return c >= 'a' && c <= 'z'; });
}

int64_t toMs(int64_t ns) { return ns / 1000000; }

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// "ep229", "ep229.tmp", "ep229.old"
bool parseSnapshotDirName(std::string_view name, SnapshotDir& out) {
    if (!startsWith(name, "ep")) return false;
    std::string_view rest = name.substr(2);
    bool tmp = false, old = false;
    if (endsWith(rest, ".tmp")) {
        tmp = true;
        rest.remove_suffix(4);
    } else if (endsWith(rest, ".old")) {
        old = true;
        rest.remove_suffix(4);
    }
    if (!allDigits(rest) || rest.size() > 9) return false;
    out.epoch = toNumber(rest);
    out.isTmp = tmp;
    out.isOld = old;
    return true;
}

}  // namespace

ParsedFileName parseStateFileName(std::string_view name) {
    ParsedFileName p;
    // contractNNNN.EEE
    if (name.size() == 16 && startsWith(name, "contract") && name[12] == '.' && allDigits(name.substr(8, 4)) &&
        allDigits(name.substr(13))) {
        p.kind = FileNameKind::ContractState;
        p.contractIndex = toNumber(name.substr(8, 4));
        p.ext = toNumber(name.substr(13));
        return p;
    }
    if (auto e = epochExtension(name, "spectrum")) {
        p.kind = FileNameKind::Spectrum;
        p.ext = e;
        return p;
    }
    if (auto e = epochExtension(name, "universe")) {
        p.kind = FileNameKind::Universe;
        p.ext = e;
        return p;
    }
    for (const char* stem : {"contract_exec_fees_rec", "contract_exec_fees_acc"}) {
        if (auto e = epochExtension(name, stem)) {
            p.kind = FileNameKind::ContractExecFees;
            p.ext = e;
            return p;
        }
    }
    if (name == "system" || name == "system.eoe" || name == "system.snp") {
        p.kind = FileNameKind::System;
        return p;
    }
    const std::string l = lower(name);
    if (endsWith(l, ".zip")) {
        p.kind = FileNameKind::Archive;
        return p;
    }
    if (name == "bpp9000.task" || name == "debug.log" || name == "profiling.csv" || name == "crash.dump" ||
        name == "startup.nsh" || name == ".qubic-tmp" || name == "s" || name == "efi" || endsWith(l, ".efi") ||
        isPageDirName(name)) {
        p.kind = FileNameKind::Ignored;
        return p;
    }
    return p;
}

const EpochFileSet* StateDirScan::find(uint32_t epoch) const {
    for (const auto& e : epochs) {
        if (e.epoch == epoch) return &e;
    }
    return nullptr;
}

std::vector<uint32_t> StateDirScan::epochNumbers() const {
    std::vector<uint32_t> r;
    for (const auto& e : epochs) r.push_back(e.epoch);
    return r;
}

std::optional<uint32_t> StateDirScan::newestEpoch() const {
    if (epochs.empty()) return std::nullopt;
    return epochs.back().epoch;  // ascending
}

std::vector<OtherFileEntry> StateDirScan::othersOfEpoch(uint32_t epoch) const {
    std::vector<OtherFileEntry> r;
    for (const auto& o : others) {
        if (!o.ext || *o.ext == epoch) r.push_back(o);
    }
    return r;
}

StateDirScan scanStateDir(const std::string& dirArg) {
    StateDirScan scan;
    scan.dir = normalizePath(dirArg);
    std::error_code ec;
    fs::directory_iterator it(fs::path(scan.dir), ec);
    if (ec) {
        scan.error = "cannot list '" + scan.dir + "': " + ec.message();
        return scan;
    }
    scan.readable = true;

    std::map<uint32_t, EpochFileSet> sets;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;  // iteration failed half way: keep what we have
        const std::string name = it->path().filename().string();
        const auto parsed = parseStateFileName(name);
        const std::string path = (fs::path(scan.dir) / name).string();

        std::error_code se;
        const bool isDir = it->is_directory(se);
        if (isDir) {
            SnapshotDir snap;
            if (parseSnapshotDirName(name, snap)) {
                snap.name = name;
                snap.path = path;
                scan.snapshots.push_back(std::move(snap));
            }
            continue;
        }
        if (parsed.kind == FileNameKind::Ignored) continue;
        const auto st = platform::statPath(path);
        if (!st.exists) continue;  // vanished between listing and stat
        if (!st.isRegular) continue;

        if (parsed.kind == FileNameKind::ContractState) {
            StateFileEntry f;
            f.index = parsed.contractIndex;
            f.ext = *parsed.ext;
            f.name = name;
            f.path = path;
            f.size = st.size;
            f.mtimeMs = toMs(st.mtimeNs);
            auto& set = sets[f.ext];
            set.epoch = f.ext;
            set.contracts.push_back(std::move(f));
            continue;
        }
        OtherFileEntry o;
        o.name = name;
        o.path = path;
        o.size = st.size;
        o.mtimeMs = toMs(st.mtimeNs);
        o.ext = parsed.ext;
        switch (parsed.kind) {
            case FileNameKind::Spectrum: o.kind = OtherFileKind::Spectrum; break;
            case FileNameKind::Universe: o.kind = OtherFileKind::Universe; break;
            case FileNameKind::ContractExecFees: o.kind = OtherFileKind::ContractExecFees; break;
            case FileNameKind::Archive: o.kind = OtherFileKind::Archive; break;
            default: o.kind = OtherFileKind::Unknown; break;
        }
        scan.others.push_back(std::move(o));
    }

    for (auto& [epoch, set] : sets) {
        std::sort(set.contracts.begin(), set.contracts.end(),
                  [](const StateFileEntry& a, const StateFileEntry& b) { return a.index < b.index; });
        // Two files with the same index cannot occur (same name); gaps are everything missing below the maximum.
        uint32_t expected = 0;
        for (const auto& f : set.contracts) {
            while (expected < f.index) set.missingIndices.push_back(expected++);
            expected = f.index + 1;
        }
        set.contiguous = set.missingIndices.empty();
        scan.epochs.push_back(std::move(set));
    }
    std::sort(scan.others.begin(), scan.others.end(),
              [](const OtherFileEntry& a, const OtherFileEntry& b) { return lessIgnoreCase(a.name, b.name); });
    std::sort(scan.snapshots.begin(), scan.snapshots.end(), [](const SnapshotDir& a, const SnapshotDir& b) {
        return a.epoch != b.epoch ? a.epoch < b.epoch : a.name < b.name;
    });
    return scan;
}

// ----------------------------------------------------------------------------------------------------------------

bool lessIgnoreCase(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca < cb;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;  // stable order for names that differ only by case
}

std::string homeDir() { return platform::homeDirectory(); }
std::string currentDir() { return platform::currentDirectory(); }
std::string platformName() { return platform::platformName(); }

char pathSeparator() {
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

bool pathExists(const std::string& path) { return platform::statPath(path).exists; }
bool isDirectory(const std::string& path) { return platform::statPath(path).isDir; }

std::string normalizePath(const std::string& input) {
    if (input.empty()) return {};
    std::string path = input;
    if (path == "~" || startsWith(path, "~/") || startsWith(path, "~\\")) {
        const std::string home = homeDir();
        if (!home.empty()) path = home + path.substr(1);
    }
    fs::path p(path);
    if (!p.is_absolute()) {
        const std::string cwd = currentDir();
        if (!cwd.empty()) p = fs::path(cwd) / p;
    }
    p = p.lexically_normal();
    std::string s = p.string();
    // lexically_normal keeps a trailing separator ("/a/b/" -> "/a/b/"); strip it except for roots.
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\') && fs::path(s).root_path() != fs::path(s)) s.pop_back();
    return s;
}

std::optional<std::string> parentPath(const std::string& normalized) {
    const fs::path p(normalized);
    if (normalized.empty() || p == p.root_path()) return std::nullopt;
    const fs::path parent = p.parent_path();
    if (parent.empty() || parent == p) return std::nullopt;
    return parent.string();
}

PathHints pathHints(const std::string& dirArg) {
    PathHints hints;
    const std::string dir = normalizePath(dirArg);
    const fs::path d(dir);
    hints.isCoreRepo = platform::statPath((d / "src" / "contract_core" / "contract_def.h").string()).isRegular;
    hints.isGitRepo = platform::statPath((d / ".git").string()).exists;
    hints.stateEpochs = scanStateDir(dir).epochNumbers();
    return hints;
}

FsListResult listDirectory(const std::string& pathArg, bool showHidden) {
    FsListResult result;
    FsListing& listing = result.listing;
    listing.path = normalizePath(pathArg);
    listing.parent = parentPath(listing.path);
    if (listing.path.empty()) {
        result.error = "empty path";
        return result;
    }
    const auto st = platform::statPath(listing.path);
    if (!st.exists) {
        result.error = st.error ? "cannot access '" + listing.path + "': " + platform::errorMessage(st.error)
                                : "'" + listing.path + "' does not exist";
        return result;
    }
    if (!st.isDir) {
        result.error = "'" + listing.path + "' is not a directory";
        return result;
    }
    std::error_code ec;
    fs::directory_iterator it(fs::path(listing.path), ec);
    if (ec) {
        result.error = "cannot list '" + listing.path + "': " + ec.message();
        return result;
    }

    bool hasGit = false;
    std::map<uint32_t, bool> epochs;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (name == ".git") hasGit = true;
        if (const auto parsed = parseStateFileName(name); parsed.kind == FileNameKind::ContractState) {
            epochs[*parsed.ext] = true;
        }
        if (!showHidden && !name.empty() && name[0] == '.') continue;
        FsEntry e;
        e.name = name;
        e.path = (fs::path(listing.path) / name).string();
        const auto es = platform::statPath(e.path);  // follows symlinks; broken links stay listed as files
        e.isDir = es.exists && es.isDir;
        if (es.exists && !es.isDir) {
            e.size = es.size;
            e.mtimeMs = toMs(es.mtimeNs);
        } else if (es.exists) {
            e.mtimeMs = toMs(es.mtimeNs);
        }
        listing.entries.push_back(std::move(e));
    }
    std::sort(listing.entries.begin(), listing.entries.end(), [](const FsEntry& a, const FsEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return lessIgnoreCase(a.name, b.name);
    });
    listing.hints.isGitRepo = hasGit;
    listing.hints.isCoreRepo =
        platform::statPath((fs::path(listing.path) / "src" / "contract_core" / "contract_def.h").string()).isRegular;
    for (const auto& [epoch, present] : epochs) {
        (void)present;
        listing.hints.stateEpochs.push_back(epoch);
    }
    result.ok = true;
    return result;
}

}  // namespace qstate::support
