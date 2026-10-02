#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "qstate/support/watcher.h"
#include "temp_dir.h"

using namespace qstate::support;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

WatcherOptions fastOptions() {
    WatcherOptions o;
    o.pollInterval = 15ms;
    o.settle = 80ms;
    o.settleLarge = 160ms;
    o.largeFileBytes = 1ull << 20;
    return o;
}

struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::vector<WatchEvent> events;

    void push(const WatchEvent& e) {
        {
            std::lock_guard<std::mutex> l(m);
            events.push_back(e);
        }
        cv.notify_all();
    }
    bool waitFor(size_t n, std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, timeout, [&] { return events.size() >= n; });
    }
    std::vector<WatchEvent> snapshot() {
        std::lock_guard<std::mutex> l(m);
        return events;
    }
};

void write(const fs::path& p, const std::string& data, bool append = false) {
    std::ofstream f(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    f << data;
}

}  // namespace

TEST_CASE("watcher: existing file is a baseline; modification is reported once after it settles") {
    testutil::TempDir dir;
    const auto p = dir.path() / "a.bin";
    write(p, "hello");
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    w.addFile(p.string(), "state");
    w.start();
    CHECK(w.running());
    std::this_thread::sleep_for(200ms);
    CHECK(c.snapshot().empty());  // baseline: no event for what already exists

    // A write that keeps going (every 30 ms for ~300 ms) must be reported once, after the last write.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; i++) {
        write(p, std::string(10, 'x'), true);
        std::this_thread::sleep_for(30ms);
    }
    const auto lastWrite = std::chrono::steady_clock::now();
    REQUIRE(c.waitFor(1));
    const auto got = std::chrono::steady_clock::now();
    CHECK(got - lastWrite >= 40ms);  // debounce: not reported while the writer was active
    (void)t0;
    std::this_thread::sleep_for(300ms);
    const auto ev = c.snapshot();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == WatchEventKind::Modified);
    CHECK(ev[0].tag == "state");
    CHECK(ev[0].path == p.string());
    CHECK(ev[0].size == 105);
    CHECK_FALSE(ev[0].isDirectory);
    w.stop();
    CHECK_FALSE(w.running());
}

TEST_CASE("watcher: creation, truncation to zero, regrowth and removal of a file") {
    testutil::TempDir dir;
    const auto p = dir.path() / "late.bin";
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    w.addFile(p.string());
    w.start();

    write(p, "12345");
    REQUIRE(c.waitFor(1));
    CHECK(c.snapshot()[0].kind == WatchEventKind::Created);
    CHECK(c.snapshot()[0].size == 5);

    write(p, "");  // truncate (fopen "wb" pattern of core-lite)
    REQUIRE(c.waitFor(2));
    CHECK(c.snapshot()[1].kind == WatchEventKind::Modified);
    CHECK(c.snapshot()[1].size == 0);

    write(p, std::string(1000, 'z'));
    REQUIRE(c.waitFor(3));
    CHECK(c.snapshot()[2].size == 1000);

    fs::remove(p);
    REQUIRE(c.waitFor(4));
    CHECK(c.snapshot()[3].kind == WatchEventKind::Removed);
    CHECK(c.snapshot()[3].size == 0);
    w.stop();
}

TEST_CASE("watcher: truncate-then-regrow within the settle time yields only the final state") {
    testutil::TempDir dir;
    const auto p = dir.path() / "t.bin";
    write(p, std::string(500, 'a'));
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    w.addFile(p.string());
    w.start();
    write(p, "");
    std::this_thread::sleep_for(20ms);
    write(p, std::string(700, 'b'));
    REQUIRE(c.waitFor(1));
    std::this_thread::sleep_for(300ms);
    const auto ev = c.snapshot();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].size == 700);
    w.stop();
}

TEST_CASE("watcher: directory watch reports files, sub directories and respects the filter") {
    testutil::TempDir dir;
    write(dir.path() / "contract0000.229", "x");
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    w.addDirectory(dir.path().string(), "dir", [](const std::string& name) { return name.rfind("contract", 0) == 0 || name.rfind("ep", 0) == 0; });
    w.start();

    write(dir.path() / "contract0001.229", "yy");
    write(dir.path() / "other.txt", "ignored");
    fs::create_directories(dir.path() / "ep229.tmp");
    REQUIRE(c.waitFor(2));
    std::this_thread::sleep_for(250ms);
    auto ev = c.snapshot();
    REQUIRE(ev.size() == 2);
    bool file = false, sub = false;
    for (const auto& e : ev) {
        if (e.path == (dir.path() / "contract0001.229").string()) {
            file = true;
            CHECK(e.kind == WatchEventKind::Created);
            CHECK(e.size == 2);
        }
        if (e.path == (dir.path() / "ep229.tmp").string()) {
            sub = true;
            CHECK(e.isDirectory);
            CHECK(e.kind == WatchEventKind::Created);
        }
    }
    CHECK(file);
    CHECK(sub);

    // A directory renamed into place (core-lite snapshot promotion) is a removal plus a creation.
    fs::rename(dir.path() / "ep229.tmp", dir.path() / "ep229");
    REQUIRE(c.waitFor(4));
    ev = c.snapshot();
    CHECK(ev[2].isDirectory);
    CHECK(ev[3].isDirectory);
    w.stop();
}

