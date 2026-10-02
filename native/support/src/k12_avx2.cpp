// AVX2 four-way Keccak-p[1600,12]: hashes four 8192-byte leaves in lock step. Compiled with -mavx2 only.
#include "k12_internal.h"

#if defined(__AVX2__)

#include <immintrin.h>

#include "keccak_impl.h"

namespace qstate::support::detail {

struct V4 {
    __m256i v;
    V4() = default;
    explicit V4(__m256i x) : v(x) {}
    explicit V4(uint64_t x) : v(_mm256_set1_epi64x(static_cast<long long>(x))) {}
};

inline V4 operator^(V4 a, V4 b) { return V4(_mm256_xor_si256(a.v, b.v)); }

template <int R>
inline V4 rotl(V4 x) {
    if constexpr (R == 8) {
        return V4(_mm256_shuffle_epi8(
            x.v, _mm256_set_epi8(14, 13, 12, 11, 10, 9, 8, 15, 6, 5, 4, 3, 2, 1, 0, 7, 14, 13, 12, 11, 10, 9, 8, 15,
                                 6, 5, 4, 3, 2, 1, 0, 7)));
    } else {
        return V4(_mm256_or_si256(_mm256_slli_epi64(x.v, R), _mm256_srli_epi64(x.v, 64 - R)));
    }
}
inline V4 rotl1(V4 x) { return rotl<1>(x); }
inline V4 andnot(V4 a, V4 b) { return V4(_mm256_andnot_si256(a.v, b.v)); }

namespace {

// Transposes four 4 x uint64 vectors in place (rows <-> columns).
inline void transpose4(__m256i& r0, __m256i& r1, __m256i& r2, __m256i& r3) {
    const __m256i t0 = _mm256_unpacklo_epi64(r0, r1);
    const __m256i t1 = _mm256_unpackhi_epi64(r0, r1);
    const __m256i t2 = _mm256_unpacklo_epi64(r2, r3);
    const __m256i t3 = _mm256_unpackhi_epi64(r2, r3);
    r0 = _mm256_permute2x128_si256(t0, t2, 0x20);
    r1 = _mm256_permute2x128_si256(t1, t3, 0x20);
    r2 = _mm256_permute2x128_si256(t0, t2, 0x31);
    r3 = _mm256_permute2x128_si256(t1, t3, 0x31);
}

inline uint64_t loadWord(const uint8_t* p) {
    uint64_t w;
    __builtin_memcpy(&w, p, 8);
    return w;
}

constexpr size_t kRate = 168;
constexpr size_t kChunk = 8192;

// XORs `words` (<= 21) 64-bit words from each of the four lanes into s[0..words).
inline void absorbWords(V4* s, const uint8_t* l0, const uint8_t* l1, const uint8_t* l2, const uint8_t* l3,
                        size_t words) {
    size_t w = 0;
    for (; w + 4 <= words; w += 4) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l0 + 8 * w));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l1 + 8 * w));
        __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l2 + 8 * w));
        __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l3 + 8 * w));
        transpose4(a, b, c, d);
        s[w] = s[w] ^ V4(a);
        s[w + 1] = s[w + 1] ^ V4(b);
        s[w + 2] = s[w + 2] ^ V4(c);
        s[w + 3] = s[w + 3] ^ V4(d);
    }
    for (; w < words; w++) {
        const __m256i x = _mm256_set_epi64x(static_cast<long long>(loadWord(l3 + 8 * w)),
                                            static_cast<long long>(loadWord(l2 + 8 * w)),
                                            static_cast<long long>(loadWord(l1 + 8 * w)),
                                            static_cast<long long>(loadWord(l0 + 8 * w)));
        s[w] = s[w] ^ V4(x);
    }
}

}  // namespace

bool avx2Compiled() { return true; }

void k12LeafChainingValues4Avx2(const uint8_t* data, uint8_t* cvs) {
    V4 s[25];
    for (auto& lane : s) lane = V4(_mm256_setzero_si256());
    const uint8_t* l0 = data;
    const uint8_t* l1 = data + kChunk;
    const uint8_t* l2 = data + 2 * kChunk;
    const uint8_t* l3 = data + 3 * kChunk;
    size_t off = 0;
    for (; off + kRate <= kChunk; off += kRate) {
        absorbWords(s, l0 + off, l1 + off, l2 + off, l3 + off, kRate / 8);
        keccakP12<V4>(s);
    }
    // Remaining 128 bytes (kChunk = 48 * 168 + 128), then leaf domain byte 0x0B and the pad10*1 end bit.
    absorbWords(s, l0 + off, l1 + off, l2 + off, l3 + off, (kChunk - off) / 8);
    s[(kChunk - off) / 8] = s[(kChunk - off) / 8] ^ V4(uint64_t{0x0B});
    s[kRate / 8 - 1] = s[kRate / 8 - 1] ^ V4(uint64_t{0x80} << 56);
    keccakP12<V4>(s);
    // Lanes 0..3 of every sponge are its chaining value: transpose so that lane i holds CV_i.
    __m256i r0 = s[0].v, r1 = s[1].v, r2 = s[2].v, r3 = s[3].v;
    transpose4(r0, r1, r2, r3);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(cvs), r0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(cvs + 32), r1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(cvs + 64), r2);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(cvs + 96), r3);
}

}  // namespace qstate::support::detail

#else

namespace qstate::support::detail {
bool avx2Compiled() { return false; }
void k12LeafChainingValues4Avx2(const uint8_t*, uint8_t*) {}
}  // namespace qstate::support::detail

#endif
