#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "qstate/support/file_reader.h"
#include "temp_dir.h"
#include "test_env.h"

using namespace qstate::support;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string patternData(size_t n) {
    std::string s(n, '\0');
    std::mt19937 rng(99);
    for (auto& c : s) c = static_cast<char>(rng());
    return s;
}

}  // namespace

TEST_CASE("file_reader: basic reads, short reads at EOF, strings") {
    testutil::TempDir dir;
    const auto p = dir.path() / "a.bin";
    writeFile(p, "0123456789");
    FileReader r(p.string());
    CHECK(r.size() == 10);
    CHECK(r.path() == p.string());
    char buf[16] = {};
    CHECK(r.read(0, 4, buf) == 4);
    CHECK(std::string(buf, 4) == "0123");
    CHECK(r.read(6, 16, buf) == 4);  // short read at EOF
    CHECK(std::string(buf, 4) == "6789");
    CHECK(r.read(10, 4, buf) == 0);
    CHECK(r.read(1000, 4, buf) == 0);
    CHECK(r.read(3, 0, buf) == 0);
    CHECK(r.readString(2, 3) == "234");
    CHECK(r.readString(8, 100) == "89");
    CHECK(r.readBytes(9, 5).size() == 1);
}

TEST_CASE("file_reader: failure paths") {
    testutil::TempDir dir;
    CHECK_THROWS_AS(FileReader((dir.path() / "missing").string()), FileError);
    std::string err;
    CHECK(FileReader::tryOpen((dir.path() / "missing").string(), &err) == nullptr);
    CHECK_FALSE(err.empty());
    CHECK(FileReader::tryOpen((dir.path() / "missing").string()) == nullptr);
    // A directory cannot be read as a file (open succeeds on POSIX, read fails with EISDIR).
    auto d = FileReader::tryOpen(dir.path().string());
    if (d) CHECK_THROWS_AS(d->readBytes(0, 10), FileError);

#if !defined(_WIN32)
    if (::geteuid() != 0) {
        const auto p = dir.path() / "secret";
        writeFile(p, "x");
        fs::permissions(p, fs::perms::none);
        std::string e;
        CHECK(FileReader::tryOpen(p.string(), &e) == nullptr);
        CHECK(e.find("ermission") != std::string::npos);
        fs::permissions(p, fs::perms::owner_all);
    }
#endif
}

TEST_CASE("file_reader: truncation and growth are visible after refresh, reads never throw for EOF") {
    testutil::TempDir dir;
    const auto p = dir.path() / "t.bin";
    writeFile(p, std::string(1000, 'a'));
    FileReader r(p.string());
    CHECK(r.size() == 1000);
    CHECK(r.refresh() == RefreshResult::Unchanged);

    fs::resize_file(p, 100);  // truncate in place: same inode
    char buf[1000];
    CHECK(r.read(0, 1000, buf) == 100);  // live read reports the short read
    CHECK(r.size() == 1000);             // cached until refresh
    CHECK(r.refresh() == RefreshResult::Changed);
    CHECK(r.size() == 100);

    {
        std::ofstream f(p, std::ios::binary | std::ios::app);
        f << std::string(50, 'b');
    }
    CHECK(r.refresh() == RefreshResult::Changed);
    CHECK(r.size() == 150);
    CHECK(r.readString(98, 4) == "aabb");
    CHECK(r.refresh() == RefreshResult::Unchanged);
}

TEST_CASE("file_reader: refresh follows a file replaced by rename and reports a missing path") {
    testutil::TempDir dir;
    const auto p = dir.path() / "r.bin";
    writeFile(p, "old content");
    FileReader r(p.string());
    CHECK(r.readString(0, 3) == "old");

    const auto q = dir.path() / "r.new";
    writeFile(q, "NEW CONTENT, longer");
    fs::rename(q, p);
    CHECK(r.readString(0, 3) == "old");  // still the old inode until refreshed
    CHECK(r.refresh() == RefreshResult::Changed);
    CHECK(r.readString(0, 3) == "NEW");
    CHECK(r.size() == 19);

    fs::remove(p);
    CHECK(r.refresh() == RefreshResult::Missing);
    CHECK(r.readString(0, 3) == "NEW");  // the open handle keeps working
}

TEST_CASE("file_reader: isAllZero / firstNonZero") {
    testutil::TempDir dir;
    const auto p = dir.path() / "z.bin";
    std::string data(5 * 1024 * 1024 + 123, '\0');
    data[3 * 1024 * 1024 + 7] = 1;
    writeFile(p, data);
    FileReader r(p.string());
    CHECK(r.isAllZero(0, 3 * 1024 * 1024));
    CHECK(r.isAllZero(0, 3 * 1024 * 1024 + 7));
    CHECK_FALSE(r.isAllZero(0, 3 * 1024 * 1024 + 8));
    CHECK(r.isAllZero(3 * 1024 * 1024 + 8, 2 * 1024 * 1024));
    CHECK(r.isAllZero(100, 0));
    CHECK_FALSE(r.isAllZero(0, data.size() + 1));  // not fully inside the file
    CHECK_FALSE(r.isAllZero(data.size(), 1));
    CHECK(r.firstNonZero(0) == std::optional<uint64_t>(3 * 1024 * 1024 + 7));
    CHECK(r.firstNonZero(3 * 1024 * 1024 + 8) == std::nullopt);
    CHECK(r.firstNonZero(3 * 1024 * 1024 + 7, 1) == std::optional<uint64_t>(3 * 1024 * 1024 + 7));
    CHECK(r.firstNonZero(0, 100) == std::nullopt);
    CHECK(r.firstNonZero(data.size() + 10) == std::nullopt);
}

