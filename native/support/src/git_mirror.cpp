#include "qstate/support/git_mirror.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "platform.h"
#include "qstate/support/dir_scan.h"
#include "qstate/support/identity.h"
#include "qstate/support/k12.h"

namespace qstate::support {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kMetaFile = "qstate-mirror.json";
constexpr const char* kFactsFile = "qstate-facts.json";

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string nowIso() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// "https://github.com/qubic/core.git" -> "core"; at most 24 characters of [A-Za-z0-9._-].
std::string readableName(const std::string& url) {
    std::string s = url;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0) s.resize(s.size() - 4);
    const size_t cut = s.find_last_of("/\\:");
    if (cut != std::string::npos) s = s.substr(cut + 1);
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') out += c;
        if (out.size() >= 24) break;
    }
    return out.empty() ? "repo" : out;
}

// Splits git's progress stream (lines end in \r while a counter updates, \n when a stage is done).
class ProgressSink {
public:
    ProgressSink(std::string phase, MirrorProgressFn fn) : phase_(std::move(phase)), fn_(std::move(fn)) {}

    void feed(const char* data, size_t size) {
        if (!fn_) return;
        for (size_t i = 0; i < size; i++) {
            if (data[i] == '\r' || data[i] == '\n') {
                flush();
            } else if (line_.size() < 512) {
                line_ += data[i];
            }
        }
    }
    void finish() { flush(); }

private:
    void flush() {
        if (line_.empty()) return;
        const std::string line = std::move(line_);
        line_.clear();
        MirrorProgress p;
        p.phase = phase_;
        if (const auto parsed = parseGitProgressLine(line)) {
            p.message = parsed->text;
            p.percent = overallPercent(*parsed);
            // A counter moves in small steps: tell the listener about whole percents and stage changes only.
            if (p.percent && *p.percent == lastPercent_ && parsed->label == lastLabel_) return;
            lastPercent_ = p.percent.value_or(-1);
            lastLabel_ = parsed->label;
        } else {
            p.message = trim(line);
            if (p.message.rfind("remote: ", 0) == 0) p.message = p.message.substr(8);
            if (p.message.empty() || p.message.rfind("fatal:", 0) == 0 || p.message.rfind("error:", 0) == 0) return;
        }
        fn_(p);
    }

    std::string phase_;
    MirrorProgressFn fn_;
    std::string line_;
    int lastPercent_ = -1;
    std::string lastLabel_;
};

}  // namespace

std::optional<GitProgressLine> parseGitProgressLine(const std::string& raw) {
    // "[remote: ]<Label>: <pp>% (<done>/<total>)[, ...]"
    std::string text = trim(raw);
    if (text.rfind("remote:", 0) == 0) text = trim(text.substr(7));
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0) return std::nullopt;
    const std::string label = text.substr(0, colon);
    if (!std::isalpha(static_cast<unsigned char>(label[0])) ||
        !std::all_of(label.begin(), label.end(), [](unsigned char c) { return std::isalpha(c) || c == ' '; })) {
        return std::nullopt;
    }
    size_t i = colon + 1;
    while (i < text.size() && text[i] == ' ') i++;
    const size_t digitsAt = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
    if (i == digitsAt || i - digitsAt > 3 || i >= text.size() || text[i] != '%') return std::nullopt;
    const int percent = std::stoi(text.substr(digitsAt, i - digitsAt));
    i++;
    while (i < text.size() && text[i] == ' ') i++;
    if (i >= text.size() || text[i] != '(' || text.find(')', i) == std::string::npos) return std::nullopt;
    GitProgressLine line;
    line.label = label;
    line.percent = std::min(100, percent);
    line.text = text;
    return line;
}

std::optional<int> overallPercent(const GitProgressLine& line) {
    struct Stage {
        const char* label;
        int from, to;
    };
    static const Stage stages[] = {{"Counting objects", 0, 5},     {"Compressing objects", 5, 20}, {"Receiving objects", 20, 80},
                                   {"Resolving deltas", 80, 100},  {"Checking connectivity", 100, 100}};
    for (const Stage& s : stages) {
        if (line.label == s.label) return s.from + (s.to - s.from) * line.percent / 100;
    }
    return std::nullopt;
}

