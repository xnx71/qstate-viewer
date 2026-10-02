#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "git_fixture.h"
#include "qstate/support/git_mirror.h"
#include "temp_dir.h"
#include "test_env.h"

using namespace qstate::support;
using qstate::testing::GitFixtureRepo;
namespace fs = std::filesystem;

namespace {

std::string settingsText(int epoch) {
    return "#define VERSION_A 1\n#define VERSION_B 2\n#define VERSION_C 3\n#define EPOCH " + std::to_string(epoch) + "\n";
}

struct Source {
    testutil::TempDir dir;
    GitFixtureRepo git{dir.path() / "origin"};
    std::string first, second;
    Source() {
        git.write("src/public_settings.h", settingsText(5));
        first = git.commit("first");
        git.tag("v1.0.0");
        git.write("src/public_settings.h", settingsText(6));
        second = git.commit("second");
        git.tag("v1.1.0", true);
    }
};

std::vector<std::string> names(const std::vector<GitRef>& refs) {
    std::vector<std::string> out;
    for (const auto& r : refs) out.push_back(r.name);
    return out;
}

// The process is gone, or only a zombie that its (new) parent has not reaped yet.
bool processGone(long pid) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return true;
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
}

bool noLeftovers(const fs::path& reposDir) {
    std::error_code ec;
    if (!fs::exists(reposDir, ec)) return true;
    for (const auto& e : fs::directory_iterator(reposDir)) {
        if (e.path().filename().string().find(".tmp-") != std::string::npos) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("git mirror: progress lines") {
    auto p = parseGitProgressLine("Receiving objects:  45% (7483/16627), 1.2 MiB | 2.0 MiB/s");
    REQUIRE(p);
    CHECK(p->label == "Receiving objects");
    CHECK(p->percent == 45);
    CHECK(p->text == "Receiving objects:  45% (7483/16627), 1.2 MiB | 2.0 MiB/s");
    CHECK(overallPercent(*p) == std::optional<int>(20 + 60 * 45 / 100));

    p = parseGitProgressLine("remote: Compressing objects:  12% (58/480)");
    REQUIRE(p);
    CHECK(p->label == "Compressing objects");
    CHECK(p->text.rfind("remote:", 0) == std::string::npos);
    CHECK(overallPercent(*p) == std::optional<int>(5 + 15 * 12 / 100));

    p = parseGitProgressLine("Resolving deltas: 100% (12369/12369), done.");
    REQUIRE(p);
    CHECK(overallPercent(*p) == std::optional<int>(100));
    CHECK_FALSE(parseGitProgressLine("Cloning into bare repository 'x'...").has_value());
    CHECK_FALSE(parseGitProgressLine("remote: Enumerating objects: 16627, done.").has_value());
    CHECK_FALSE(parseGitProgressLine("").has_value());
    GitProgressLine unknown;
    unknown.label = "Updating files";
    CHECK_FALSE(overallPercent(unknown).has_value());
}

TEST_CASE("git mirror: repository URLs") {
    CHECK(GitMirrorStore::checkUrl("https://github.com/qubic/core").empty());
    CHECK(GitMirrorStore::checkUrl("  /some/local/path  ").empty());
    CHECK(GitMirrorStore::checkUrl("git@github.com:qubic/core.git").empty());
    CHECK(GitMirrorStore::checkUrl("file:///tmp/x").empty());
    for (const char* bad : {"", "   ", "-x", "--upload-pack=touch /tmp/x", " --help", "ext::sh -c touch% /tmp/pwned", "fd::17",
                            "https://x/\ny", "a\x01" "b"}) {
        CHECK_MESSAGE(!GitMirrorStore::checkUrl(bad).empty(), bad);
    }
    CHECK_FALSE(GitMirrorStore::checkUrl(std::string(3000, 'a')).empty());

    testutil::TempDir dir;
    GitMirrorStore store((dir.path() / "repos").string());
    const std::string a = store.mirrorDir("https://github.com/qubic/core");
    CHECK(a == store.mirrorDir("  https://github.com/qubic/core "));
    CHECK(a != store.mirrorDir("https://github.com/qubic/core2"));
    CHECK(fs::path(a).parent_path() == dir.path() / "repos");
    CHECK(fs::path(a).filename().string().rfind("core-", 0) == 0);
    CHECK(a.size() > 10);
    // a local directory is normalised: trailing slash and "." segments do not make a second mirror
    CHECK(store.mirrorDir(dir.path().string()) == store.mirrorDir(dir.path().string() + "/"));
    CHECK(store.mirrorDir(dir.path().string()) == store.mirrorDir(dir.path().string() + "/./"));
    CHECK_FALSE(store.exists("https://github.com/qubic/core"));
    CHECK_FALSE(store.fetchedAt("https://github.com/qubic/core").has_value());
}

TEST_CASE("git mirror: clone, fetch, offline use") {
    if (!GitRepo::isAvailable()) return;
    Source src;
    testutil::TempDir cache;
    const auto reposDir = cache.path() / "repos";
    GitMirrorStore store(reposDir.string());
    const std::string url = src.git.url();

    std::vector<MirrorProgress> events;
    auto progress = [&](const MirrorProgress& p) { events.push_back(p); };

    auto r = store.sync(url, GitMirrorStore::Mode::CloneOrFetch, progress);
    CHECK(r.cloned);
    CHECK_FALSE(r.fetched);
    CHECK(store.exists(url));
    CHECK(r.dir == store.mirrorDir(url));
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().phase == "clone");
    CHECK(events.front().percent == std::optional<int>(0));
    CHECK(events.back().percent == std::optional<int>(100));
    for (const auto& e : events) CHECK(e.phase == "clone");
    CHECK(noLeftovers(reposDir));
    const auto stamp = store.fetchedAt(url);
    REQUIRE(stamp);
    CHECK(stamp->size() == 20);  // 2026-01-01T00:00:00Z
    CHECK(stamp->back() == 'Z');

    GitRepo repo = store.open(url);
    CHECK(names(repo.listTags()) == std::vector<std::string>{"v1.1.0", "v1.0.0"});
    CHECK(repo.defaultBranch() == std::optional<std::string>("main"));
    auto head = repo.resolve("main");
    REQUIRE(head);
    CHECK(head->sha == src.second);

    // upstream moves: a new commit + tag + branch, and the first tag disappears
    src.git.write("src/public_settings.h", settingsText(7));
    const std::string third = src.git.commit("third");
    src.git.tag("v1.2.0");
    src.git.branch("topic");
    src.git.git({"tag", "-d", "v1.0.0"});

    // CloneIfMissing leaves an existing mirror alone: no network, no change
    r = store.sync(url, GitMirrorStore::Mode::CloneIfMissing, progress);
    CHECK_FALSE(r.cloned);
    CHECK_FALSE(r.fetched);
    CHECK(names(store.open(url).listTags()) == std::vector<std::string>{"v1.1.0", "v1.0.0"});

    events.clear();
    r = store.sync(url, GitMirrorStore::Mode::CloneOrFetch, progress);
    CHECK_FALSE(r.cloned);
    CHECK(r.fetched);
    REQUIRE_FALSE(events.empty());
    for (const auto& e : events) CHECK(e.phase == "fetch");
    CHECK(events.back().percent == std::optional<int>(100));
    repo = store.open(url);
    CHECK(names(repo.listTags()) == std::vector<std::string>{"v1.2.0", "v1.1.0"});  // the deleted tag is pruned
    CHECK(repo.resolve("main")->sha == third);
    CHECK(repo.resolve("topic").has_value());
    CHECK(noLeftovers(reposDir));

    // a branch deleted upstream disappears, a force-pushed one follows
    src.git.git({"branch", "-D", "topic"});
    src.git.git({"reset", "--hard", "--quiet", src.first});
    store.sync(url, GitMirrorStore::Mode::CloneOrFetch);
    repo = store.open(url);
    CHECK_FALSE(repo.resolve("topic").has_value());
    CHECK(repo.resolve("main")->sha == src.first);

    // the file:// form of the same repository is a mirror of its own and works the same way
    GitMirrorStore store2((cache.path() / "repos2").string());
    CHECK(store2.sync("file://" + url, GitMirrorStore::Mode::CloneOrFetch).cloned);
    CHECK(store2.open("file://" + url).listTags().size() == 2);
}

TEST_CASE("git mirror: facts memo") {
    if (!GitRepo::isAvailable()) return;
    Source src;
    testutil::TempDir cache;
    GitMirrorStore store((cache.path() / "repos").string());
    store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch);
    const auto facts = store.publicSettings(src.git.url(), {src.first, src.second, src.first});
    REQUIRE(facts.size() == 2);
    CHECK(facts.at(src.first).epoch == std::optional<int>(5));
    CHECK(facts.at(src.second).epoch == std::optional<int>(6));
    CHECK(facts.at(src.second).version == std::optional<std::string>("1.2.3"));
    CHECK(fs::exists(fs::path(store.mirrorDir(src.git.url())) / "qstate-facts.json"));

    // answered from the memo: works without a usable git executable
    GitOptions noGit;
    noGit.executable = "definitely-not-a-git-binary";
    GitMirrorStore offline((cache.path() / "repos").string(), noGit);
    const auto again = offline.publicSettings(src.git.url(), {src.first, src.second});
    CHECK(again.at(src.first).epoch == std::optional<int>(5));
    CHECK(again.at(src.second).version == std::optional<std::string>("1.2.3"));

    // a corrupt memo is rebuilt
    {
        std::ofstream out(fs::path(store.mirrorDir(src.git.url())) / "qstate-facts.json");
        out << "{ not json";
    }
    CHECK(store.publicSettings(src.git.url(), {src.second}).at(src.second).epoch == std::optional<int>(6));
}

