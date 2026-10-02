#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "qstate/support/git.h"
#include "temp_dir.h"
#include "test_env.h"

using namespace qstate::support;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

std::string settingsText(int epoch, int a, int b, int c) {
    return "#pragma once\n\n#define VERSION_A " + std::to_string(a) + "\n#define VERSION_B " + std::to_string(b) +
           "\n#define VERSION_C " + std::to_string(c) + "\n\n// comment\n#define EPOCH " + std::to_string(epoch) + "\n";
}

// Runs git in `dir` with a fixed identity and commit date; test-only, the paths are plain temp paths.
int git(const fs::path& dir, const std::string& args, const std::string& date = "2026-01-01T12:00:00+00:00") {
    const std::string cmd = "cd '" + dir.string() + "' && GIT_AUTHOR_DATE='" + date + "' GIT_COMMITTER_DATE='" + date +
                            "' git -c user.name=t -c user.email=t@example.org -c commit.gpgsign=false -c tag.gpgsign=false " +
                            args + " >/dev/null 2>&1";
    return std::system(cmd.c_str());
}

}  // namespace

TEST_CASE("git: parsePublicSettings") {
    const auto info = parsePublicSettings(settingsText(233, 1, 306, 0));
    CHECK(info.epoch == std::optional<int>(233));
    CHECK(info.version == std::optional<std::string>("1.306.0"));
    const auto none = parsePublicSettings("// nothing\n#define EPOCHX 5\n");
    CHECK_FALSE(none.epoch.has_value());
    CHECK_FALSE(none.version.has_value());
    const auto spaced = parsePublicSettings("  #  define   EPOCH   42 // trailing\n#define VERSION_A 1\n");
    CHECK(spaced.epoch == std::optional<int>(42));
    CHECK_FALSE(spaced.version.has_value());
    // A commented out define is not a define.
    CHECK_FALSE(parsePublicSettings("// #define EPOCH 7\n").epoch.has_value());
}

TEST_CASE("git: unavailable executable and non repositories") {
    GitOptions bad;
    bad.executable = "definitely-not-a-git-binary";
    CHECK_FALSE(GitRepo::isAvailable(bad));
    CHECK_FALSE(GitRepo::isRepo("/tmp", bad));
    GitRepo repo("/nonexistent-dir-xyz", bad);
    CHECK(repo.listTags().empty());
    CHECK_FALSE(repo.resolveCommit("HEAD").has_value());
    CHECK_THROWS_AS(repo.exportTree("HEAD", "/tmp/never-created"), std::runtime_error);

    if (!GitRepo::isAvailable()) {
        MESSAGE("git not installed: remaining checks skipped");
        return;
    }
    testutil::TempDir dir;
    CHECK_FALSE(GitRepo::isRepo(dir.path().string()));
    CHECK_FALSE(GitRepo::isRepo((dir.path() / "missing").string()));
    GitRepo plain(dir.path().string());
    CHECK(plain.listTags().empty());
    CHECK_FALSE(plain.showFile("HEAD", "src/public_settings.h").has_value());
    CHECK_FALSE(plain.autoPickRef(1).has_value());
}

