// Standalone KangarooTwelve (KT128, RFC 9861) with EMPTY customization string.
// Reference implementation for qstate-viewer research spike (06-state-files).
// Bit-compatible with Qubic core's KangarooTwelve() (src/kangaroo_twelve.h:1394) and
// XKCP::KangarooTwelve() (src/K12/kangaroo_twelve_xkcp.h:427), which the core unit test
// test/kangaroo_twelve.cpp (CompareK12Implementations) asserts to be identical.
//
//   k12::hash(data, len, out, outLen)            one-shot
//   k12::leafCV(chunk, len, cv32)                chaining value of one 8192-byte leaf chunk (for parallel / incremental use)
//   k12::hash64to32(in64, out32)                 == KangarooTwelve64To32 (Merkle inner node)
//
// Plain C++17, little-endian hosts only (x86-64). No SIMD; ~1 GB/s per core at -O3.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace k12 {

inline constexpr size_t CHUNK = 8192;      // K12 leaf size
inline constexpr size_t RATE = 168;        // TurboSHAKE128 rate in bytes (1600-256)/8
inline constexpr size_t CV = 32;           // chaining value size

inline uint64_t rol(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

// Keccak-p[1600, 12 rounds] = last 12 rounds of Keccak-f[1600]
inline void keccakP1600_12(uint64_t s[25])
{
    static const uint64_t RC[12] = {
        0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
        0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL };
    static const int ROT[25] = { 0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };
    for (int r = 0; r < 12; r++)
    {
        uint64_t c[5], d[5], b[25];
        for (int x = 0; x < 5; x++) c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        for (int x = 0; x < 5; x++) d[x] = c[(x + 4) % 5] ^ rol(c[(x + 1) % 5], 1);
        for (int i = 0; i < 25; i++) s[i] ^= d[i % 5];
        // rho + pi: B[y, 2x+3y] = rot(A[x, y])
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
            {
                int i = x + 5 * y;
                uint64_t v = ROT[i] ? rol(s[i], ROT[i]) : s[i];
                b[y + 5 * ((2 * x + 3 * y) % 5)] = v;
            }
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++)
                s[x + 5 * y] = b[x + 5 * y] ^ (~b[(x + 1) % 5 + 5 * y] & b[(x + 2) % 5 + 5 * y]);
        s[0] ^= RC[r];
    }
}

// TurboSHAKE128 sponge
struct Sponge
{
    uint64_t s[25];
    size_t pos;
    Sponge() { reset(); }
    void reset() { std::memset(s, 0, sizeof(s)); pos = 0; }
    void absorb(const uint8_t* p, size_t n)
    {
        uint8_t* sb = reinterpret_cast<uint8_t*>(s);
        while (n)
        {
            if (pos == 0 && n >= RATE)
            {
                // fast path: whole blocks
                for (size_t i = 0; i < RATE / 8; i++) { uint64_t w; std::memcpy(&w, p + 8 * i, 8); s[i] ^= w; }
                keccakP1600_12(s);
                p += RATE; n -= RATE;
                continue;
            }
            size_t k = RATE - pos; if (k > n) k = n;
            for (size_t i = 0; i < k; i++) sb[pos + i] ^= p[i];
            pos += k; p += k; n -= k;
            if (pos == RATE) { keccakP1600_12(s); pos = 0; }
        }
    }
    // finalize with domain separation byte D, then squeeze outLen bytes (outLen <= RATE supported here)
    void finish(uint8_t D, uint8_t* out, size_t outLen)
    {
        uint8_t* sb = reinterpret_cast<uint8_t*>(s);
        sb[pos] ^= D;
        sb[RATE - 1] ^= 0x80;
        keccakP1600_12(s);
        std::memcpy(out, sb, outLen);
    }
};

// length_encode(x) as used by K12: big-endian x without leading zero bytes, then 1 byte = number of bytes.
inline size_t lengthEncode(uint64_t x, uint8_t* buf)
{
    size_t n = 0;
    for (uint64_t v = x; v; v >>= 8) n++;
    for (size_t i = 0; i < n; i++) buf[i] = uint8_t(x >> (8 * (n - 1 - i)));
    buf[n] = uint8_t(n);
    return n + 1;
}

// CV of one non-first chunk of S (TurboSHAKE128(chunk, 0x0B, 32)).
inline void leafCV(const uint8_t* chunk, size_t len, uint8_t cv[CV])
{
    Sponge q; q.absorb(chunk, len); q.finish(0x0B, cv, CV);
}

// One-shot KT128(M = data[0..len), C = "", outLen <= 168)
inline void hash(const uint8_t* data, uint64_t len, uint8_t* out, size_t outLen = 32)
{
    // S = M || length_encode(0) = M || 0x00 ; |S| = len + 1
    static const uint8_t zero = 0;
    const uint64_t sLen = len + 1;
    Sponge f;
    if (sLen <= CHUNK)
    {
        f.absorb(data, len); f.absorb(&zero, 1);
        f.finish(0x07, out, outLen);
        return;
    }
    // first chunk S_0 = M[0..8192)
    f.absorb(data, CHUNK);
    static const uint8_t pad[8] = { 0x03, 0, 0, 0, 0, 0, 0, 0 };
    f.absorb(pad, 8);
    uint64_t n = 0; // number of leaf chunks after S_0
    uint64_t off = CHUNK;
    while (off < sLen)
    {
        uint64_t l = sLen - off; if (l > CHUNK) l = CHUNK;
        uint8_t cv[CV];
        Sponge q;
        // bytes of S in [off, off+l): all from M except possibly the very last byte (the 0x00 of length_encode(0))
        uint64_t fromM = (off + l <= len) ? l : (len - off);
        q.absorb(data + off, fromM);
        if (fromM < l) q.absorb(&zero, 1);
        q.finish(0x0B, cv, CV);
        f.absorb(cv, CV);
        off += l; n++;
    }
    uint8_t enc[12];
    size_t e = lengthEncode(n, enc);
    enc[e++] = 0xFF; enc[e++] = 0xFF;
    f.absorb(enc, e);
    f.finish(0x06, out, outLen);
}

// Combine cached CVs (for chunks 1..n) into the final digest. firstChunk = M[0..8192). Requires len+1 > 8192.
// cvs must hold n*32 bytes where n = ceil((len + 1 - 8192) / 8192).
inline void hashFromCVs(const uint8_t* firstChunk, const uint8_t* cvs, uint64_t n, uint8_t* out, size_t outLen = 32)
{
    Sponge f;
    f.absorb(firstChunk, CHUNK);
    static const uint8_t pad[8] = { 0x03, 0, 0, 0, 0, 0, 0, 0 };
    f.absorb(pad, 8);
    f.absorb(cvs, n * CV);
    uint8_t enc[12];
    size_t e = lengthEncode(n, enc);
    enc[e++] = 0xFF; enc[e++] = 0xFF;
    f.absorb(enc, e);
    f.finish(0x06, out, outLen);
}

// Merkle inner node used by Qubic for spectrum/universe/computer digest trees: K12(64 bytes) -> 32 bytes.
inline void hash64to32(const uint8_t in[64], uint8_t out[32]) { hash(in, 64, out, 32); }

} // namespace k12