TEST_CASE("git mirror: bad input never reaches git and leaves nothing behind") {
    if (!GitRepo::isAvailable()) return;
    testutil::TempDir cache;
    const auto reposDir = cache.path() / "repos";
    GitMirrorStore store(reposDir.string());
    const auto pwned = cache.path() / "pwned";
    for (const std::string& bad : {std::string("-x"), std::string("--upload-pack=touch ") + pwned.string(),
                                  "ext::sh -c touch% " + pwned.string(), std::string(""), std::string("a\nb")}) {
        try {
            store.sync(bad, GitMirrorStore::Mode::CloneOrFetch);
            FAIL("expected a GitError for '" << bad << "'");
        } catch (const GitError& e) {
            CHECK_MESSAGE(e.kind() == GitError::Kind::BadInput, bad);
        }
    }
    CHECK_FALSE(fs::exists(pwned));
    CHECK_FALSE(fs::exists(reposDir));

    // not a repository / missing path / unreachable host: a clear error and no partial directory
    testutil::TempDir plain;
    for (const std::string& url : {(plain.path() / "missing").string(), plain.path().string(), std::string("file:///nonexistent/repo"),
                                  std::string("https://127.0.0.1:9/never.git")}) {
        try {
            store.sync(url, GitMirrorStore::Mode::CloneOrFetch);
            FAIL("expected a GitError for " << url);
        } catch (const GitError& e) {
            CHECK_MESSAGE(e.kind() == GitError::Kind::Failed, url);
            CHECK_FALSE(std::string(e.what()).empty());
        }
        CHECK_FALSE(store.exists(url));
        CHECK(noLeftovers(reposDir));
    }

    // an incomplete leftover directory (no metadata) is not a mirror and is replaced by a real clone
    Source src;
    fs::create_directories(store.mirrorDir(src.git.url()) + "/junk");
    CHECK_FALSE(store.exists(src.git.url()));
    CHECK(store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch).cloned);
    CHECK(store.exists(src.git.url()));

    // git missing
    GitOptions noGit;
    noGit.executable = "definitely-not-a-git-binary";
    GitMirrorStore broken((cache.path() / "repos3").string(), noGit);
    try {
        broken.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch);
        FAIL("expected a GitError");
    } catch (const GitError& e) {
        CHECK(e.kind() == GitError::Kind::Unavailable);
    }
}

