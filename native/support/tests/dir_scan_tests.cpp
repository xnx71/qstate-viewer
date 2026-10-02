#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "qstate/support/dir_scan.h"
#include "temp_dir.h"
#include "test_env.h"

using namespace qstate::support;
namespace fs = std::filesystem;

namespace {

void touch(const fs::path& p, size_t size = 0) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << std::string(size, 'x');
}

}  // namespace

TEST_CASE("dir_scan: file name classification") {
    auto c = parseStateFileName("contract0028.229");
    CHECK(c.kind == FileNameKind::ContractState);
    CHECK(c.contractIndex == 28);
    CHECK(c.ext == std::optional<uint32_t>(229));
    c = parseStateFileName("contract0000.000");
    CHECK(c.kind == FileNameKind::ContractState);
    CHECK(c.ext == std::optional<uint32_t>(0));
    CHECK(parseStateFileName("contract028.229").kind == FileNameKind::Unknown);
    CHECK(parseStateFileName("contract0028.22").kind == FileNameKind::Unknown);
    CHECK(parseStateFileName("contract0028.2290").kind == FileNameKind::Unknown);
    CHECK(parseStateFileName("contract00a8.229").kind == FileNameKind::Unknown);
    CHECK(parseStateFileName("contract0028.229.bak").kind == FileNameKind::Unknown);
    CHECK(parseStateFileName("spectrum.229").kind == FileNameKind::Spectrum);
    CHECK(parseStateFileName("universe.230").ext == std::optional<uint32_t>(230));
    CHECK(parseStateFileName("contract_exec_fees_rec.229").kind == FileNameKind::ContractExecFees);
    CHECK(parseStateFileName("contract_exec_fees_acc.229").kind == FileNameKind::ContractExecFees);
    CHECK(parseStateFileName("system.eoe").kind == FileNameKind::System);
    CHECK(parseStateFileName("ep229-full.zip").kind == FileNameKind::Archive);
    CHECK(parseStateFileName("debug.log").kind == FileNameKind::Ignored);
    CHECK(parseStateFileName("logs.233").kind == FileNameKind::Ignored);
    CHECK(parseStateFileName("spectrum.2xx").kind == FileNameKind::Unknown);
    CHECK(parseStateFileName("notes.txt").kind == FileNameKind::Unknown);
}

TEST_CASE("dir_scan: scanStateDir groups epochs, siblings and snapshots") {
    testutil::TempDir dir;
    const auto root = dir.path();
    touch(root / "contract0000.229", 8192);
    touch(root / "contract0001.229", 100);
    touch(root / "contract0003.229", 50);  // gap at 2
    touch(root / "contract0000.230", 8192);
    touch(root / "contract0001.230", 10);
    touch(root / "contract0000.000", 8192);
    touch(root / "spectrum.229", 64);
    touch(root / "universe.229", 48);
    touch(root / "spectrum.230", 64);
    touch(root / "contract_exec_fees_rec.229", 5408);
    touch(root / "ep229-full.zip", 10);
    touch(root / "notes.txt", 3);
    touch(root / "debug.log", 3);
    touch(root / "system", 3);
    fs::create_directories(root / "ep229");
    fs::create_directories(root / "ep230.tmp");
    fs::create_directories(root / "ep228.old");
    fs::create_directories(root / "logs.233");
    fs::create_directories(root / "epic");
    touch(root / "ep229" / "contract0000.000", 1);  // sub directory content is not scanned

    const auto scan = scanStateDir(root.string());
    REQUIRE(scan.readable);
    CHECK(scan.dir == root.string());
    CHECK(scan.epochNumbers() == std::vector<uint32_t>{0, 229, 230});
    CHECK(scan.newestEpoch() == std::optional<uint32_t>(230));

    const auto* e229 = scan.find(229);
    REQUIRE(e229);
    REQUIRE(e229->contracts.size() == 3);
    CHECK(e229->contracts[0].index == 0);
    CHECK(e229->contracts[1].size == 100);
    CHECK(e229->contracts[2].index == 3);
    CHECK(e229->contracts[2].name == "contract0003.229");
    CHECK(e229->contracts[2].path == (root / "contract0003.229").string());
    CHECK(e229->contracts[2].mtimeMs > 0);
    CHECK_FALSE(e229->contiguous);
    CHECK(e229->missingIndices == std::vector<uint32_t>{2});
    CHECK(scan.find(230)->contiguous);
    CHECK(scan.find(231) == nullptr);

    // others: spectrum x2, universe, fees, zip, notes, system (debug.log ignored)
    std::vector<std::string> names;
    for (const auto& o : scan.others) names.push_back(o.name);
    CHECK(names == std::vector<std::string>{"contract_exec_fees_rec.229", "ep229-full.zip", "notes.txt", "spectrum.229",
                                            "spectrum.230", "system", "universe.229"});
    const auto of229 = scan.othersOfEpoch(229);
    CHECK(of229.size() == 6);  // everything but spectrum.230
    for (const auto& o : of229) CHECK(o.name != "spectrum.230");
    CHECK(scan.others[1].kind == OtherFileKind::Archive);
    CHECK(scan.others[0].kind == OtherFileKind::ContractExecFees);

    REQUIRE(scan.snapshots.size() == 3);
    CHECK(scan.snapshots[0].epoch == 228);
    CHECK(scan.snapshots[0].isOld);
    CHECK(scan.snapshots[1].epoch == 229);
    CHECK_FALSE(scan.snapshots[1].isTmp);
    CHECK(scan.snapshots[2].isTmp);
}

