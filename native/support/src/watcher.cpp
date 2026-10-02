#include "qstate/support/watcher.h"

#include <algorithm>
#include <filesystem>

#include "platform.h"

namespace qstate::support {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

bool DirWatcher::Fingerprint::sameAs(const Fingerprint& o) const {
    if (exists != o.exists) return false;
    if (!exists) return true;
    if (isDir != o.isDir) return false;
    if (isDir) return inode == o.inode;  // directory size / mtime change with their content: not interesting
    return size == o.size && mtimeNs == o.mtimeNs && inode == o.inode;
}

DirWatcher::Fingerprint DirWatcher::fingerprintOf(const std::string& path) {
    const auto st = platform::statPath(path);
    Fingerprint f;
    f.exists = st.exists;
    if (!st.exists) return f;
    f.isDir = st.isDir;
    f.size = st.size;
    f.mtimeNs = st.mtimeNs;
    f.inode = st.inode;
    return f;
}

DirWatcher::DirWatcher(Callback callback, WatcherOptions options)
    : callback_(std::move(callback)), options_(options) {}

DirWatcher::~DirWatcher() { stop(); }

void DirWatcher::baselineDirectory(Watch& w) {
    std::error_code ec;
    fs::directory_iterator it(fs::path(w.path), ec);
    if (ec) return;
    const auto at = Clock::now();
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (w.filter && !w.filter(name)) continue;
        EntryState st;
        st.accepted = fingerprintOf((fs::path(w.path) / name).string());
        st.seen = st.accepted;
        st.seenSince = at;
        w.entries[name] = st;
    }
}

DirWatcher::WatchId DirWatcher::addFile(const std::string& path, const std::string& tag) {
    Watch w;
    w.tag = tag;
    w.path = path;
    EntryState st;
    st.accepted = fingerprintOf(path);
    st.seen = st.accepted;
    st.seenSince = Clock::now();
    w.entries[""] = st;
    std::lock_guard<std::mutex> lock(mutex_);
    w.id = nextId_++;
    const WatchId id = w.id;
    watches_.emplace(id, std::move(w));
    return id;
}

DirWatcher::WatchId DirWatcher::addDirectory(const std::string& path, const std::string& tag, NameFilter filter) {
    Watch w;
    w.tag = tag;
    w.path = path;
    w.isDirectory = true;
    w.filter = std::move(filter);
    baselineDirectory(w);
    std::lock_guard<std::mutex> lock(mutex_);
    w.id = nextId_++;
    const WatchId id = w.id;
    watches_.emplace(id, std::move(w));
    return id;
}

std::vector<DirWatcher::WatchId> DirWatcher::addFiles(const std::vector<std::string>& paths, const std::string& tag) {
    std::vector<WatchId> ids;
    ids.reserve(paths.size());
    for (const auto& p : paths) ids.push_back(addFile(p, tag));
    return ids;
}

void DirWatcher::remove(WatchId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    watches_.erase(id);
}

void DirWatcher::removeByTag(const std::string& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = watches_.begin(); it != watches_.end();) {
        it = (it->second.tag == tag) ? watches_.erase(it) : std::next(it);
    }
}

void DirWatcher::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    watches_.clear();
}

size_t DirWatcher::watchCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watches_.size();
}

void DirWatcher::start() {
    std::lock_guard<std::mutex> life(lifecycle_);
    if (thread_.joinable()) {
        if (running_) return;
        thread_.join();  // a previous run ended by a stop() issued from inside a callback
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
        pollRequested_ = false;
    }
    running_ = true;
    thread_ = std::thread([this] { threadMain(); });
}

bool DirWatcher::running() const { return running_; }

void DirWatcher::stop() {
    if (std::this_thread::get_id() == threadId_.load()) {
        // Called from a callback: only request the stop; the thread exits when the callback returns.
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
        running_ = false;
        wake_.notify_all();
        return;
    }
    std::lock_guard<std::mutex> life(lifecycle_);
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
}

