#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "real_helpers.h"

#if defined(QSTATE_DECODE_HAS_SUPPORT) && QSTATE_DECODE_HAS_SUPPORT

using namespace qstate::decode;
using namespace qstate::decode::testing;

TEST_CASE("SupportIdentityCodec reproduces the identity vectors of the research data") {
    const std::string dir = qstate::testing::sourceDir();
    if (dir.empty()) {
        MESSAGE("skipped");
        return;
    }
    std::ifstream in(dir + "/docs/research/data/identity_vectors.txt");
    REQUIRE(in);
    SupportIdentityCodec codec;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string hex, ident;
        ls >> hex >> ident;
        if (hex.size() != 64 || ident.size() != 60) continue; // other sections of the file (digest vectors)
        std::uint8_t key[32];
        for (int i = 0; i < 32; ++i) key[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(static_cast<std::size_t>(2 * i), 2), nullptr, 16));
        CHECK(codec.encode(key) == ident);
        std::uint8_t back[32];
        std::string err;
        CHECK(codec.decode(ident, back, &err));
        CHECK(std::memcmp(key, back, 32) == 0);
        ++n;
    }
    CHECK(n >= 10);
    std::uint8_t k[32];
    std::string err;
    CHECK_FALSE(codec.decode(std::string(60, 'A'), k, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("FileReaderByteSource matches the pread source on a real file") {
    const std::string dir = qstate::testing::stateDir();
    if (dir.empty()) {
        MESSAGE("skipped");
        return;
    }
    const std::string path = dir + "/contract0000.229";
    PreadSource ref(path);
    REQUIRE(ref.ok());
    FileReaderByteSource fr(std::make_shared<qstate::support::FileReader>(path));
    CHECK(fr.size() == ref.size());
    std::vector<std::uint8_t> a(1000), b(1000);
    CHECK(fr.read(100, a.size(), a.data()) == ref.read(100, b.size(), b.data()));
    CHECK(a == b);
    CHECK(fr.read(fr.size() - 10, 100, a.data()) == 10);
    CHECK(fr.read(fr.size() + 5, 10, a.data()) == 0);
    CHECK(fr.isAllZero(8000, 100) == ref.isAllZero(8000, 100));
}

#endif
