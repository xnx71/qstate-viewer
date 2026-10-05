#include "qstate/support/git.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include "platform.h"
#include "qstate/support/identity.h"
#include "qstate/support/k12.h"
#include "qstate/support/tar.h"

namespace qstate::support {

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersionHeader = "src/public_settings.h";
constexpr const char* kCompleteMarker = ".qstate-export-complete";

bool safeSubpath(const std::string& p) {
    if (p.empty() || p[0] == '-' || p[0] == '/' || p[0] == ':') return false;
    if (p.find("..") != std::string::npos) return false;
    return std::none_of(p.begin(), p.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F || c == '\\'; });
}

bool isHexPrefix(const std::string& s, size_t minLen = 4) {
    return s.size() >= minLen && s.size() <= 64 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(i);
}

std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Value of `#define <name> <digits>` (first match, one directive per line, blanks allowed around the tokens).
// Hand-written instead of std::regex: std::regex::multiline is not available in every standard library.
std::optional<int> defineValue(const std::string& text, const char* name) {
    const std::string_view wanted(name);
    const auto isBlank = [](char c) { return c == ' ' || c == '\t'; };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string_view line(text.data() + pos, eol - pos);
        pos = eol + 1;

        size_t i = 0;
        const auto skipBlanks = [&]() {
            const size_t begin = i;
            while (i < line.size() && isBlank(line[i])) i++;
            return i > begin;
        };
        skipBlanks();
        if (i >= line.size() || line[i] != '#') continue;
        i++;
        skipBlanks();
        if (line.compare(i, 6, "define") != 0) continue;
        i += 6;
        if (!skipBlanks()) continue;
        if (line.compare(i, wanted.size(), wanted) != 0) continue;
        i += wanted.size();
        if (!skipBlanks()) continue;
        const size_t digits = i;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') i++;
        if (i == digits) continue;
        try {
            return std::stoi(std::string(line.substr(digits, i - digits)));
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> f;
    size_t s = 0;
    while (true) {
        const size_t t = line.find('\t', s);
        f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
        if (t == std::string::npos) break;
        s = t + 1;
    }
    return f;
}

template <class Fn>
void forEachLine(const std::string& text, Fn fn) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        fn(text.substr(pos, eol - pos));
        pos = eol + 1;
    }
}

}  // namespace

const char* refKindName(RefKind kind) {
    switch (kind) {
        case RefKind::Tag: return "tag";
        case RefKind::Branch: return "branch";
        case RefKind::Commit: break;
    }
    return "commit";
}

bool isHexSha(const std::string& s) {
    return (s.size() == 40 || s.size() == 64) &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

bool isValidRefName(const std::string& ref) {
    if (ref.empty() || ref.size() > 255 || ref[0] == '-' || ref[0] == '/' || ref.back() == '/' || ref.back() == '.') return false;
    if (ref.find("..") != std::string::npos || ref.find("@{") != std::string::npos || ref.find("//") != std::string::npos) return false;
    if (ref.size() >= 5 && ref.compare(ref.size() - 5, 5, ".lock") == 0) return false;
    return std::none_of(ref.begin(), ref.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7F || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\';
    });
}

PublicSettings parsePublicSettings(const std::string& text) {
    PublicSettings info;
    info.epoch = defineValue(text, "EPOCH");
    const auto a = defineValue(text, "VERSION_A");
    const auto b = defineValue(text, "VERSION_B");
    const auto c = defineValue(text, "VERSION_C");
    if (a && b && c) info.version = std::to_string(*a) + "." + std::to_string(*b) + "." + std::to_string(*c);
    return info;
}

std::string gitErrorLine(const std::string& stderrText) {
    std::string last, lastAny;
    size_t pos = 0;
    while (pos < stderrText.size()) {
        size_t eol = stderrText.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = stderrText.size();
        const std::string line = trim(stderrText.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty()) continue;
        lastAny = line;
        if (line.rfind("fatal:", 0) == 0 || line.rfind("error:", 0) == 0) last = line;
    }
    return last.empty() ? lastAny : last;
}

