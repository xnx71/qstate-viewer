#include <doctest/doctest.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "hex_util.h"
#include "qstate/support/k12.h"
#include "test_vectors.h"

using namespace qstate::support;

namespace {

std::vector<uint8_t> ptn(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i % 251);
    return v;
}

std::string hexOf(const uint8_t* p, size_t n) { return testutil::toHex(p, n); }

}  // namespace

TEST_CASE("k12: official KT128 vectors (ptn patterns, empty customization)") {
    for (const auto& v : testvec::kK12Vectors) {
        const auto m = ptn(v.length);
        const auto d = k12Digest(m.data(), m.size());
        CHECK_MESSAGE(hexOf(d.data(), d.size()) == v.hex32, "length " << v.length);
    }
}

TEST_CASE("k12: 10032 byte output, last 32 bytes (official vector for the empty message)") {
    std::vector<uint8_t> out(10032);
    k12(nullptr, 0, out.data(), out.size());
    CHECK(hexOf(out.data() + 10000, 32) == "e8dc563642f7228c84684c898405d3a834799158c079b12880277a1d28e2ff6d");
    // Shorter outputs are prefixes of longer ones.
    uint8_t shortOut[32];
    k12(nullptr, 0, shortOut, 32);
    CHECK(std::memcmp(shortOut, out.data(), 32) == 0);
    CHECK(hexOf(shortOut, 32) == "1ac2d450fc3b4205d19da7bfca1b37513c0803577ac7167f06fe2ce1f0ef39e5");
}

TEST_CASE("k12: zero length output is allowed") {
    uint8_t dummy = 0;
    k12("abc", 3, &dummy, 0);
    K12 h;
    h.update("abc", 3);
    h.finalize(&dummy, 0);
}

TEST_CASE("k12: incremental hashing equals one-shot for random slicing around chunk borders") {
    std::mt19937 rng(12345);
    const size_t lengths[] = {0, 1, 167, 168, 169, 8191, 8192, 8193, 16383, 16384, 16385, 24576, 24577, 40000, 100000};
    for (size_t len : lengths) {
        std::vector<uint8_t> m(len);
        for (auto& b : m) b = static_cast<uint8_t>(rng());
        const auto expected = k12Digest(m.data(), m.size());
        for (int round = 0; round < 6; round++) {
            K12 h;
            size_t off = 0;
            while (off < len) {
                size_t step = 1 + rng() % (round < 3 ? 300 : 20000);
                step = std::min(step, len - off);
                h.update(m.data() + off, step);
                off += step;
            }
            CHECK(h.bytesProcessed() == len);
            CHECK_MESSAGE(h.finalize32() == expected, "length " << len << " round " << round);
        }
    }
}

TEST_CASE("k12: hasher is reusable after finalize") {
    K12 h;
    h.update("hello", 5);
    const auto a = h.finalize32();
    h.update("hello", 5);
    CHECK(h.finalize32() == a);
    CHECK(a == k12Digest("hello", 5));
}

TEST_CASE("k12: tree primitives agree with the full digest") {
    for (size_t len : {size_t{8192}, size_t{8193}, size_t{16384}, size_t{16385}, size_t{50000}, size_t{8192 * 9 + 5}}) {
        const auto m = ptn(len);
        const uint64_t n = k12ChunkCount(len);
        REQUIRE(n >= 1);
        std::vector<uint8_t> cvs(n * kK12CvSize);
        k12AllChainingValues(m.data(), len, cvs.data());
        uint8_t out[32];
        k12FromChainingValues(m.data(), cvs.data(), n, out, 32);
        CHECK_MESSAGE(hexOf(out, 32) == hexOf(k12Digest(m.data(), len).data(), 32), "length " << len);
    }
    CHECK(k12ChunkCount(0) == 0);
    CHECK(k12ChunkCount(8191) == 0);
    CHECK(k12ChunkCount(8192) == 1);
    CHECK(k12ChunkCount(8193) == 1);
    CHECK(k12ChunkCount(16383) == 1);
    CHECK(k12ChunkCount(16384) == 2);
}

TEST_CASE("k12: multi leaf hashing (SIMD path when available) equals the scalar leaf function") {
    std::mt19937 rng(7);
    std::vector<uint8_t> m(11 * kK12ChunkSize);
    for (auto& b : m) b = static_cast<uint8_t>(rng());
    std::vector<uint8_t> cvs(11 * kK12CvSize);
    k12LeafChainingValues(m.data(), 11, cvs.data());
    for (size_t i = 0; i < 11; i++) {
        uint8_t cv[32];
        k12LeafChainingValue(m.data() + i * kK12ChunkSize, kK12ChunkSize, false, cv);
        CHECK_MESSAGE(std::memcmp(cv, cvs.data() + i * 32, 32) == 0, "leaf " << i);
    }
    MESSAGE("AVX2 in use: " << k12UsesAvx2());
}

TEST_CASE("k12: Merkle inner node helper") {
    uint8_t a[32], b[32], both[64];
    for (int i = 0; i < 32; i++) {
        a[i] = static_cast<uint8_t>(i);
        b[i] = static_cast<uint8_t>(100 + i);
    }
    std::memcpy(both, a, 32);
    std::memcpy(both + 32, b, 32);
    CHECK(k12Digest64(a, b) == k12Digest(both, 64));
    CHECK(hexOf(k12Digest(ptn(64).data(), 64).data(), 32) ==
          "21a9962295fb748cf50dc975a868bb7178d8f1067112a2f377bc9a4f272971fb");
}