TEST_CASE("git: synthetic repository: tags, versions, auto pick, export and cache") {
    if (!GitRepo::isAvailable()) return;
    testutil::TempDir dir;
    const auto repoDir = dir.path() / "core";
    fs::create_directories(repoDir);
    REQUIRE(git(repoDir, "init -q") == 0);

    write(repoDir / "src" / "public_settings.h", settingsText(10, 1, 1, 0));
    write(repoDir / "src" / "contract_core" / "contract_def.h", "// def v1\n");
    write(repoDir / "lib" / "x.h", "// lib\n");
    write(repoDir / "test" / "t.cpp", "// not exported\n");
    write(repoDir / "CMakeLists.txt", "# root\n");
    REQUIRE(git(repoDir, "add -A") == 0);
    REQUIRE(git(repoDir, "commit -q -m one", "2026-01-01T12:00:00+00:00") == 0);
    REQUIRE(git(repoDir, "tag v1.1.0", "2026-01-01T12:00:00+00:00") == 0);

    write(repoDir / "src" / "public_settings.h", settingsText(10, 1, 1, 1));
    write(repoDir / "src" / "contract_core" / "contract_def.h", "// def v2\n");
    REQUIRE(git(repoDir, "commit -q -am two", "2026-01-02T12:00:00+00:00") == 0);
    REQUIRE(git(repoDir, "tag v1.1.1", "2026-01-02T12:00:00+00:00") == 0);

    write(repoDir / "src" / "public_settings.h", settingsText(11, 1, 2, 0));
    REQUIRE(git(repoDir, "commit -q -am three", "2026-01-03T12:00:00+00:00") == 0);
    REQUIRE(git(repoDir, "tag -a v1.2.0 -m annotated", "2026-01-03T12:00:00+00:00") == 0);

    // working tree edit (uncommitted)
    write(repoDir / "src" / "public_settings.h", settingsText(12, 1, 3, 0));

    GitRepo repo(repoDir.string());
    CHECK(repo.isRepo());
    CHECK(GitRepo::isRepo(repoDir.string()));
    CHECK_FALSE(GitRepo::isRepo((repoDir / "src").string()));  // a sub directory is not a repository root

    const auto tags = repo.listTags();
    REQUIRE(tags.size() == 3);
    CHECK(tags[0].name == "v1.2.0");
    CHECK(tags[1].name == "v1.1.1");
    CHECK(tags[2].name == "v1.1.0");
    CHECK(tags[0].commitSha.size() == 40);  // annotated tag: peeled to the commit
    CHECK(tags[0].commitSha == repo.resolveCommit("v1.2.0"));
    CHECK(tags[0].date.rfind("2026-01-03", 0) == 0);
    CHECK(repo.listTags(2).size() == 2);
    CHECK(repo.listTags(2)[1].name == "v1.1.1");

    const auto v = repo.readVersionInfo("v1.1.1");
    REQUIRE(v);
    CHECK(v->epoch == std::optional<int>(10));
    CHECK(v->version == std::optional<std::string>("1.1.1"));
    CHECK(v->sha == repo.resolveCommit("v1.1.1"));
    CHECK(v->date.rfind("2026-01-02", 0) == 0);
    CHECK(v->ref == "v1.1.1");

    const auto wt = repo.readVersionInfo("");
    REQUIRE(wt);
    CHECK(wt->epoch == std::optional<int>(12));
    CHECK(wt->version == std::optional<std::string>("1.3.0"));
    CHECK(wt->sha.size() == 40);  // HEAD
    CHECK(wt->ref.empty());
    CHECK_FALSE(repo.readVersionInfo("no-such-tag").has_value());

    CHECK(repo.autoPickRef(10) == std::optional<std::string>("v1.1.1"));  // newest tag with that epoch
    CHECK(repo.autoPickRef(11) == std::optional<std::string>("v1.2.0"));
    CHECK_FALSE(repo.autoPickRef(99).has_value());

    CHECK(repo.showFile("v1.1.0", "src/contract_core/contract_def.h") == std::optional<std::string>("// def v1\n"));
    CHECK_FALSE(repo.showFile("v1.1.0", "src/nothing.h").has_value());
    CHECK_FALSE(repo.showFile("--output=/tmp/x", "a").has_value());
    CHECK_FALSE(repo.showFile("v1.1.0", "../etc/passwd").has_value());
    CHECK_FALSE(repo.resolveCommit("-h").has_value());
    CHECK_FALSE(repo.resolveCommit("").has_value());
    CHECK_FALSE(repo.resolveCommit("--all").has_value());

    // export
    const auto cache = dir.path() / "cache";
    const auto ex = repo.exportTree("v1.1.0", cache.string());
    CHECK_FALSE(ex.fromCache);
    CHECK(ex.sha == repo.resolveCommit("v1.1.0"));
    CHECK(fs::path(ex.dir) == cache / ex.sha);
    CHECK(fs::exists(fs::path(ex.dir) / "src" / "contract_core" / "contract_def.h"));
    CHECK(fs::exists(fs::path(ex.dir) / "lib" / "x.h"));
    CHECK(fs::exists(fs::path(ex.dir) / "CMakeLists.txt"));
    CHECK_FALSE(fs::exists(fs::path(ex.dir) / "test"));
    {
        std::ifstream f(fs::path(ex.dir) / "src" / "contract_core" / "contract_def.h");
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(s == "// def v1\n");
    }
    // a second export is served from the cache
    const auto again = repo.exportTree("v1.1.0", cache.string());
    CHECK(again.fromCache);
    CHECK(again.dir == ex.dir);
    // different commit, other directory; other subpaths, other cache entry; missing subpath skipped
    const auto ex2 = repo.exportTree("v1.1.1", cache.string());
    CHECK(ex2.dir != ex.dir);
    const auto ex3 = repo.exportTree("v1.1.0", cache.string(), {"src/contract_core", "does/not/exist"});
    CHECK(ex3.dir != ex.dir);
    CHECK(fs::exists(fs::path(ex3.dir) / "src" / "contract_core" / "contract_def.h"));
    CHECK_FALSE(fs::exists(fs::path(ex3.dir) / "lib"));
    CHECK_THROWS_AS(repo.exportTree("nope", cache.string()), std::runtime_error);
    CHECK_THROWS_AS(repo.exportTree("v1.1.0", cache.string(), {"../x"}), std::runtime_error);
    CHECK_THROWS_AS(repo.exportTree("v1.1.0", cache.string(), {"does/not/exist"}), std::runtime_error);
    // no temporary directories are left behind
    for (const auto& e : fs::directory_iterator(cache)) CHECK(e.path().filename().string().find(".tmp-") == std::string::npos);
}

