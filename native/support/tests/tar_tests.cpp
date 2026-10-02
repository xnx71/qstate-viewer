#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "qstate/support/tar.h"
#include "temp_dir.h"

using namespace qstate::support;
namespace fs = std::filesystem;

namespace {

std::string octal(uint64_t v, size_t width) {
    std::ostringstream os;
    os << std::oct << v;
    std::string s = os.str();
    return std::string(width - 1 - std::min(width - 1, s.size()), '0') + s + '\0';
}

std::string header(const std::string& name, char type, uint64_t size, const std::string& prefix = "") {
    std::string h(512, '\0');
    std::memcpy(h.data(), name.data(), std::min<size_t>(name.size(), 100));
    std::memcpy(h.data() + 100, octal(0644, 8).data(), 8);
    std::memcpy(h.data() + 108, octal(0, 8).data(), 8);
    std::memcpy(h.data() + 116, octal(0, 8).data(), 8);
    std::memcpy(h.data() + 124, octal(size, 12).data(), 12);
    std::memcpy(h.data() + 136, octal(0, 12).data(), 12);
    std::memset(h.data() + 148, ' ', 8);
    h[156] = type;
    std::memcpy(h.data() + 257, "ustar\0" "00", 8);
    std::memcpy(h.data() + 345, prefix.data(), std::min<size_t>(prefix.size(), 155));
    uint64_t sum = 0;
    for (unsigned char c : h) sum += c;
    const std::string cs = octal(sum, 7);  // 6 digits + NUL
    std::memcpy(h.data() + 148, cs.data(), 7);
    h[155] = ' ';
    return h;
}

std::string entry(const std::string& name, const std::string& data, char type = '0', const std::string& prefix = "") {
    std::string e = header(name, type, data.size(), prefix) + data;
    e.append((512 - data.size() % 512) % 512, '\0');
    return e;
}

std::string pax(const std::string& path) {
    std::string rec = " path=" + path + "\n";
    size_t len = rec.size() + 1;
    while (std::to_string(len).size() + rec.size() != len) len = std::to_string(len).size() + rec.size();
    rec = std::to_string(len) + rec;
    return entry("PaxHeader", rec, 'x');
}

const std::string kEnd(1024, '\0');

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("tar: extracts directories, files, prefixes, pax and gnu long names") {
    testutil::TempDir dir;
    const std::string longName = "deep/" + std::string(120, 'd') + "/file.txt";
    std::string tar;
    tar += entry("pax_global_header", "52 comment=0123456789abcdef0123456789abcdef01234567\n", 'g');
    tar += entry("src/", "", '5');
    tar += entry("src/a.h", "int a;\n");
    tar += entry("big.bin", std::string(1500, 'q'));
    tar += entry("b.h", "int b;", '0', "src/inc");
    tar += pax(longName);
    tar += entry("ignored-name", "long content");
    tar += entry("src/link", "", '2');
    tar += entry("empty", "");
    tar += kEnd;

    TarExtractor x((dir.path() / "out").string());
    // Feed in awkward pieces.
    for (size_t i = 0; i < tar.size();) {
        const size_t n = std::min<size_t>(1 + (i * 7) % 700, tar.size() - i);
        REQUIRE(x.feed(tar.data() + i, n));
        i += n;
    }
    CHECK(x.finish());
    CHECK(x.error().empty());
    const auto out = dir.path() / "out";
    CHECK(slurp(out / "src" / "a.h") == "int a;\n");
    CHECK(slurp(out / "big.bin") == std::string(1500, 'q'));
    CHECK(slurp(out / "src" / "inc" / "b.h") == "int b;");
    CHECK(slurp(out / longName) == "long content");
    CHECK(fs::exists(out / "empty"));
    CHECK(fs::file_size(out / "empty") == 0);
    CHECK_FALSE(fs::exists(out / "src" / "link"));  // links are never created
    CHECK(x.filesWritten() == 5);
    CHECK(x.skippedEntries() == 1);
    CHECK(x.directoriesCreated() == 1);
    CHECK(x.bytesWritten() == 7 + 1500 + 6 + 12);
}

TEST_CASE("tar: unsafe paths are rejected") {
    for (const char* bad : {"../escape.txt", "a/../../escape.txt", "/abs.txt"}) {
        testutil::TempDir dir;
        TarExtractor x((dir.path() / "out").string());
        const std::string tar = entry(bad, "x") + kEnd;
        CHECK_FALSE(x.feed(tar.data(), tar.size()));
        CHECK(x.error().find("unsafe path") != std::string::npos);
        CHECK_FALSE(fs::exists(dir.path() / "escape.txt"));
    }
}

TEST_CASE("tar: corrupt and truncated streams") {
    testutil::TempDir dir;
    {
        TarExtractor x((dir.path() / "o1").string());
        std::string tar = entry("f", "data") + kEnd;
        tar[10] ^= 1;  // damage the name: checksum mismatch
        CHECK_FALSE(x.feed(tar.data(), tar.size()));
        CHECK(x.error().find("checksum") != std::string::npos);
        CHECK_FALSE(x.finish());
    }
    {
        TarExtractor x((dir.path() / "o2").string());
        const std::string tar = entry("f", std::string(2000, 'z'));
        CHECK(x.feed(tar.data(), 700));  // cut inside the data
        CHECK_FALSE(x.finish());
        CHECK(x.error().find("ended inside") != std::string::npos);
    }
    {
        TarExtractor x((dir.path() / "o3").string());
        CHECK(x.finish());  // empty stream is fine
    }
}
