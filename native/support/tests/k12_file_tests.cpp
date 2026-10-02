#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include "qstate/support/identity.h"
#include "qstate/support/k12_file.h"
#include "temp_dir.h"
#include "test_env.h"

using namespace qstate::support;
namespace fs = std::filesystem;

namespace {

void writeBytes(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// name -> hex digest, from docs/research/k12-spike/digest_229.txt
std::map<std::string, std::string> loadReferenceDigests() {
    std::map<std::string, std::string> result;
    const std::string root = qstate::testing::sourceDir();
    if (root.empty()) return result;
    std::ifstream in(root + "/docs/research/k12-spike/digest_229.txt");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string name, size, unit, hash;
        if (ls >> name >> size >> unit >> hash && name.rfind("contract", 0) == 0 && hash.size() == 64) result[name] = hash;
    }
    return result;
}

}  // namespace

TEST_CASE("k12_file: file digest equals the in-memory digest for all interesting sizes and thread counts") {
    testutil::TempDir dir;
    std::mt19937 rng(5);
    const size_t sizes[] = {0, 1, 8191, 8192, 8193, 16383, 16384, 16385, 2 * 1024 * 1024 + 8192,
                            2 * 1024 * 1024 + 8193, 5 * 1024 * 1024 + 777, 9 * 1024 * 1024};
    for (size_t size : sizes) {
        std::vector<uint8_t> data(size);
        for (auto& b : data) b = static_cast<uint8_t>(rng());
        const auto p = dir.path() / "f.bin";
        writeBytes(p, data);
        FileReader r(p.string());
        const auto expected = k12Digest(data.data(), data.size());
        for (unsigned threads : {1u, 3u, 0u}) {
            K12FileOptions opt;
            opt.threads = threads;
            const auto d = k12DigestFile(r, opt);
            REQUIRE(d.has_value());
            CHECK_MESSAGE(*d == expected, "size " << size << " threads " << threads);
        }
    }
}

TEST_CASE("k12_file: progress, cancellation and truncation") {
    testutil::TempDir dir;
    std::vector<uint8_t> data(12 * 1024 * 1024, 7);
    const auto p = dir.path() / "f.bin";
    writeBytes(p, data);
    FileReader r(p.string());

    uint64_t last = 0, total = 0;
    K12FileOptions opt;
    opt.threads = 2;
    opt.progress = [&](uint64_t done, uint64_t t) {
        last = std::max(last, done);
        total = t;
        return true;
    };
    REQUIRE(k12DigestFile(r, opt).has_value());
    CHECK(last == data.size());
    CHECK(total == data.size());

    int calls = 0;
    opt.progress = [&](uint64_t, uint64_t) { return ++calls < 2; };
    CHECK_FALSE(k12DigestFile(r, opt).has_value());

    fs::resize_file(p, 6 * 1024 * 1024);  // truncated after the reader recorded the size
    CHECK_THROWS_AS(k12DigestFile(r), FileError);
    CHECK(r.refresh() == RefreshResult::Changed);
    CHECK(k12DigestFile(r).has_value());
}

TEST_CASE("k12_file: epoch 229 state files match the core computed digests" * doctest::skip(qstate::testing::stateDir().empty())) {
    const auto reference = loadReferenceDigests();
    if (reference.empty()) {
        MESSAGE("reference digests not available (QSTATE_SOURCE_DIR)");
        return;
    }
    const bool slow = qstate::testing::envOr("QSTATE_TEST_SLOW") == "1";
    const uint64_t smallLimit = 64ull * 1024 * 1024;
    int checked = 0;
    for (const auto& [name, hex] : reference) {
        const auto p = fs::path(qstate::testing::stateDir()) / name;
        if (!fs::exists(p)) continue;
        const auto size = fs::file_size(p);
        if (size > smallLimit && !slow) continue;
        FileReader r(p.string());
        const auto t0 = std::chrono::steady_clock::now();
        const auto d = k12DigestFile(r);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(d.has_value());
        CHECK_MESSAGE(toHex(*d) == hex, name);
        if (size > smallLimit) MESSAGE(name << ": " << size / 1e6 / s << " MB/s");
        checked++;
    }
    CHECK(checked > 0);
}