TEST_CASE("git: real core repository" * doctest::skip(qstate::testing::coreDir().empty())) {
    if (!GitRepo::isAvailable()) return;
    const std::string coreDir = qstate::testing::coreDir();
    GitRepo repo(coreDir);
    if (!repo.isRepo()) return;
    const auto tags = repo.listTags(10);
    REQUIRE_FALSE(tags.empty());
    CHECK(tags.size() <= 10);
    CHECK(tags[0].name.rfind("v1.", 0) == 0);
    CHECK_FALSE(tags[0].date.empty());

    const auto v = repo.readVersionInfo("v1.303.2");
    if (!v) return;  // older clone without that tag
    CHECK(v->epoch == std::optional<int>(229));
    CHECK(v->version == std::optional<std::string>("1.303.2"));
    CHECK(repo.autoPickRef(229) == std::optional<std::string>("v1.303.2"));
    CHECK_FALSE(repo.autoPickRef(1).has_value());

    const auto wt = repo.readVersionInfo("");
    REQUIRE(wt);
    CHECK(wt->epoch.has_value());

    testutil::TempDir cache;
    const auto t0 = std::chrono::steady_clock::now();
    const auto ex = repo.exportTree("v1.303.2", cache.path().string());
    const auto t1 = std::chrono::steady_clock::now();
    const auto ex2 = repo.exportTree("v1.303.2", cache.path().string());
    const auto t2 = std::chrono::steady_clock::now();
    CHECK_FALSE(ex.fromCache);
    CHECK(ex2.fromCache);
    MESSAGE("export " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, cached "
                      << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms");
    const fs::path def = fs::path(ex.dir) / "src" / "contract_core" / "contract_def.h";
    REQUIRE(fs::exists(def));
    const auto shown = repo.showFile("v1.303.2", "src/contract_core/contract_def.h");
    REQUIRE(shown);
    CHECK(fs::file_size(def) == shown->size());
    CHECK(fs::exists(fs::path(ex.dir) / "src" / "public_settings.h"));
    CHECK_FALSE(fs::exists(fs::path(ex.dir) / "test"));
}