TEST_CASE("dir_scan: scan failure paths") {
    testutil::TempDir dir;
    const auto missing = scanStateDir((dir.path() / "nope").string());
    CHECK_FALSE(missing.readable);
    CHECK_FALSE(missing.error.empty());
    CHECK(missing.epochs.empty());

    touch(dir.path() / "plain.txt", 1);
    const auto notDir = scanStateDir((dir.path() / "plain.txt").string());
    CHECK_FALSE(notDir.readable);

    const auto empty = scanStateDir(dir.path().string());
    CHECK(empty.readable);
    CHECK_FALSE(empty.newestEpoch().has_value());

#if !defined(_WIN32)
    if (::geteuid() != 0) {
        const auto locked = dir.path() / "locked";
        fs::create_directories(locked);
        touch(locked / "contract0000.229", 8);
        fs::permissions(locked, fs::perms::none);
        const auto s = scanStateDir(locked.string());
        CHECK_FALSE(s.readable);
        CHECK(s.error.find("ermission") != std::string::npos);
        const auto l = listDirectory(locked.string());
        CHECK_FALSE(l.ok);
        fs::permissions(locked, fs::perms::owner_all);
    }
#endif
}

TEST_CASE("dir_scan: listDirectory ordering, hidden filter, hints") {
    testutil::TempDir dir;
    const auto root = dir.path();
    fs::create_directories(root / "beta");
    fs::create_directories(root / "Alpha");
    fs::create_directories(root / ".hiddendir");
    fs::create_directories(root / ".git");
    fs::create_directories(root / "src" / "contract_core");
    touch(root / "src" / "contract_core" / "contract_def.h", 1);
    touch(root / "zeta.txt", 5);
    touch(root / "Apple.txt", 7);
    touch(root / ".secret", 1);
    touch(root / "contract0001.229", 1);
    touch(root / "contract0001.231", 1);
    touch(root / "contract0001.230", 1);
#if !defined(_WIN32)
    fs::create_symlink(root / "beta", root / "linkdir");
    fs::create_symlink(root / "doesnotexist", root / "broken");
#endif

    auto r = listDirectory(root.string());
    REQUIRE(r.ok);
    CHECK(r.listing.path == root.string());
    REQUIRE(r.listing.parent.has_value());
    CHECK(*r.listing.parent == root.parent_path().string());
    std::vector<std::string> names;
    for (const auto& e : r.listing.entries) names.push_back(e.name);
    std::vector<std::string> expected = {"Alpha", "beta", "src"};
#if !defined(_WIN32)
    expected.push_back("linkdir");
    std::sort(expected.begin(), expected.end(), lessIgnoreCase);
#endif
#if !defined(_WIN32)
    expected.insert(expected.end(), {"Apple.txt", "broken"});  // case-insensitive: Apple < broken
#else
    expected.push_back("Apple.txt");
#endif
    expected.insert(expected.end(), {"contract0001.229", "contract0001.230", "contract0001.231", "zeta.txt"});
    CHECK(names == expected);
    CHECK(r.listing.entries[0].isDir);
    CHECK_FALSE(r.listing.entries[0].size.has_value());
    CHECK(r.listing.hints.stateEpochs == std::vector<uint32_t>{229, 230, 231});
    for (const auto& e : r.listing.entries) {
        if (e.name.rfind("contract0001.", 0) == 0) {
            REQUIRE(e.state.has_value());
            CHECK(e.state->first == 1);
            CHECK(e.state->second == std::stoul(e.name.substr(13)));
        } else {
            CHECK_FALSE(e.state.has_value());
        }
        if (e.name == "zeta.txt") {
            CHECK(e.size == std::optional<uint64_t>(5));
            CHECK(e.mtimeMs.has_value());
            CHECK(e.path == (root / "zeta.txt").string());
        }
    }

    r = listDirectory(root.string(), true);
    REQUIRE(r.ok);
    names.clear();
    for (const auto& e : r.listing.entries) names.push_back(e.name);
    CHECK(std::find(names.begin(), names.end(), ".secret") != names.end());
    CHECK(names[0] == ".git");  // hidden dirs sort with the other dirs (ASCII '.' first)
}

