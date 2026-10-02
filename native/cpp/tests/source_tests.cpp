#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "qstate/cpp/source.h"

using namespace qstate::cpp;

TEST_CASE("source: normalizePath") {
    CHECK(normalizePath("a/b/../c/./d") == "a/c/d");
    CHECK(normalizePath("a//b///c") == "a/b/c");
    CHECK(normalizePath("./a") == "a");
    CHECK(normalizePath("a/b/..") == "a");
    CHECK(normalizePath("src/network_messages/../platform/m256.h") == "src/platform/m256.h");
    CHECK(normalizePath("a\\b\\c.h") == "a/b/c.h");
    CHECK(normalizePath("") == "");
    CHECK(normalizePath(".") == "");
    CHECK(normalizePath("a/..") == "");
}

TEST_CASE("source: normalizePath rejects paths escaping the root") {
    CHECK(normalizePath("..") == "");
    CHECK(normalizePath("../x") == "");
    CHECK(normalizePath("a/../../x") == "");
    CHECK(normalizePath("/etc/passwd") == "");
}

TEST_CASE("source: dirName and joinPath") {
    CHECK(dirName("a/b/c.h") == "a/b");
    CHECK(dirName("c.h") == "");
    CHECK(dirName("") == "");
    CHECK(dirName("a/c.h") == "a");
    CHECK(joinPath("a/b", "c.h") == "a/b/c.h");
    CHECK(joinPath("a/b", "../c.h") == "a/c.h");
    CHECK(joinPath("", "x/y.h") == "x/y.h");
    CHECK(joinPath("a", "../../x.h") == "");
    CHECK(joinPath("a/b", "./c//d.h") == "a/b/c/d.h");
}

TEST_CASE("source: MapSource") {
    MapSource src;
    src.add("a/b.h", "hello");
    src.add("c.h", "");
    CHECK(src.read("a/b.h").value() == "hello");
    CHECK(src.read("a/x/../b.h").value() == "hello");
    CHECK(src.read("c.h").value() == "");
    CHECK(!src.read("nope.h").has_value());
    CHECK(src.exists("a/b.h"));
    CHECK(src.exists("./c.h"));
    CHECK(!src.exists("nope.h"));
    MapSource init({{"x.h", "1"}});
    CHECK(init.read("x.h").value() == "1");
}

TEST_CASE("source: DiskSource") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("qstate_source_test_" + std::to_string(std::hash<std::string>{}(__FILE__)));
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    {
        std::ofstream(root / "sub" / "f.h", std::ios::binary) << "line1\r\nline2\n";
        std::ofstream(root / "empty.h", std::ios::binary);
    }
    DiskSource src(root);
    CHECK(src.root() == root);
    CHECK(src.read("sub/f.h").value() == "line1\r\nline2\n");
    CHECK(src.read("sub/../sub/f.h").value() == "line1\r\nline2\n");
    CHECK(src.read("empty.h").value() == "");
    CHECK(src.exists("sub/f.h"));
    CHECK(!src.exists("sub"));          // directories are not files
    CHECK(!src.read("sub").has_value());
    CHECK(!src.read("missing.h").has_value());
    CHECK(!src.exists("missing.h"));
    CHECK(!src.read("../escape.h").has_value());
    CHECK(!src.read("").has_value());
    fs::remove_all(root);
}