std::vector<std::pair<std::string, std::string>> gitEnvironment() {
    std::vector<std::pair<std::string, std::string>> env = {{"GIT_TERMINAL_PROMPT", "0"},
                                                            {"GCM_INTERACTIVE", "never"},
                                                            {"GIT_OPTIONAL_LOCKS", "0"},
                                                            {"GIT_ALLOW_PROTOCOL", "file:git:http:https:ssh"},
                                                            {"LC_ALL", "C"},
                                                            {"GIT_PAGER", "cat"}};
    // ssh must fail instead of asking (a passphrase or an unknown host key) on a terminal that does not exist;
    // a user who configured GIT_SSH_COMMAND keeps their command.
    if (const char* ssh = std::getenv("GIT_SSH_COMMAND"); ssh == nullptr || *ssh == '\0') {
        env.emplace_back("GIT_SSH_COMMAND", "ssh -o BatchMode=yes");
    }
    return env;
}

GitRepo::GitRepo(std::string dir, GitOptions options) : dir_(std::move(dir)), options_(std::move(options)) {}

GitRepo::RunResult GitRepo::run(const std::vector<std::string>& args, size_t maxOutput) const {
    platform::ProcessSpec spec;
    spec.argv.push_back(options_.executable);
    spec.argv.insert(spec.argv.end(), {"-C", dir_});
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.extraEnv = gitEnvironment();
    spec.timeoutMs = options_.timeoutMs;
    spec.cancel = options_.cancel;
    RunResult r;
    spec.onStdout = [&](const char* d, size_t n) {
        if (r.out.size() + n > maxOutput) return false;
        r.out.append(d, n);
        return true;
    };
    const auto res = platform::runProcess(spec);
    if (res.cancelled) throw GitError(GitError::Kind::Cancelled, "cancelled");
    r.err = res.stderrText.empty() ? res.error : gitErrorLine(res.stderrText);
    r.exitCode = res.exitCode;
    r.ok = res.started && !res.timedOut && !res.aborted && res.exitCode == 0;
    if (res.timedOut && r.err.empty()) r.err = "git timed out";
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

std::optional<GitRepo::Resolved> GitRepo::resolve(const std::string& ref) const {
    if (!isValidRefName(ref)) return std::nullopt;
    auto commitOf = [&](const std::string& full) -> std::optional<std::string> {
        const auto r = run({"rev-parse", "--verify", "--quiet", full + "^{commit}"}, 4096);
        if (!r.ok) return std::nullopt;
        const std::string sha = trim(r.out);
        if (!isHexSha(sha)) return std::nullopt;
        return sha;
    };
    if (const auto sha = commitOf("refs/tags/" + ref)) return Resolved{ref, RefKind::Tag, *sha};
    if (const auto sha = commitOf("refs/heads/" + ref)) return Resolved{ref, RefKind::Branch, *sha};
    if (ref == "HEAD") {
        if (const auto branch = defaultBranch()) {
            if (const auto sha = commitOf("refs/heads/" + *branch)) return Resolved{*branch, RefKind::Branch, *sha};
        }
        return std::nullopt;
    }
    if (isHexPrefix(ref)) {
        if (const auto sha = commitOf(ref)) return Resolved{*sha, RefKind::Commit, *sha};
    }
    return std::nullopt;
}

std::vector<GitRef> GitRepo::listRefs(const char* pattern, RefKind kind) const {
    const std::string prefix = std::string(pattern) + "/";
    const auto r = run({"for-each-ref", "--sort=-creatordate",
                        "--format=%(refname)%09%(objectname)%09%(*objectname)%09%(creatordate:iso-strict)", pattern});
    std::vector<GitRef> refs;
    if (!r.ok) return refs;
    forEachLine(r.out, [&](const std::string& line) {
        const auto f = splitTabs(line);
        if (f.size() < 4 || f[0].rfind(prefix, 0) != 0) return;
        GitRef ref;
        ref.name = f[0].substr(prefix.size());
        ref.kind = kind;
        ref.sha = f[2].empty() ? f[1] : f[2];  // annotated tags: the peeled commit
        ref.date = f[3];
        if (!ref.name.empty()) refs.push_back(std::move(ref));
    });
    return refs;
}

std::vector<GitRef> GitRepo::listTags() const {
    return listRefs("refs/tags", RefKind::Tag);
}

std::optional<std::string> GitRepo::defaultBranch() const {
    const auto head = run({"symbolic-ref", "--quiet", "HEAD"}, 4096);
    const std::string prefix = "refs/heads/";
    if (head.ok) {
        const std::string name = trim(head.out);
        if (name.rfind(prefix, 0) == 0 &&
            run({"rev-parse", "--verify", "--quiet", name + "^{commit}"}, 4096).ok) {
            return name.substr(prefix.size());
        }
    }
    // HEAD dangling (the remote's default branch was renamed): main, master, else the newest branch.
    const auto branches = listRefs("refs/heads", RefKind::Branch);
    for (const char* wanted : {"main", "master"}) {
        for (const auto& b : branches) {
            if (b.name == wanted) return b.name;
        }
    }
    if (!branches.empty()) return branches.front().name;
    return std::nullopt;
}

std::vector<GitRef> GitRepo::listBranches() const {
    std::vector<GitRef> branches = listRefs("refs/heads", RefKind::Branch);
    if (const auto def = defaultBranch()) {
        std::stable_partition(branches.begin(), branches.end(), [&](const GitRef& b) { return b.name == *def; });
    }
    return branches;
}

std::map<std::string, PublicSettings> GitRepo::publicSettings(const std::vector<std::string>& shas) const {
    std::map<std::string, PublicSettings> out;
    std::vector<std::string> todo;
    for (const auto& sha : shas) {
        if (!isHexSha(sha)) continue;
        if (out.emplace(sha, PublicSettings{}).second) todo.push_back(sha);
    }
    constexpr size_t kBatch = 300;
    for (size_t first = 0; first < todo.size(); first += kBatch) {
        const size_t last = std::min(todo.size(), first + kBatch);
        std::vector<std::string> args = {
            "grep", "-I", "-E",
            R"(^[[:space:]]*#[[:space:]]*define[[:space:]]+(EPOCH|VERSION_A|VERSION_B|VERSION_C)[[:space:]]+[0-9]+)"};
        args.insert(args.end(), todo.begin() + static_cast<std::ptrdiff_t>(first), todo.begin() + static_cast<std::ptrdiff_t>(last));
        args.insert(args.end(), {"--", kVersionHeader});
        const auto r = run(args);
        if (!r.ok && r.exitCode != 1) throw GitError(GitError::Kind::Failed, "git grep failed: " + r.err);
        // Lines: <sha>:src/public_settings.h:<text of the line>
        std::map<std::string, std::string> texts;
        const std::string marker = std::string(":") + kVersionHeader + ":";
        forEachLine(r.out, [&](const std::string& line) {
            const size_t at = line.find(marker);
            if (at == std::string::npos) return;
            texts[line.substr(0, at)] += line.substr(at + marker.size()) + "\n";
        });
        for (const auto& [sha, text] : texts) {
            if (auto it = out.find(sha); it != out.end()) it->second = parsePublicSettings(text);
        }
    }
    return out;
}

std::optional<std::string> GitRepo::showFile(const std::string& sha, const std::string& path) const {
    if (!isHexSha(sha) || !safeSubpath(path)) return std::nullopt;
    const auto r = run({"cat-file", "blob", sha + ":" + path});
    if (!r.ok) return std::nullopt;
    return r.out;
}

GitCommitPage GitRepo::log(const GitCommitQuery& q) const {
    GitCommitPage page;
    if (!isHexSha(q.sha)) throw GitError(GitError::Kind::BadInput, "log: not a commit sha");
    const char* format = "--format=%H%x1f%cI%x1f%s";
    auto parse = [](const std::string& line) -> std::optional<GitCommit> {
        const size_t a = line.find('\x1f');
        const size_t b = a == std::string::npos ? a : line.find('\x1f', a + 1);
        if (b == std::string::npos) return std::nullopt;
        return GitCommit{line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)};
    };
    if (q.search.empty()) {
        const auto r = run({"log", format, "-n", std::to_string(q.skip + q.limit), q.sha, "--"});
        if (!r.ok) throw GitError(GitError::Kind::Failed, "git log failed: " + r.err);
        size_t index = 0;
        forEachLine(r.out, [&](const std::string& line) {
            if (const auto c = parse(line); c && index++ >= q.skip) page.commits.push_back(*c);
        });
        const auto count = run({"rev-list", "--count", q.sha, "--"}, 64);
        if (count.ok) {
            try {
                page.total = static_cast<size_t>(std::stoull(trim(count.out)));
            } catch (...) {
            }
        }
        return page;
    }
    // Search: scan the history (bounded) and filter here; git's --grep cannot match the sha as well.
    const auto r = run({"log", format, q.sha, "--"}, 128u << 20);
    if (!r.ok && r.out.empty()) throw GitError(GitError::Kind::Failed, "git log failed: " + r.err);
    const std::string needle = lowerCase(q.search);
    const bool shaLike = isHexPrefix(needle);
    size_t matches = 0;
    forEachLine(r.out, [&](const std::string& line) {
        const auto c = parse(line);
        if (!c) return;
        const bool hit = lowerCase(c->subject).find(needle) != std::string::npos || (shaLike && lowerCase(c->sha).rfind(needle, 0) == 0);
        if (!hit) return;
        if (matches >= q.skip && page.commits.size() < q.limit) page.commits.push_back(*c);
        matches++;
    });
    if (r.ok) page.total = matches;   // a cut-off scan (output cap) has no reliable total
    return page;
}

