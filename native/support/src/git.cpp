#include "qstate/support/git.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

#include "platform.h"
#include "qstate/support/identity.h"
#include "qstate/support/k12.h"
#include "qstate/support/tar.h"

namespace qstate::support {

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersionHeader = "src/public_settings.h";
constexpr const char* kCompleteMarker = ".qstate-export-complete";

bool safeRef(const std::string& ref) {
    if (ref.empty() || ref[0] == '-') return false;
    return std::none_of(ref.begin(), ref.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

bool safeSubpath(const std::string& p) {
    if (p.empty() || p[0] == '-' || p[0] == '/' || p[0] == ':') return false;
    if (p.find("..") != std::string::npos) return false;
    return std::none_of(p.begin(), p.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F || c == '\\'; });
}

bool isHexSha(const std::string& s) {
    return s.size() >= 40 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(i);
}

std::optional<int> defineValue(const std::string& text, const char* name) {
    const std::regex re(std::string(R"(^[ \t]*#[ \t]*define[ \t]+)") + name + R"([ \t]+(\d+))", std::regex::multiline);
    std::smatch m;
    if (std::regex_search(text, m, re)) {
        try {
            return std::stoi(m[1].str());
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::string readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

GitVersionInfo parsePublicSettings(const std::string& text) {
    GitVersionInfo info;
    info.epoch = defineValue(text, "EPOCH");
    const auto a = defineValue(text, "VERSION_A");
    const auto b = defineValue(text, "VERSION_B");
    const auto c = defineValue(text, "VERSION_C");
    if (a && b && c) info.version = std::to_string(*a) + "." + std::to_string(*b) + "." + std::to_string(*c);
    return info;
}

GitRepo::GitRepo(std::string dir, GitOptions options) : dir_(std::move(dir)), options_(std::move(options)) {}

GitRepo::RunResult GitRepo::run(const std::vector<std::string>& args, size_t maxOutput) const {
    platform::ProcessSpec spec;
    spec.argv.push_back(options_.executable);
    spec.argv.insert(spec.argv.end(), {"-C", dir_});
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.extraEnv = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_OPTIONAL_LOCKS", "0"}, {"LC_ALL", "C"}, {"GIT_PAGER", "cat"}};
    spec.timeoutMs = options_.timeoutMs;
    RunResult r;
    spec.onStdout = [&](const char* d, size_t n) {
        if (r.out.size() + n > maxOutput) return false;
        r.out.append(d, n);
        return true;
    };
    const auto res = platform::runProcess(spec);
    r.err = res.stderrText.empty() ? res.error : res.stderrText;
    r.ok = res.started && !res.timedOut && !res.aborted && res.exitCode == 0;
    return r;
}

bool GitRepo::isAvailable(const GitOptions& options) {
    platform::ProcessSpec spec;
    spec.argv = {options.executable, "--version"};
    spec.timeoutMs = std::min(options.timeoutMs, 10000);
    std::string out;
    spec.onStdout = [&](const char* d, size_t n) {
        out.append(d, n);
        return out.size() < 4096;
    };
    const auto res = platform::runProcess(spec);
    return res.started && res.exitCode == 0 && out.rfind("git version", 0) == 0;
}

bool GitRepo::isRepo(const std::string& dir, const GitOptions& options) {
    if (dir.empty() || !fs::is_directory(dir)) return false;
    GitRepo repo(dir, options);
    const auto inside = repo.run({"rev-parse", "--is-inside-work-tree", "--show-prefix"}, 4096);
    if (!inside.ok) return false;
    // Output: "true\n<prefix>\n"; the prefix is empty at the top level of the work tree.
    return inside.out == "true\n\n";
}

std::optional<std::string> GitRepo::resolveCommit(const std::string& ref) const {
    if (!safeRef(ref)) return std::nullopt;
    const auto r = run({"rev-parse", "--verify", "--quiet", ref + "^{commit}"}, 4096);
    if (!r.ok) return std::nullopt;
    const std::string sha = trim(r.out);
    if (!isHexSha(sha)) return std::nullopt;
    return sha;
}

std::vector<GitTag> GitRepo::listTags(size_t limit) const {
    std::vector<std::string> args = {"for-each-ref", "--sort=-creatordate"};
    if (limit > 0) args.push_back("--count=" + std::to_string(limit));
    args.push_back("--format=%(refname:short)%09%(objectname)%09%(*objectname)%09%(creatordate:iso-strict)");
    args.push_back("refs/tags");
    const auto r = run(args);
    std::vector<GitTag> tags;
    if (!r.ok) return tags;
    size_t pos = 0;
    while (pos < r.out.size()) {
        size_t eol = r.out.find('\n', pos);
        if (eol == std::string::npos) eol = r.out.size();
        const std::string line = r.out.substr(pos, eol - pos);
        pos = eol + 1;
        std::vector<std::string> f;
        size_t s = 0;
        while (true) {
            const size_t t = line.find('\t', s);
            f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        if (f.size() < 4 || f[0].empty()) continue;
        GitTag tag;
        tag.name = f[0];
        tag.commitSha = f[2].empty() ? f[1] : f[2];  // annotated tags: the peeled commit
        tag.date = f[3];
        tags.push_back(std::move(tag));
    }
    return tags;
}

std::optional<std::string> GitRepo::showFile(const std::string& ref, const std::string& path) const {
    if (!safeRef(ref) || !safeSubpath(path)) return std::nullopt;
    const auto r = run({"cat-file", "blob", ref + ":" + path});
    if (!r.ok) return std::nullopt;
    return r.out;
}

std::optional<GitVersionInfo> GitRepo::readVersionInfo(const std::string& ref) const {
    GitVersionInfo info;
    if (ref.empty()) {
        const fs::path header = fs::path(dir_) / kVersionHeader;
        std::error_code ec;
        if (!fs::is_regular_file(header, ec)) return std::nullopt;
        info = parsePublicSettings(readTextFile(header));
        info.ref.clear();
        if (isAvailable(options_) && isRepo()) {
            if (const auto sha = resolveCommit("HEAD")) {
                info.sha = *sha;
                const auto d = run({"log", "-1", "--format=%cI", *sha});
                if (d.ok) info.date = trim(d.out);
            }
        }
        return info;
    }
    const auto sha = resolveCommit(ref);
    if (!sha) return std::nullopt;
    const auto text = showFile(*sha, kVersionHeader);
    if (!text) return std::nullopt;
    info = parsePublicSettings(*text);
    info.ref = ref;
    info.sha = *sha;
    const auto d = run({"log", "-1", "--format=%cI", *sha});
    if (d.ok) info.date = trim(d.out);
    return info;
}

std::optional<std::string> GitRepo::autoPickRef(int epoch) const {
    for (const auto& tag : listTags()) {
        const auto text = showFile(tag.commitSha, kVersionHeader);
        if (!text) continue;
        if (parsePublicSettings(*text).epoch == epoch) return tag.name;
    }
    return std::nullopt;
}

GitExportResult GitRepo::exportTree(const std::string& ref, const std::string& cacheRoot,
                                    const std::vector<std::string>& subpathsIn) const {
    static std::atomic<unsigned> counter{0};
    const auto sha = resolveCommit(ref);
    if (!sha) throw std::runtime_error("git: cannot resolve ref '" + ref + "' in '" + dir_ + "'");
    for (const auto& s : subpathsIn) {
        if (!safeSubpath(s)) throw std::runtime_error("git: unsafe export path '" + s + "'");
    }

    // Cache key: commit + the requested subpaths (default set has no suffix).
    std::string name = *sha;
    if (!subpathsIn.empty()) {
        std::vector<std::string> sorted = subpathsIn;
        std::sort(sorted.begin(), sorted.end());
        std::string joined;
        for (const auto& s : sorted) joined += s + '\n';
        const auto d = k12Digest(joined.data(), joined.size());
        name += "-" + toHex(d.data(), 4);
    }
    const fs::path finalDir = fs::path(cacheRoot) / name;
    std::error_code ec;
    if (fs::exists(finalDir / kCompleteMarker, ec)) return {finalDir.string(), *sha, true};

    fs::create_directories(cacheRoot, ec);
    if (ec) throw std::runtime_error("git: cannot create cache directory '" + cacheRoot + "': " + ec.message());

    // Which paths to archive.
    std::vector<std::string> paths;
    const auto top = run({"ls-tree", "-z", *sha});
    if (!top.ok) throw std::runtime_error("git: ls-tree failed: " + top.err);
    std::vector<std::pair<std::string, std::string>> rootEntries;  // (type, name)
    {
        size_t pos = 0;
        while (pos < top.out.size()) {
            size_t end = top.out.find('\0', pos);
            if (end == std::string::npos) end = top.out.size();
            const std::string rec = top.out.substr(pos, end - pos);
            pos = end + 1;
            const size_t tab = rec.find('\t');
            if (tab == std::string::npos) continue;
            const size_t sp1 = rec.find(' ');
            const size_t sp2 = sp1 == std::string::npos ? sp1 : rec.find(' ', sp1 + 1);
            if (sp2 == std::string::npos || sp2 > tab) continue;
            rootEntries.emplace_back(rec.substr(sp1 + 1, sp2 - sp1 - 1), rec.substr(tab + 1));
        }
    }
    if (subpathsIn.empty()) {
        for (const auto& [type, entry] : rootEntries) {
            if (type == "blob" || ((entry == "src" || entry == "lib") && type == "tree")) paths.push_back(entry);
        }
    } else {
        for (const auto& s : subpathsIn) {
            if (run({"cat-file", "-e", *sha + ":" + s}, 16).ok) paths.push_back(s);
        }
    }
    if (paths.empty()) throw std::runtime_error("git: nothing to export from '" + ref + "'");

    const fs::path tmpDir =
        fs::path(cacheRoot) / (name + ".tmp-" + std::to_string(platform::processId()) + "-" + std::to_string(counter++));
    fs::remove_all(tmpDir, ec);

    std::vector<std::string> args = {"archive", "--format=tar", *sha, "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    platform::ProcessSpec spec;
    spec.argv.push_back(options_.executable);
    spec.argv.insert(spec.argv.end(), {"-C", dir_});
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.extraEnv = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_OPTIONAL_LOCKS", "0"}, {"LC_ALL", "C"}};
    spec.timeoutMs = options_.timeoutMs * 4;
    TarExtractor tar(tmpDir.string());
    spec.onStdout = [&](const char* d, size_t n) { return tar.feed(d, n); };
    const auto res = platform::runProcess(spec);

    auto cleanup = [&] { fs::remove_all(tmpDir, ec); };
    if (!res.started) {
        cleanup();
        throw std::runtime_error("git archive: " + res.error);
    }
    if (res.timedOut) {
        cleanup();
        throw std::runtime_error("git archive timed out");
    }
    if (!tar.error().empty()) {
        cleanup();
        throw std::runtime_error("git archive: " + tar.error());
    }
    if (res.exitCode != 0) {
        cleanup();
        throw std::runtime_error("git archive failed: " + trim(res.stderrText));
    }
    if (!tar.finish()) {
        cleanup();
        throw std::runtime_error("git archive: " + tar.error());
    }
    {
        std::ofstream marker(tmpDir / kCompleteMarker);
        marker << *sha << '\n';
        if (!marker) {
            cleanup();
            throw std::runtime_error("git: cannot write export marker in '" + tmpDir.string() + "'");
        }
    }
    fs::rename(tmpDir, finalDir, ec);
    if (ec) {
        // Lost a race against a concurrent export of the same commit: use theirs when it is complete.
        const bool complete = fs::exists(finalDir / kCompleteMarker);
        cleanup();
        if (!complete) throw std::runtime_error("git: cannot move export into place: " + ec.message());
        return {finalDir.string(), *sha, true};
    }
    return {finalDir.string(), *sha, false};
}

}  // namespace qstate::support