TEST_CASE("dir_scan: listDirectory failures") {
    testutil::TempDir dir;
    CHECK_FALSE(listDirectory((dir.path() / "missing").string()).ok);
    touch(dir.path() / "f", 1);
    const auto r = listDirectory((dir.path() / "f").string());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("not a directory") != std::string::npos);
    CHECK(r.listing.parent.has_value());
    CHECK_FALSE(listDirectory("").ok);
}

TEST_CASE("dir_scan: path normalisation") {
    CHECK(normalizePath("") == "");
#if !defined(_WIN32)
    CHECK(normalizePath("/") == "/");
    CHECK(normalizePath("/a/b/") == "/a/b");
    CHECK(normalizePath("/a/./b/../c") == "/a/c");
    CHECK(normalizePath("/a///b") == "/a/b");
    CHECK(parentPath("/") == std::nullopt);
    CHECK(parentPath("/a") == std::optional<std::string>("/"));
    CHECK(parentPath("/a/b") == std::optional<std::string>("/a"));
    CHECK(pathSeparator() == '/');
    const std::string home = homeDir();
    if (!home.empty()) {
        CHECK(normalizePath("~") == normalizePath(home));
        CHECK(normalizePath("~/x") == normalizePath(home) + "/x");
    }
    CHECK(normalizePath("relative") == normalizePath(currentDir() + "/relative"));
    CHECK(listDirectory("/").listing.parent == std::nullopt);
#endif
    CHECK(platformName() == "linux");
}

TEST_CASE("dir_scan: real state directory" * doctest::skip(qstate::testing::stateDir().empty())) {
    const auto scan = scanStateDir(qstate::testing::stateDir());
    REQUIRE(scan.readable);
    const auto* set = scan.find(229);
    if (!set) return;
    CHECK(set->contracts.size() == 29);
    CHECK(set->contiguous);
    CHECK(set->contracts[0].size == 8192);
    bool fees = false, spectrum = false, universe = false, zip = false;
    for (const auto& o : scan.othersOfEpoch(229)) {
        fees |= o.kind == OtherFileKind::ContractExecFees && o.size == 156832;
        spectrum |= o.kind == OtherFileKind::Spectrum;
        universe |= o.kind == OtherFileKind::Universe;
        zip |= o.kind == OtherFileKind::Archive;
    }
    CHECK(fees);
    CHECK(spectrum);
    CHECK(universe);
    CHECK(zip);
}