GitExportResult GitRepo::exportTree(const std::string& shaIn, const std::string& cacheRoot,
                                    const std::vector<std::string>& subpathsIn) const {
    static std::atomic<unsigned> counter{0};
    if (!isHexSha(shaIn)) throw GitError(GitError::Kind::BadInput, "git: export needs a full commit sha, got '" + shaIn + "'");
    const std::optional<std::string> sha = shaIn;
    for (const auto& s : subpathsIn) {
        if (!safeSubpath(s)) throw GitError(GitError::Kind::BadInput, "git: unsafe export path '" + s + "'");
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
    if (ec) throw GitError(GitError::Kind::Failed, "git: cannot create cache directory '" + cacheRoot + "': " + ec.message());

    // Which paths to archive.
    std::vector<std::string> paths;
    const auto top = run({"ls-tree", "-z", *sha});
    if (!top.ok) throw GitError(GitError::Kind::Failed, "git: ls-tree failed: " + top.err);
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
    if (paths.empty()) throw GitError(GitError::Kind::Failed, "git: nothing to export from commit " + *sha);

    const fs::path tmpDir =
        fs::path(cacheRoot) / (name + ".tmp-" + std::to_string(platform::processId()) + "-" + std::to_string(counter++));
    fs::remove_all(tmpDir, ec);

    std::vector<std::string> args = {"archive", "--format=tar", *sha, "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    platform::ProcessSpec spec;
    spec.argv.push_back(options_.executable);
    spec.argv.insert(spec.argv.end(), {"-C", dir_});
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.extraEnv = gitEnvironment();
    spec.timeoutMs = options_.timeoutMs * 4;
    spec.cancel = options_.cancel;
    TarExtractor tar(tmpDir.string());
    spec.onStdout = [&](const char* d, size_t n) { return tar.feed(d, n); };
    const auto res = platform::runProcess(spec);

    auto cleanup = [&] { fs::remove_all(tmpDir, ec); };
    if (res.cancelled) {
        cleanup();
        throw GitError(GitError::Kind::Cancelled, "cancelled");
    }
    if (!res.started) {
        cleanup();
        throw GitError(GitError::Kind::Failed, "git archive: " + res.error);
    }
    if (res.timedOut) {
        cleanup();
        throw GitError(GitError::Kind::Failed, "git archive timed out");
    }
    if (!tar.error().empty()) {
        cleanup();
        throw GitError(GitError::Kind::Failed, "git archive: " + tar.error());
    }
    if (res.exitCode != 0) {
        cleanup();
        throw GitError(GitError::Kind::Failed, "git archive failed: " + gitErrorLine(res.stderrText));
    }
    if (!tar.finish()) {
        cleanup();
        throw GitError(GitError::Kind::Failed, "git archive: " + tar.error());
    }
    {
        std::ofstream marker(tmpDir / kCompleteMarker);
        marker << *sha << '\n';
        if (!marker) {
            cleanup();
            throw GitError(GitError::Kind::Failed, "git: cannot write export marker in '" + tmpDir.string() + "'");
        }
    }
    fs::rename(tmpDir, finalDir, ec);
    if (ec) {
        // Lost a race against a concurrent export of the same commit: use theirs when it is complete.
        const bool complete = fs::exists(finalDir / kCompleteMarker);
        cleanup();
        if (!complete) throw GitError(GitError::Kind::Failed, "git: cannot move export into place: " + ec.message());
        return {finalDir.string(), *sha, true};
    }
    return {finalDir.string(), *sha, false};
}

}  // namespace qstate::support