void DirWatcher::pollNow() {
    std::lock_guard<std::mutex> lock(mutex_);
    pollRequested_ = true;
    wake_.notify_all();
}

void DirWatcher::observe(Watch& w, const std::string& key, const std::string& path, const Fingerprint& now,
                         Clock::time_point at, std::vector<WatchEvent>& out) {
    auto it = w.entries.find(key);
    if (it == w.entries.end()) {
        // New directory entry: never accepted before, so "accepted" is the non-existing state.
        EntryState st;
        st.seen = Fingerprint{};
        st.seenSince = at;
        it = w.entries.emplace(key, st).first;
    }
    EntryState& st = it->second;
    if (!st.seen.sameAs(now)) {
        st.seen = now;
        st.seenSince = at;
    }
    st.touched = !st.accepted.sameAs(st.seen);
    if (!st.touched) return;

    const bool large = std::max(st.seen.size, st.accepted.size) >= options_.largeFileBytes;
    const auto settle = large ? options_.settleLarge : options_.settle;
    if (at - st.seenSince < settle) return;

    WatchEvent ev;
    ev.watchId = w.id;
    ev.tag = w.tag;
    ev.path = path;
    ev.isDirectory = st.seen.exists ? st.seen.isDir : st.accepted.isDir;
    ev.size = st.seen.exists && !st.seen.isDir ? st.seen.size : 0;
    ev.mtimeMs = st.seen.exists ? st.seen.mtimeNs / 1000000 : 0;
    if (!st.accepted.exists && st.seen.exists) ev.kind = WatchEventKind::Created;
    else if (st.accepted.exists && !st.seen.exists) ev.kind = WatchEventKind::Removed;
    else ev.kind = WatchEventKind::Modified;
    st.accepted = st.seen;
    st.touched = false;
    out.push_back(std::move(ev));
}

void DirWatcher::pollOnce(std::vector<WatchEvent>& out) {
    // Stat calls happen under the lock; they are fast (a handful of paths) and keep add / remove consistent.
    std::lock_guard<std::mutex> lock(mutex_);
    const auto at = Clock::now();
    for (auto& [id, w] : watches_) {
        (void)id;
        if (!w.isDirectory) {
            observe(w, "", w.path, fingerprintOf(w.path), at, out);
            continue;
        }
        std::error_code ec;
        fs::directory_iterator it(fs::path(w.path), ec);
        std::vector<std::string> present;
        if (ec) {
            // Directory gone: all entries disappear. Any other error (permissions, I/O): keep the previous state.
            const bool gone = !platform::statPath(w.path).exists;
            if (!gone) continue;
        } else {
            for (const fs::directory_iterator end; it != end; it.increment(ec)) {
                if (ec) break;
                const std::string name = it->path().filename().string();
                if (w.filter && !w.filter(name)) continue;
                present.push_back(name);
            }
        }
        std::vector<std::string> names = present;
        for (const auto& kv : w.entries) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const auto& name : names) {
            const std::string path = (fs::path(w.path) / name).string();
            observe(w, name, path, fingerprintOf(path), at, out);
        }
        // Forget entries that are gone and reported as removed.
        for (auto e = w.entries.begin(); e != w.entries.end();) {
            const bool forget = !e->second.accepted.exists && !e->second.seen.exists;
            e = forget ? w.entries.erase(e) : std::next(e);
        }
    }
}

void DirWatcher::threadMain() {
    threadId_ = std::this_thread::get_id();
    struct Exit {
        DirWatcher* self;
        ~Exit() {
            self->running_ = false;
            self->threadId_ = std::thread::id();
        }
    } onExit{this};
    std::vector<WatchEvent> events;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopRequested_) return;
        }
        events.clear();
        pollOnce(events);
        for (const auto& ev : events) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopRequested_) return;
            }
            if (callback_) callback_(ev);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopRequested_) return;
        if (!pollRequested_) wake_.wait_for(lock, options_.pollInterval, [this] { return stopRequested_ || pollRequested_; });
        pollRequested_ = false;
        if (stopRequested_) return;
    }
}

}  // namespace qstate::support