TEST_CASE("file_reader: findAll handles block borders, overlaps, alignment, limits") {
    testutil::TempDir dir;
    const auto p = dir.path() / "f.bin";
    const size_t block = 4 * 1024 * 1024;
    std::string data(3 * block + 1000, '\0');
    const std::string key = "\x01\x02\x03\x04\x05\x06\x07\x08";
    std::vector<uint64_t> expected;
    // Matches: start, straddling the first border, exactly at a border, inside, at the very end.
    for (uint64_t off : {uint64_t{0}, block - 3, 2 * block, block + 17, data.size() - key.size()}) {
        std::memcpy(data.data() + off, key.data(), key.size());
        expected.push_back(off);
    }
    std::sort(expected.begin(), expected.end());
    writeFile(p, data);
    FileReader r(p.string());
    CHECK(r.findAll(key) == expected);
    CHECK(r.findAll(key, 0, kToEnd, 1, 2) == std::vector<uint64_t>(expected.begin(), expected.begin() + 2));
    CHECK(r.findAll(key, 1) == std::vector<uint64_t>(expected.begin() + 1, expected.end()));
    CHECK(r.findAll(key, 0, block - 3 + 7).size() == 1);   // range ends one byte short of the second match
    CHECK(r.findAll(key, 0, block - 3 + 8).size() == 2);
    CHECK(r.findAll(key, 0, kToEnd, 1, 0).empty());
    CHECK(r.findAll("").empty());
    CHECK(r.findAll("nothere").empty());
    CHECK(r.findAll(std::string(2000, 'x')).empty());
    // alignment: only offsets that are multiples of 8
    const auto aligned = r.findAll(key, 0, kToEnd, 8);
    for (auto o : aligned) CHECK(o % 8 == 0);
    CHECK(std::find(aligned.begin(), aligned.end(), block - 3) == aligned.end());  // block - 3 is odd
    CHECK(std::find(aligned.begin(), aligned.end(), uint64_t{0}) != aligned.end());
    // pattern starting with zero bytes
    std::string zk = std::string("\0\0\0", 3) + "\x09\x09";
    std::memcpy(data.data() + 2 * block + 500, zk.data(), zk.size());
    writeFile(p, data);
    FileReader r2(p.string());
    CHECK(r2.findAll(zk) == std::vector<uint64_t>{2 * block + 500});
    // overlapping matches
    writeFile(p, std::string("aaaaa"));
    FileReader r3(p.string());
    CHECK(r3.findAll("aa").size() == 4);
    // callback abort
    uint64_t calls = 0;
    CHECK(r3.scan("a", 0, kToEnd, 1, [&](uint64_t) { return ++calls < 3; }) == 3);
}

TEST_CASE("file_reader: concurrent reads") {
    testutil::TempDir dir;
    const auto p = dir.path() / "c.bin";
    const std::string data = patternData(1 << 20);
    writeFile(p, data);
    FileReader r(p.string());
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::string buf(4096, '\0');
            for (int i = 0; i < 2000; i++) {
                const size_t off = rng() % (data.size() - 4096);
                if (r.read(off, 4096, buf.data()) != 4096 || std::memcmp(buf.data(), data.data() + off, 4096) != 0) bad++;
                if (i % 500 == 0) r.refresh();
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(bad == 0);
}

TEST_CASE("file_reader: files larger than 4 GiB (sparse)") {
    testutil::TempDir dir;
    const auto p = dir.path() / "big.bin";
    const uint64_t off = (uint64_t{5} << 30) + 12345;
    {
        std::ofstream f(p, std::ios::binary);
        f.seekp(static_cast<std::streamoff>(off));
        f.write("TAIL", 4);
    }
    if (!fs::exists(p) || fs::file_size(p) != off + 4) {
        MESSAGE("file system does not support large sparse files; skipped");
        return;
    }
    FileReader r(p.string());
    CHECK(r.size() == off + 4);
    CHECK(r.readString(off, 8) == "TAIL");
    CHECK(r.isAllZero(uint64_t{4} << 30, 1 << 20));
    CHECK(r.firstNonZero(off - 5000) == std::optional<uint64_t>(off));
    CHECK(r.findAll("TAIL", off - 5000) == std::vector<uint64_t>{off});
    CHECK(r.findAll("TAIL", off + 1).empty());
}

TEST_CASE("file_reader: real state file" * doctest::skip(qstate::testing::stateDir().empty())) {
    const auto p = fs::path(qstate::testing::stateDir()) / "contract0000.229";
    if (!fs::exists(p)) return;
    FileReader r(p.string());
    CHECK(r.size() == 8192);
}