TEST_CASE("git mirror: cancelling a clone and a fetch") {
    if (!GitRepo::isAvailable()) return;
    Source src;
    testutil::TempDir cache;
    const auto reposDir = cache.path() / "repos";
    GitMirrorStore store(reposDir.string());

    // cancelled before anything started
    CHECK_THROWS_AS(store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch, {}, [] { return true; }), GitError);
    CHECK_FALSE(fs::exists(reposDir));

    // cancelled while git runs (the first progress event fires right before the process starts)
    std::atomic<bool> stop{false};
    try {
        store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch, [&](const MirrorProgress&) { stop = true; },
                   [&] { return stop.load(); });
        // a very fast local clone may finish before the first poll: then it simply succeeded
    } catch (const GitError& e) {
        CHECK(e.kind() == GitError::Kind::Cancelled);
        CHECK_FALSE(store.exists(src.git.url()));
    }
    CHECK(noLeftovers(reposDir));
    // no zombie / child left over
    errno = 0;
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);

    // a fetch cancelled the same way leaves the mirror usable
    store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch);
    stop = false;
    try {
        store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch, [&](const MirrorProgress&) { stop = true; },
                   [&] { return stop.load(); });
    } catch (const GitError& e) {
        CHECK(e.kind() == GitError::Kind::Cancelled);
    }
    CHECK(store.exists(src.git.url()));
    CHECK(store.open(src.git.url()).listTags().size() == 2);
    CHECK(noLeftovers(reposDir));
}