TEST_CASE("watcher: removing the watched directory reports its entries as removed") {
    testutil::TempDir dir;
    const auto sub = dir.path() / "sub";
    fs::create_directories(sub);
    write(sub / "f1", "1");
    write(sub / "f2", "2");
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    w.addDirectory(sub.string());
    w.start();
    fs::remove_all(sub);
    REQUIRE(c.waitFor(2));
    for (const auto& e : c.snapshot()) CHECK(e.kind == WatchEventKind::Removed);
    // The directory comes back: creations are reported.
    fs::create_directories(sub);
    write(sub / "f1", "again");
    REQUIRE(c.waitFor(3));
    CHECK(c.snapshot().back().kind == WatchEventKind::Created);
    w.stop();
}

TEST_CASE("watcher: many core header files with a tag, removeByTag, remove, clear") {
    testutil::TempDir dir;
    std::vector<std::string> paths;
    for (int i = 0; i < 5; i++) {
        paths.push_back((dir.path() / ("h" + std::to_string(i) + ".h")).string());
        write(paths.back(), "//");
    }
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    const auto ids = w.addFiles(paths, "core");
    const auto stateId = w.addFile((dir.path() / "s").string(), "state");
    CHECK(w.watchCount() == 6);
    CHECK(ids.size() == 5);
    w.start();
    write(paths[2], "// changed");
    REQUIRE(c.waitFor(1));
    CHECK(c.snapshot()[0].tag == "core");
    CHECK(c.snapshot()[0].watchId == ids[2]);

    w.removeByTag("core");
    CHECK(w.watchCount() == 1);
    write(paths[3], "// changed again");
    write(dir.path() / "s", "now");
    REQUIRE(c.waitFor(2));
    std::this_thread::sleep_for(200ms);
    CHECK(c.snapshot().size() == 2);
    CHECK(c.snapshot()[1].watchId == stateId);
    w.remove(stateId);
    CHECK(w.watchCount() == 0);
    w.addFile(paths[0], "x");
    w.clear();
    CHECK(w.watchCount() == 0);
    w.stop();
}

TEST_CASE("watcher: stop is clean, idempotent, restartable and silences callbacks") {
    testutil::TempDir dir;
    const auto p = dir.path() / "f";
    write(p, "a");
    std::atomic<int> calls{0};
    DirWatcher w([&](const WatchEvent&) { calls++; }, fastOptions());
    w.addFile(p.string());
    w.stop();  // not started: no-op
    w.start();
    w.start();  // idempotent
    w.stop();
    w.stop();
    CHECK_FALSE(w.running());
    write(p, "changed while stopped");
    std::this_thread::sleep_for(250ms);
    CHECK(calls == 0);  // no callback after stop
    w.start();          // restart: the change that happened meanwhile is reported
    for (int i = 0; i < 100 && calls == 0; i++) std::this_thread::sleep_for(20ms);
    CHECK(calls == 1);
    w.stop();
    const int after = calls;
    write(p, "changed after final stop");
    std::this_thread::sleep_for(250ms);
    CHECK(calls == after);
}

TEST_CASE("watcher: callbacks may add watches and stop the watcher") {
    testutil::TempDir dir;
    const auto p = dir.path() / "f";
    write(p, "a");
    std::atomic<int> calls{0};
    DirWatcher* self = nullptr;
    DirWatcher w(
        [&](const WatchEvent&) {
            calls++;
            self->addFile((dir.path() / "another").string());
            self->stop();  // from inside the callback
        },
        fastOptions());
    self = &w;
    w.addFile(p.string());
    w.start();
    write(p, "bbb");
    for (int i = 0; i < 100 && calls == 0; i++) std::this_thread::sleep_for(20ms);
    CHECK(calls == 1);
    write(p, "ccccc");
    std::this_thread::sleep_for(300ms);
    CHECK(calls == 1);  // stop() from the callback silenced everything
    CHECK(w.watchCount() == 2);
    w.start();  // can be restarted afterwards
    for (int i = 0; i < 100 && calls == 1; i++) std::this_thread::sleep_for(20ms);
    CHECK(calls == 2);
    w.stop();
}

TEST_CASE("watcher: large files use the longer settle time") {
    testutil::TempDir dir;
    const auto p = dir.path() / "big";
    write(p, "x");
    Collector c;
    auto opt = fastOptions();
    opt.largeFileBytes = 100;
    opt.settle = 40ms;
    opt.settleLarge = 400ms;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, opt);
    w.addFile(p.string());
    w.start();
    write(p, std::string(200, 'y'));
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(c.waitFor(1));
    CHECK(std::chrono::steady_clock::now() - t0 >= 350ms);
    w.stop();
}

TEST_CASE("watcher: pollNow wakes the thread") {
    testutil::TempDir dir;
    const auto p = dir.path() / "f";
    write(p, "a");
    Collector c;
    auto opt = fastOptions();
    opt.pollInterval = 5s;
    opt.settle = 0ms;
    opt.settleLarge = 0ms;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, opt);
    w.addFile(p.string());
    w.start();
    write(p, "bb");
    w.pollNow();
    CHECK(c.waitFor(1, 1500ms));
    w.stop();  // returns promptly despite the 5 s interval
}

TEST_CASE("watcher: unreadable directory does not produce spurious events") {
#if !defined(_WIN32)
    if (::geteuid() == 0) return;
    testutil::TempDir dir;
    const auto sub = dir.path() / "locked";
    fs::create_directories(sub);
    write(sub / "f", "1");
    Collector c;
    DirWatcher w([&](const WatchEvent& e) { c.push(e); }, fastOptions());
    w.addDirectory(sub.string());
    w.start();
    fs::permissions(sub, fs::perms::none);
    std::this_thread::sleep_for(250ms);
    CHECK(c.snapshot().empty());
    fs::permissions(sub, fs::perms::owner_all);
    w.stop();
#endif
}