GitMirrorStore::GitMirrorStore(std::string reposDir, GitOptions options) : reposDir_(std::move(reposDir)), options_(std::move(options)) {}

std::string GitMirrorStore::checkUrl(const std::string& urlIn) {
    const std::string url = trim(urlIn);
    if (url.empty()) return "the repository URL is empty";
    if (url[0] == '-') return "the repository URL must not start with '-'";
    if (url.size() > 2048) return "the repository URL is too long";
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; })) {
        return "the repository URL contains control characters";
    }
    // `<helper>::<address>` runs a remote helper program (`ext::` runs commands): never.
    size_t name = 0;
    while (name < url.size() && (std::isalnum(static_cast<unsigned char>(url[name])) || url[name] == '+' || url[name] == '.' || url[name] == '-')) name++;
    if (name > 0 && url.compare(name, 2, "::") == 0) return "remote helper URLs ('<helper>::...') are not supported";
    return {};
}

std::string GitMirrorStore::normalizeUrl(const std::string& urlIn) {
    const std::string url = trim(urlIn);
    if (url.empty() || url.find("://") != std::string::npos) return url;
    const std::string path = normalizePath(url);
    if (!path.empty() && isDirectory(path)) return path;
    return url;
}

std::string GitMirrorStore::mirrorDir(const std::string& urlIn) const {
    const std::string url = normalizeUrl(urlIn);
    const auto digest = k12Digest(url.data(), url.size());
    return (fs::path(reposDir_) / (readableName(url) + "-" + toHex(digest.data(), 8) + ".git")).string();
}

bool GitMirrorStore::exists(const std::string& url) const {
    const fs::path dir = mirrorDir(url);
    std::error_code ec;
    return fs::is_regular_file(dir / kMetaFile, ec) && fs::is_regular_file(dir / "HEAD", ec);
}

std::optional<std::string> GitMirrorStore::fetchedAt(const std::string& url) const {
    if (!exists(url)) return std::nullopt;
    const json meta = json::parse(readText(fs::path(mirrorDir(url)) / kMetaFile), nullptr, false);
    if (meta.is_object()) {
        if (auto it = meta.find("fetchedAt"); it != meta.end() && it->is_string()) return it->get<std::string>();
    }
    return std::string();
}

std::shared_ptr<std::mutex> GitMirrorStore::lockFor(const std::string& dir) {
    std::lock_guard lock(locksMutex_);
    auto& slot = locks_[dir];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}

GitOptions GitMirrorStore::optionsWith(std::function<bool()> cancel) const {
    GitOptions o = options_;
    if (cancel) o.cancel = std::move(cancel);
    return o;
}

GitRepo GitMirrorStore::open(const std::string& url, std::function<bool()> cancel) const {
    return GitRepo(mirrorDir(url), optionsWith(std::move(cancel)));
}

