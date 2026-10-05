// The portable 128-bit integers (used on MSVC) against the compiler's __int128, where there is one.
#include <doctest/doctest.h>

#include <cstdint>
#include <random>

#include "../src/int128.h"

using namespace qstate::decode::wide;

#if defined(__SIZEOF_INT128__)

namespace {

__extension__ typedef unsigned __int128 RefU;
__extension__ typedef __int128 RefI;

portable::u128 fromRef(RefU v) { return portable::u128(static_cast<std::uint64_t>(v >> 64), static_cast<std::uint64_t>(v), 0); }
bool same(portable::u128 a, RefU b) { return a == fromRef(b); }
RefU toRef(portable::u128 v) { return (static_cast<RefU>(v.hi) << 64) | v.lo; }

std::uint64_t pick(std::mt19937_64& rng) {
    switch (rng() % 6) {
    case 0: return 0;
    case 1: return ~std::uint64_t{0};
    case 2: return rng() & 0xFF;
    case 3: return std::uint64_t{1} << (rng() % 64);
    default: return rng();
    }
}

} // namespace

TEST_CASE("portable u128 matches __int128 on random operands") {
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        const portable::u128 a(pick(rng), pick(rng), 0), b(pick(rng), pick(rng), 0);
        const RefU ra = toRef(a), rb = toRef(b);
        const int sh = static_cast<int>(rng() % 140);
        REQUIRE(same(a + b, ra + rb));
        REQUIRE(same(a - b, ra - rb));
        REQUIRE(same(a * b, ra * rb));
        REQUIRE(same(a | b, ra | rb));
        REQUIRE(same(a & b, ra & rb));
        REQUIRE(same(a ^ b, ra ^ rb));
        REQUIRE(same(~a, ~ra));
        REQUIRE(same(a << sh, sh >= 128 ? RefU{0} : ra << sh));
        REQUIRE(same(a >> sh, sh >= 128 ? RefU{0} : ra >> sh));
        REQUIRE((a < b) == (ra < rb));
        REQUIRE((a > b) == (ra > rb));
        REQUIRE((a <= b) == (ra <= rb));
        REQUIRE((a == b) == (ra == rb));
        if (rb != 0) {
            REQUIRE(same(a / b, ra / rb));
            REQUIRE(same(a % b, ra % rb));
        }
    }
}

TEST_CASE("portable u128: division by small bases, as used for decimal formatting and parsing") {
    std::mt19937_64 rng(777);
    for (int i = 0; i < 2000; ++i) {
        portable::u128 a(pick(rng), pick(rng), 0);
        RefU ra = toRef(a);
        for (unsigned base : {10u, 16u}) {
            REQUIRE(static_cast<int>(a % base) == static_cast<int>(ra % base));
            REQUIRE(same(a / base, ra / base));
        }
    }
}

TEST_CASE("portable i128 and conversions") {
    std::mt19937_64 rng(99);
    for (int i = 0; i < 5000; ++i) {
        const std::int64_t x = static_cast<std::int64_t>(pick(rng)), y = static_cast<std::int64_t>(pick(rng));
        const portable::i128 a(x), b(y);
        const RefI ra = x, rb = y;
        REQUIRE((a < b) == (ra < rb));
        REQUIRE((a == b) == (ra == rb));
        REQUIRE((a > b) == (ra > rb));
        const portable::i128 n = -a;
        const RefI rn = -ra;
        REQUIRE(n.lo == static_cast<std::uint64_t>(rn));
        REQUIRE(n.hi == static_cast<std::int64_t>(rn >> 64));
        REQUIRE(static_cast<std::int64_t>(a) == x);
        REQUIRE(static_cast<std::uint64_t>(a) == static_cast<std::uint64_t>(x));
        const portable::u128 u(a);  // sign-extending conversion, like (unsigned __int128) of a negative __int128
        REQUIRE(same(u, static_cast<RefU>(ra)));
        REQUIRE(portable::i128(u) == a);
    }
    CHECK(portable::i128(portable::u128(1u)) > portable::i128(0));
    CHECK(portable::i128(std::uint64_t{~0ull}) > portable::i128(0));       // unsigned values do not go negative
    CHECK(portable::i128(-1) < portable::i128(0));
    CHECK(portable::i128(true) == portable::i128(1));
}

#endif

TEST_CASE("the selected 128-bit types support the decoder's operations") {
    u128 v = 0;
    for (const char c : std::string("340282366920938463463374607431768211455")) {
        const unsigned d = static_cast<unsigned>(c - '0');
        CHECK_FALSE(v > (~u128{0} - d) / 10u);
        v = v * 10u + d;
    }
    CHECK(v == ~u128{0});
    CHECK((static_cast<u128>(1) << 126) > (static_cast<u128>(1) << 64));
    const i128 n = -static_cast<i128>(static_cast<u128>(5));
    CHECK(n < 0);
    CHECK(static_cast<std::int64_t>(n) == -5);
    CHECK(static_cast<u128>(i128(7)) == static_cast<u128>(7));
    CHECK((static_cast<u128>(std::uint64_t{3}) << 64 | std::uint64_t{4}) > static_cast<u128>(std::uint64_t{~0ull}));
}
