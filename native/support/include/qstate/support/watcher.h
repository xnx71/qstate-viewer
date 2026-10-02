// Polling file / directory watcher with the debounce rules of report 06 section 8: a path counts as changed when its
// fingerprint (existence, size, mtime, inode) changed AND then stayed stable for a settle time (so a node that is
// still writing a 1 GB state file produces one event, after the write finished). Truncation to zero and
// disappearance are ordinary changes. Polling (stat) is portable and cheap for the handful of paths watched.
//
// Thread-safety: all member functions may be called from any thread, including from inside the callback (except
// the destructor). Callbacks run on the single watcher thread, never concurrently with each other, never while
// the watcher's internal lock is held, and never after stop() has returned (a stop() issued from inside a callback
// returns immediately and suppresses all further callbacks; the thread is joined by the next stop()/destructor
// call from another thread).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qstate::support {

struct WatcherOptions {
    std::chrono::milliseconds pollInterval{500};
    // Quiet time required before a change is reported. The larger value applies to files >= largeFileBytes
    // (before or after the change).
    std::chrono::milliseconds settle{300};
    std::chrono::milliseconds settleLarge{1000};
    uint64_t largeFileBytes = 64ull * 1024 * 1024;
};

enum class WatchEventKind { Created, Modified, Removed };

struct WatchEvent {
    uint64_t watchId = 0;
    std::string tag;       // the tag given to addFile / addDirectory
    std::string path;      // absolute path of the file (or directory entry) that changed
    WatchEventKind kind = WatchEventKind::Modified;
    bool isDirectory = false;  // a sub directory appeared / disappeared inside a watched directory
    uint64_t size = 0;     // after the change (0 for Removed)
    int64_t mtimeMs = 0;
};

class DirWatcher {
public:
    using WatchId = uint64_t;
    using Callback = std::function<void(const WatchEvent&)>;
    // Decides which entries of a watched directory are reported (default: all). Called with the bare name.
    using NameFilter = std::function<bool(const std::string&)>;

    explicit DirWatcher(Callback callback, WatcherOptions options = {});
    ~DirWatcher();
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

    // Watch one file (it may not exist yet: its creation is reported). The current state is the baseline: nothing is
    // reported for what exists at registration time.
    WatchId addFile(const std::string& path, const std::string& tag = {});
    // Watch the entries of one directory level: files (size / mtime), and sub directories (appearance /
    // disappearance only). Sub directories are not descended into; add them separately when they appear.
    WatchId addDirectory(const std::string& path, const std::string& tag = {}, NameFilter filter = {});
    // Watch many files at once (e.g. the core header files), all with the same tag; returns their ids.
    std::vector<WatchId> addFiles(const std::vector<std::string>& paths, const std::string& tag = {});

    void remove(WatchId id);
    void removeByTag(const std::string& tag);
    void clear();
    size_t watchCount() const;

    void start();
    void stop();
    bool running() const;

    // Asks the watcher thread to poll immediately instead of waiting for the next interval (does not skip settling).
    void pollNow();

private:
    struct Fingerprint {
        bool exists = false;
        bool isDir = false;
        uint64_t size = 0;
        int64_t mtimeNs = 0;
        uint64_t inode = 0;
        bool sameAs(const Fingerprint& o) const;
    };
    struct EntryState {
        Fingerprint accepted;   // last state that was reported (or the baseline)
        Fingerprint seen;       // last observed state
        std::chrono::steady_clock::time_point seenSince;
        bool touched = false;   // seen differs from accepted
    };
    struct Watch {
        WatchId id = 0;
        std::string tag;
        std::string path;
        bool isDirectory = false;
        NameFilter filter;
        std::map<std::string, EntryState> entries;  // file watch: single entry with an empty key
    };

    void threadMain();
    void pollOnce(std::vector<WatchEvent>& out);
    void observe(Watch& w, const std::string& key, const std::string& path, const Fingerprint& now,
                 std::chrono::steady_clock::time_point at, std::vector<WatchEvent>& out);
    static Fingerprint fingerprintOf(const std::string& path);
    static void baselineDirectory(Watch& w);

    Callback callback_;
    WatcherOptions options_;

    mutable std::mutex mutex_;          // watches_, flags
    std::condition_variable wake_;
    std::map<WatchId, Watch> watches_;
    WatchId nextId_ = 1;
    bool stopRequested_ = false;
    bool pollRequested_ = false;

    std::mutex lifecycle_;              // serialises start / stop
    std::thread thread_;
    std::atomic<std::thread::id> threadId_{};
    std::atomic<bool> running_{false};
};

} // namespace qstate::support