GitMirrorStore::SyncResult GitMirrorStore::sync(const std::string& urlIn, Mode mode, const MirrorProgressFn& progress,
                                                const std::function<bool()>& cancel) {
    if (const std::string why = checkUrl(urlIn); !why.empty()) throw GitError(GitError::Kind::BadInput, why);
    const std::string url = normalizeUrl(urlIn);
    const std::string dir = mirrorDir(url);
    const auto mutex = lockFor(dir);
    std::lock_guard lock(*mutex);
    if (cancel && cancel()) throw GitError(GitError::Kind::Cancelled, "cancelled");

    SyncResult result;
    result.dir = dir;
    const bool present = exists(url);
    if (present && mode == Mode::CloneIfMissing) return result;

    static std::atomic<unsigned> counter{0};
    const std::string phase = present ? "fetch" : "clone";
    const fs::path tmp = fs::path(dir + ".tmp-" + std::to_string(platform::processId()) + "-" + std::to_string(counter++));
    std::error_code ec;

    platform::ProcessSpec spec;
    spec.argv.push_back(options_.executable);
    if (present) {
        spec.argv.insert(spec.argv.end(), {"-C", dir, "fetch", "--progress", "--prune", "--no-tags", "--force", "--", "origin",
                                           "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"});
    } else {
        fs::create_directories(reposDir_, ec);
        if (ec) throw GitError(GitError::Kind::Failed, "cannot create '" + reposDir_ + "': " + ec.message());
        fs::remove_all(tmp, ec);
        fs::remove_all(dir, ec);  // an incomplete leftover (no metadata file): not a mirror, start over
        spec.argv.insert(spec.argv.end(), {"clone", "--bare", "--progress", "--", url, tmp.string()});
    }
    spec.extraEnv = gitEnvironment();
    spec.timeoutMs = 0;
    spec.cancel = cancel;
    ProgressSink sink(phase, progress);
    spec.onStderr = [&](const char* d, size_t n) { sink.feed(d, n); };
    if (progress) progress(MirrorProgress{phase, present ? "Fetching " + url : "Cloning " + url, 0});

    const auto res = platform::runProcess(spec);
    sink.finish();
    auto cleanup = [&] {
        if (!present) fs::remove_all(tmp, ec);
    };
    if (res.cancelled) {
        cleanup();
        throw GitError(GitError::Kind::Cancelled, "cancelled");
    }
    if (!res.started) {
        cleanup();
        throw GitError(GitError::Kind::Unavailable, "git is not available: " + res.error);
    }
    if (res.timedOut || res.exitCode != 0) {
        cleanup();
        const std::string why = gitErrorLine(res.stderrText);
        throw GitError(GitError::Kind::Failed, std::string("git ") + (present ? "fetch" : "clone") + " failed: " +
                                                   (why.empty() ? "exit code " + std::to_string(res.exitCode) : why));
    }

    const std::string meta = json{{"url", url}, {"fetchedAt", nowIso()}}.dump(2) + "\n";
    std::string error;
    if (!platform::writeFileAtomic(((present ? fs::path(dir) : tmp) / kMetaFile).string(), meta, &error)) {
        cleanup();
        throw GitError(GitError::Kind::Failed, "cannot write the mirror metadata: " + error);
    }
    if (!present) {
        fs::rename(tmp, dir, ec);
        if (ec) {
            // A concurrent process finished the same clone first: use theirs.
            const bool theirs = exists(url);
            cleanup();
            if (!theirs) throw GitError(GitError::Kind::Failed, "cannot move the clone into place: " + ec.message());
        }
        result.cloned = true;
    } else {
        result.fetched = true;
    }
    if (progress) progress(MirrorProgress{phase, "Done", 100});
    return result;
}

std::map<std::string, PublicSettings> GitMirrorStore::publicSettings(const std::string& url, const std::vector<std::string>& shas,
                                                                     std::function<bool()> cancel) {
    static std::mutex factsMutex;
    const fs::path file = fs::path(mirrorDir(url)) / kFactsFile;
    json memo = json::object();
    std::map<std::string, PublicSettings> out;
    std::vector<std::string> missing;
    {
        std::lock_guard lock(factsMutex);
        json parsed = json::parse(readText(file), nullptr, false);
        if (parsed.is_object()) memo = std::move(parsed);
    }
    for (const std::string& sha : shas) {
        if (out.count(sha) != 0) continue;
        const auto it = memo.find(sha);
        if (it == memo.end() || !it->is_object()) {
            missing.push_back(sha);
            out[sha];
            continue;
        }
        PublicSettings& ps = out[sha];
        if (auto e = it->find("e"); e != it->end() && e->is_number_integer()) ps.epoch = e->get<int>();
        if (auto v = it->find("v"); v != it->end() && v->is_string()) ps.version = v->get<std::string>();
    }
    if (missing.empty()) return out;

    const auto fresh = open(url, std::move(cancel)).publicSettings(missing);
    for (const auto& [sha, ps] : fresh) {
        out[sha] = ps;
        json entry = json::object();
        if (ps.epoch) entry["e"] = *ps.epoch;
        if (ps.version) entry["v"] = *ps.version;
        memo[sha] = std::move(entry);
    }
    std::lock_guard lock(factsMutex);
    // Merge with what another thread wrote meanwhile.
    json current = json::parse(readText(file), nullptr, false);
    if (current.is_object()) {
        for (auto it = current.begin(); it != current.end(); ++it) {
            if (!memo.contains(it.key())) memo[it.key()] = it.value();
        }
    }
    std::string error;
    platform::writeFileAtomic(file.string(), memo.dump(), &error);  // a failed memo write only costs time later
    return out;
}

}  // namespace qstate::support
