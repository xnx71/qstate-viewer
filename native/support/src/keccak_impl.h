// Internal: Keccak-p[1600, 12 rounds] written once as a template over the lane type (uint64_t for the scalar
// permutation, a 4 x uint64 vector for the AVX2 four-way permutation). Not part of the public API.
#pragma once

#include <cstdint>

namespace qstate::support::detail {

inline constexpr uint64_t kKeccakRoundConstants12[12] = {
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

template <int R>
inline uint64_t rotl(uint64_t x) {
    return (x << R) | (x >> (64 - R));
}
inline uint64_t rotl1(uint64_t x) { return rotl<1>(x); }
inline uint64_t andnot(uint64_t a, uint64_t b) { return ~a & b; }

// One round on the 25 lanes a[x + 5 * y].
template <class V>
inline void keccakRound(V* a, V rc) {
    V c0 = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
    V c1 = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
    V c2 = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
    V c3 = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
    V c4 = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];
    const V d0 = c4 ^ rotl1(c1);
    const V d1 = c0 ^ rotl1(c2);
    const V d2 = c1 ^ rotl1(c3);
    const V d3 = c2 ^ rotl1(c4);
    const V d4 = c3 ^ rotl1(c0);
    const V b0 = a[0] ^ d0;
    const V b1 = rotl<44>(a[6] ^ d1);
    const V b2 = rotl<43>(a[12] ^ d2);
    const V b3 = rotl<21>(a[18] ^ d3);
    const V b4 = rotl<14>(a[24] ^ d4);
    const V b5 = rotl<28>(a[3] ^ d3);
    const V b6 = rotl<20>(a[9] ^ d4);
    const V b7 = rotl<3>(a[10] ^ d0);
    const V b8 = rotl<45>(a[16] ^ d1);
    const V b9 = rotl<61>(a[22] ^ d2);
    const V b10 = rotl<1>(a[1] ^ d1);
    const V b11 = rotl<6>(a[7] ^ d2);
    const V b12 = rotl<25>(a[13] ^ d3);
    const V b13 = rotl<8>(a[19] ^ d4);
    const V b14 = rotl<18>(a[20] ^ d0);
    const V b15 = rotl<27>(a[4] ^ d4);
    const V b16 = rotl<36>(a[5] ^ d0);
    const V b17 = rotl<10>(a[11] ^ d1);
    const V b18 = rotl<15>(a[17] ^ d2);
    const V b19 = rotl<56>(a[23] ^ d3);
    const V b20 = rotl<62>(a[2] ^ d2);
    const V b21 = rotl<55>(a[8] ^ d3);
    const V b22 = rotl<39>(a[14] ^ d4);
    const V b23 = rotl<41>(a[15] ^ d0);
    const V b24 = rotl<2>(a[21] ^ d1);
    a[0] = b0 ^ andnot(b1, b2);
    a[1] = b1 ^ andnot(b2, b3);
    a[2] = b2 ^ andnot(b3, b4);
    a[3] = b3 ^ andnot(b4, b0);
    a[4] = b4 ^ andnot(b0, b1);
    a[5] = b5 ^ andnot(b6, b7);
    a[6] = b6 ^ andnot(b7, b8);
    a[7] = b7 ^ andnot(b8, b9);
    a[8] = b8 ^ andnot(b9, b5);
    a[9] = b9 ^ andnot(b5, b6);
    a[10] = b10 ^ andnot(b11, b12);
    a[11] = b11 ^ andnot(b12, b13);
    a[12] = b12 ^ andnot(b13, b14);
    a[13] = b13 ^ andnot(b14, b10);
    a[14] = b14 ^ andnot(b10, b11);
    a[15] = b15 ^ andnot(b16, b17);
    a[16] = b16 ^ andnot(b17, b18);
    a[17] = b17 ^ andnot(b18, b19);
    a[18] = b18 ^ andnot(b19, b15);
    a[19] = b19 ^ andnot(b15, b16);
    a[20] = b20 ^ andnot(b21, b22);
    a[21] = b21 ^ andnot(b22, b23);
    a[22] = b22 ^ andnot(b23, b24);
    a[23] = b23 ^ andnot(b24, b20);
    a[24] = b24 ^ andnot(b20, b21);
    a[0] = a[0] ^ rc;
}

template <class V>
inline void keccakP12(V* a) {
    for (int r = 0; r < 12; r++) {
        keccakRound<V>(a, V(kKeccakRoundConstants12[r]));
    }
}

} // namespace qstate::support::detail
