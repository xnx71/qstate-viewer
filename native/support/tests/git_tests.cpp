#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "git_fixture.h"
#include "qstate/support/git.h"
#include "temp_dir.h"
#include "test_env.h"

using namespace qstate::support;
using qstate::testing::GitFixtureRepo;
namespace fs = std::filesystem;

namespace {

std::string settingsText(int epoch, int a, int b, int c) {
    return "#pragma once\n\n#define VERSION_A " + std::to_string(a) + "\n#define VERSION_B " + std::to_string(b) +
           "\n#define VERSION_C " + std::to_string(c) + "\n\n// comment\n#define EPOCH " + std::to_string(epoch) + "\n";
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// main: one (epoch 10, 1.1.0, tag v1.1.0) -> two (epoch 10, 1.1.1, tag v1.1.1) -> three (epoch 11, annotated tag v1.2.0);
// branch "feature" with one more commit (epoch 12) forked from "three"; a commit without public_settings.h on "nofile".
struct Repo {
    testutil::TempDir dir;
    GitFixtureRepo git{dir.path() / "core"};
    std::string one, two, three, feature, nofile;

    Repo() {
        git.write("src/public_settings.h", settingsText(10, 1, 1, 0));
        git.write("src/contract_core/contract_def.h", "// def v1\n");
        git.write("lib/x.h", "// lib\n");
        git.write("test/t.cpp", "// not exported\n");
        git.write("CMakeLists.txt", "# root\n");
        one = git.commit("one: initial import");
        git.tag("v1.1.0");
        git.write("src/public_settings.h", settingsText(10, 1, 1, 1));
        git.write("src/contract_core/contract_def.h", "// def v2\n");
        two = git.commit("two: fix");
        git.tag("v1.1.1");
        git.write("src/public_settings.h", settingsText(11, 1, 2, 0));
        three = git.commit("three: new epoch");
        git.tag("v1.2.0", /*annotated=*/true);
        git.checkoutNew("feature");
        git.write("src/public_settings.h", settingsText(12, 1, 3, 0));
        feature = git.commit("feature: work in progress");
        git.checkout("main");
        git.checkoutNew("nofile");
        git.remove("src/public_settings.h");
        nofile = git.commit("nofile: delete the settings");
        git.checkout("main");
    }
};

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

    // Windows line endings, tabs, a longer macro with the same prefix, and the first definition wins.
    const auto crlf = parsePublicSettings("#define VERSION_AB 9\r\n#define VERSION_A\t1\r\n#define VERSION_B 2\r\n"
                                          "#define VERSION_C 3\r\n\t#define\tEPOCH\t12\r\n#define EPOCH 99\r\n");
    CHECK(crlf.epoch == std::optional<int>(12));
    CHECK(crlf.version == std::optional<std::string>("1.2.3"));
    // No value, no digits, or no blank after the name: not a match.
    CHECK_FALSE(parsePublicSettings("#define EPOCH\n#define EPOCH abc\n#define EPOCH(x) 5\n").epoch.has_value());
    // No trailing newline at the end of the file.
    CHECK(parsePublicSettings("#define EPOCH 5").epoch == std::optional<int>(5));
}

TEST_CASE("git: ref names and shas that may reach git") {
    for (const char* ok : {"main", "v1.303.2", "feature/x-1", "release_2", "a"}) CHECK_MESSAGE(isValidRefName(ok), ok);
    for (const char* bad : {"", "-h", "--output=x", "a..b", "a b", "a~1", "a^", "a:b", "a?", "a*", "a[", "a\\b", "a@{1}", "/a", "a/",
                            "a//b", "x.lock", "tab\tx", "nl\nx"}) {
        CHECK_MESSAGE(!isValidRefName(bad), bad);
    }
    CHECK(isHexSha(std::string(40, 'a')));
    CHECK(isHexSha(std::string(64, 'F')));
    CHECK_FALSE(isHexSha(std::string(39, 'a')));
    CHECK_FALSE(isHexSha(std::string(40, 'g')));
}

TEST_CASE("git: unavailable executable and non repositories") {
    GitOptions bad;
    bad.executable = "definitely-not-a-git-binary";
    CHECK_FALSE(GitRepo::isAvailable(bad));
    GitRepo repo("/nonexistent-dir-xyz", bad);
    CHECK(repo.listTags().empty());
    CHECK_FALSE(repo.resolve("HEAD").has_value());
    CHECK_THROWS_AS(repo.exportTree(std::string(40, 'a'), "/tmp/never-created"), GitError);

    if (!GitRepo::isAvailable()) {
        MESSAGE("git not installed: remaining checks skipped");
        return;
    }
    testutil::TempDir dir;
    GitRepo plain(dir.path().string());
    CHECK(plain.listTags().empty());
    CHECK(plain.listBranches().empty());
    CHECK_FALSE(plain.defaultBranch().has_value());
    CHECK_FALSE(plain.resolve("main").has_value());
    CHECK_FALSE(plain.showFile(std::string(40, 'a'), "src/public_settings.h").has_value());
}

TEST_CASE("git: refs, resolve, versions, log, export") {
    if (!GitRepo::isAvailable()) return;
    Repo r;
    GitRepo repo(r.git.dir().string());

    SUBCASE("tags and branches") {
        const auto tags = repo.listTags();
        REQUIRE(tags.size() == 3);
        CHECK(tags[0].name == "v1.2.0");
        CHECK(tags[1].name == "v1.1.1");
        CHECK(tags[2].name == "v1.1.0");
        CHECK(tags[0].kind == RefKind::Tag);
        CHECK(tags[0].sha == r.three);  // annotated tag: peeled to the commit
        CHECK(tags[1].sha == r.two);
        CHECK(tags[0].date.rfind("2024-01-", 0) == 0);

        const auto branches = repo.listBranches();
        REQUIRE(branches.size() == 3);
        CHECK(branches[0].name == "main");  // the default branch comes first
        CHECK(branches[0].kind == RefKind::Branch);
        CHECK(branches[0].sha == r.three);
        CHECK(repo.defaultBranch() == std::optional<std::string>("main"));
        std::vector<std::string> names;
        for (const auto& b : branches) names.push_back(b.name);
        CHECK(names == std::vector<std::string>{"main", "nofile", "feature"});  // then newest first
    }

    SUBCASE("resolve") {
        auto tag = repo.resolve("v1.1.1");
        REQUIRE(tag);
        CHECK(tag->kind == RefKind::Tag);
        CHECK(tag->name == "v1.1.1");
        CHECK(tag->sha == r.two);
        auto annotated = repo.resolve("v1.2.0");
        REQUIRE(annotated);
        CHECK(annotated->sha == r.three);
        auto branch = repo.resolve("feature");
        REQUIRE(branch);
        CHECK(branch->kind == RefKind::Branch);
        CHECK(branch->sha == r.feature);
        auto full = repo.resolve(r.one);
        REQUIRE(full);
        CHECK(full->kind == RefKind::Commit);
        CHECK(full->name == r.one);
        auto abbreviated = repo.resolve(r.one.substr(0, 8));
        REQUIRE(abbreviated);
        CHECK(abbreviated->sha == r.one);
        auto head = repo.resolve("HEAD");
        REQUIRE(head);
        CHECK(head->kind == RefKind::Branch);
        CHECK(head->name == "main");
        CHECK(head->sha == r.three);
        CHECK_FALSE(repo.resolve("no-such-ref").has_value());
        CHECK_FALSE(repo.resolve("abc").has_value());  // too short for a sha
        // revision syntax and options never reach git
        for (const char* bad : {"-h", "--all", "main~1", "main^", "main:src/public_settings.h", "HEAD@{1}", "", "--output=/tmp/x"}) {
            CHECK_MESSAGE(!repo.resolve(bad).has_value(), bad);
        }
        // a tag wins over a branch of the same name
        r.git.checkout("main");
        r.git.branch("v1.1.0");
        auto both = repo.resolve("v1.1.0");
        REQUIRE(both);
        CHECK(both->kind == RefKind::Tag);
        CHECK(both->sha == r.one);
    }

    SUBCASE("public settings of many commits in one call") {
        const auto facts = repo.publicSettings({r.one, r.two, r.three, r.feature, r.nofile, "not-a-sha", r.one});
        CHECK(facts.size() == 5);
        CHECK(facts.at(r.one).epoch == std::optional<int>(10));
        CHECK(facts.at(r.one).version == std::optional<std::string>("1.1.0"));
        CHECK(facts.at(r.two).version == std::optional<std::string>("1.1.1"));
        CHECK(facts.at(r.three).epoch == std::optional<int>(11));
        CHECK(facts.at(r.feature).epoch == std::optional<int>(12));
        CHECK_FALSE(facts.at(r.nofile).epoch.has_value());  // file deleted: empty entry
        CHECK_FALSE(facts.at(r.nofile).version.has_value());
        CHECK(repo.publicSettings({}).empty());
    }

    SUBCASE("showFile") {
        CHECK(repo.showFile(r.one, "src/contract_core/contract_def.h") == std::optional<std::string>("// def v1\n"));
        CHECK_FALSE(repo.showFile(r.one, "src/nothing.h").has_value());
        CHECK_FALSE(repo.showFile("--output=/tmp/x", "a").has_value());
        CHECK_FALSE(repo.showFile("v1.1.0", "src/public_settings.h").has_value());  // only full shas
        CHECK_FALSE(repo.showFile(r.one, "../etc/passwd").has_value());
    }

    SUBCASE("log") {
        GitCommitQuery q;
        q.sha = r.three;
        auto page = repo.log(q);
        REQUIRE(page.commits.size() == 3);
        CHECK(page.total == std::optional<size_t>(3));
        CHECK(page.commits[0].sha == r.three);
        CHECK(page.commits[0].subject == "three: new epoch");
        CHECK(page.commits[2].sha == r.one);
        CHECK(page.commits[0].date.rfind("2024-01-", 0) == 0);

        q.limit = 1;
        q.skip = 1;
        page = repo.log(q);
        REQUIRE(page.commits.size() == 1);
        CHECK(page.commits[0].sha == r.two);
        CHECK(page.total == std::optional<size_t>(3));

        q = {};
        q.sha = r.feature;
        CHECK(repo.log(q).commits.size() == 4);  // feature has the three plus its own

        q = {};
        q.sha = r.three;
        q.search = "FIX";
        page = repo.log(q);
        REQUIRE(page.commits.size() == 1);
        CHECK(page.commits[0].sha == r.two);
        CHECK(page.total == std::optional<size_t>(1));
        q.search = r.one.substr(0, 7);  // a sha prefix
        page = repo.log(q);
        REQUIRE(page.commits.size() == 1);
        CHECK(page.commits[0].sha == r.one);
        q.search = "nothing like this";
        page = repo.log(q);
        CHECK(page.commits.empty());
        CHECK(page.total == std::optional<size_t>(0));
        q.search = "o";  // every subject of this history contains an 'o': paging inside a search
        q.skip = 1;
        q.limit = 1;
        page = repo.log(q);
        REQUIRE(page.commits.size() == 1);
        CHECK(page.commits[0].sha == r.two);
        CHECK(page.total == std::optional<size_t>(3));
        q = {};
        q.sha = "main";
        CHECK_THROWS_AS(repo.log(q), GitError);
    }

    SUBCASE("export and its cache") {
        const auto cache = r.dir.path() / "cache";
        const auto ex = repo.exportTree(r.one, cache.string());
        CHECK_FALSE(ex.fromCache);
        CHECK(ex.sha == r.one);
        CHECK(fs::path(ex.dir) == cache / r.one);
        CHECK(readText(fs::path(ex.dir) / "src/contract_core/contract_def.h") == "// def v1\n");
        CHECK(fs::exists(fs::path(ex.dir) / "lib" / "x.h"));
        CHECK(fs::exists(fs::path(ex.dir) / "CMakeLists.txt"));
        CHECK_FALSE(fs::exists(fs::path(ex.dir) / "test"));
        // a second export is served from the cache
        const auto again = repo.exportTree(r.one, cache.string());
        CHECK(again.fromCache);
        CHECK(again.dir == ex.dir);
        // other commit: other directory; other subpaths: other cache entry; missing subpath skipped
        CHECK(repo.exportTree(r.two, cache.string()).dir != ex.dir);
        const auto ex3 = repo.exportTree(r.one, cache.string(), {"src/contract_core", "does/not/exist"});
        CHECK(ex3.dir != ex.dir);
        CHECK(fs::exists(fs::path(ex3.dir) / "src" / "contract_core" / "contract_def.h"));
        CHECK_FALSE(fs::exists(fs::path(ex3.dir) / "lib"));
        CHECK_THROWS_AS(repo.exportTree("main", cache.string()), GitError);  // a full sha is required
        CHECK_THROWS_AS(repo.exportTree(std::string(40, 'a'), cache.string()), GitError);
        CHECK_THROWS_AS(repo.exportTree(r.one, cache.string(), {"../x"}), GitError);
        CHECK_THROWS_AS(repo.exportTree(r.one, cache.string(), {"does/not/exist"}), GitError);
        for (const auto& e : fs::directory_iterator(cache)) CHECK(e.path().filename().string().find(".tmp-") == std::string::npos);
    }

    SUBCASE("a cancelled query throws and leaves no export behind") {
        GitOptions o;
        o.cancel = [] { return true; };
        GitRepo cancelled(r.git.dir().string(), o);
        CHECK_THROWS_AS(cancelled.listTags(), GitError);
        const auto cache = r.dir.path() / "cache-cancel";
        try {
            cancelled.exportTree(r.one, cache.string());
            FAIL("expected a GitError");
        } catch (const GitError& e) {
            CHECK(e.kind() == GitError::Kind::Cancelled);
        }
        CHECK_FALSE(fs::exists(cache / r.one));
    }
}

TEST_CASE("git: real core repository" * doctest::skip(qstate::testing::coreRepo().empty())) {
    if (!GitRepo::isAvailable()) return;
    GitRepo repo(qstate::testing::coreRepo());
    const auto tags = repo.listTags();
    REQUIRE_FALSE(tags.empty());
    CHECK(tags[0].name.rfind("v1.", 0) == 0);
    CHECK_FALSE(tags[0].date.empty());

    const auto v = repo.resolve("v1.303.2");
    if (!v) return;  // older clone without that tag
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> shas;
    for (const auto& t : tags) shas.push_back(t.sha);
    const auto facts = repo.publicSettings(shas);
    const auto t1 = std::chrono::steady_clock::now();
    MESSAGE(tags.size() << " tags: version / epoch of all in " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms");
    CHECK(facts.at(v->sha).epoch == std::optional<int>(229));
    CHECK(facts.at(v->sha).version == std::optional<std::string>("1.303.2"));

    qstate::support::GitCommitQuery q;
    q.sha = v->sha;
    q.limit = 5;
    const auto page = repo.log(q);
    CHECK(page.commits.size() == 5);
    CHECK(page.commits[0].sha == v->sha);

    testutil::TempDir cache;
    const auto ex = repo.exportTree(v->sha, cache.path().string());
    const auto ex2 = repo.exportTree(v->sha, cache.path().string());
    CHECK_FALSE(ex.fromCache);
    CHECK(ex2.fromCache);
    const fs::path def = fs::path(ex.dir) / "src" / "contract_core" / "contract_def.h";
    REQUIRE(fs::exists(def));
    const auto shown = repo.showFile(v->sha, "src/contract_core/contract_def.h");
    REQUIRE(shown);
    CHECK(fs::file_size(def) == shown->size());
    CHECK_FALSE(fs::exists(fs::path(ex.dir) / "test"));
}