TEST_CASE("git mirror: a hung git process and everything it started is killed on cancel") {
    if (!GitRepo::isAvailable()) return;
    Source src;
    testutil::TempDir cache;
    // A fake `git` that starts a long running child and waits for it (like git and git-remote-https).
    const auto script = cache.path() / "slow-git";
    const auto pidFile = cache.path() / "child.pid";
    {
        std::ofstream out(script);
        out << "#!/bin/sh\nsleep 60 &\necho $! > '" << pidFile.string() << "'\nwait\n";
    }
    fs::permissions(script, fs::perms::owner_all);
    GitOptions slow;
    slow.executable = script.string();
    GitMirrorStore store((cache.path() / "repos").string(), slow);

    std::atomic<bool> stop{false};
    std::thread canceller([&] {
        for (int i = 0; i < 200 && !fs::exists(pidFile); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    try {
        store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch, {}, [&] { return stop.load(); });
        FAIL("expected the sync to be cancelled");
    } catch (const GitError& e) {
        CHECK(e.kind() == GitError::Kind::Cancelled);
    }
    canceller.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    CHECK(noLeftovers(cache.path() / "repos"));

    std::ifstream in(pidFile);
    long pid = 0;
    in >> pid;
    REQUIRE(pid > 1);
    bool gone = false;
    for (int i = 0; i < 100 && !gone; i++) {
        // the orphaned `sleep` is reparented and reaped by init: poll until it is gone (it must not live 60 s)
        gone = processGone(pid);
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(gone);
    errno = 0;
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);
}

TEST_CASE("git mirror: concurrent syncs of one repository") {
    if (!GitRepo::isAvailable()) return;
    Source src;
    testutil::TempDir cache;
    GitMirrorStore store((cache.path() / "repos").string());
    std::atomic<int> cloned{0}, failed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&] {
            try {
                if (store.sync(src.git.url(), GitMirrorStore::Mode::CloneOrFetch).cloned) cloned++;
            } catch (const std::exception&) {
                failed++;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(failed.load() == 0);
    CHECK(cloned.load() == 1);  // the others waited and fetched
    CHECK(store.open(src.git.url()).listTags().size() == 2);
    CHECK(noLeftovers(cache.path() / "repos"));
}

TEST_CASE("git mirror: process environment" ) {
    std::map<std::string, std::string> env;
    for (const auto& kv : gitEnvironment()) env[kv.first] = kv.second;
    CHECK(env["GIT_TERMINAL_PROMPT"] == "0");
    CHECK(env["GIT_PAGER"] == "cat");
    CHECK(env["GIT_ALLOW_PROTOCOL"].find("ext") == std::string::npos);
    CHECK(env["GIT_ALLOW_PROTOCOL"].find("https") != std::string::npos);
    CHECK(env.count("GIT_SSH_COMMAND") == 1);
}

TEST_CASE("git mirror: the real core repository as a local source" * doctest::skip(qstate::testing::coreRepo().empty())) {
    if (!GitRepo::isAvailable()) return;
    testutil::TempDir cache;
    GitMirrorStore store((cache.path() / "repos").string());
    const std::string url = qstate::testing::coreRepo();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(store.sync(url, GitMirrorStore::Mode::CloneOrFetch).cloned);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(store.sync(url, GitMirrorStore::Mode::CloneOrFetch).fetched);
    const auto t2 = std::chrono::steady_clock::now();
    GitRepo repo = store.open(url);
    const auto tags = repo.listTags();
    std::vector<std::string> shas;
    for (const auto& t : tags) shas.push_back(t.sha);
    const auto cold = store.publicSettings(url, shas);
    const auto t3 = std::chrono::steady_clock::now();
    const auto warm = store.publicSettings(url, shas);
    const auto t4 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    MESSAGE("local core: clone " << ms(t0, t1) << " ms, fetch " << ms(t1, t2) << " ms, " << tags.size() << " tags: facts cold "
                                 << ms(t2, t3) << " ms, warm " << ms(t3, t4) << " ms");
    CHECK(cold.size() == warm.size());
    CHECK(ms(t3, t4) < 1000);
}
