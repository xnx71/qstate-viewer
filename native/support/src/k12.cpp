#include "qstate/support/k12.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "k12_internal.h"
#include "keccak_impl.h"

static_assert(std::endian::native == std::endian::little, "K12 implementation assumes a little-endian host");

namespace qstate::support {

namespace {

constexpr size_t kRate = 168;  // (1600 - 256) / 8

bool detectAvx2() {
#if defined(__x86_64__) || defined(__i386__)
#if defined(__GNUC__) || defined(__clang__)
    return detail::avx2Compiled() && __builtin_cpu_supports("avx2");
#else
    return false;
#endif
#else
    return false;
#endif
}

bool useAvx2() {
    static const bool value = detectAvx2();
    return value;
}

// Streaming TurboSHAKE128 sponge over the 25 state lanes.
struct Sponge {
    alignas(16) uint64_t s[25];
    size_t pos = 0;

    Sponge() { std::memset(s, 0, sizeof(s)); }

    uint8_t* bytes() { return reinterpret_cast<uint8_t*>(s); }

    void absorb(const uint8_t* p, size_t n) {
        while (n > 0) {
            if (pos == 0) {
                while (n >= kRate) {
                    for (size_t i = 0; i < kRate / 8; i++) {
                        uint64_t w;
                        std::memcpy(&w, p + 8 * i, 8);
                        s[i] ^= w;
                    }
                    detail::keccakP12<uint64_t>(s);
                    p += kRate;
                    n -= kRate;
                }
                if (n == 0) return;
            }
            const size_t k = std::min(kRate - pos, n);
            uint8_t* b = bytes() + pos;
            for (size_t i = 0; i < k; i++) b[i] ^= p[i];
            pos += k;
            p += k;
            n -= k;
            if (pos == kRate) {
                detail::keccakP12<uint64_t>(s);
                pos = 0;
            }
        }
    }

    void absorbByte(uint8_t b) { absorb(&b, 1); }

    // Domain separation byte + pad10*1, then squeeze.
    void finish(uint8_t domain, uint8_t* out, size_t outLen) {
        bytes()[pos] ^= domain;
        bytes()[kRate - 1] ^= 0x80;
        detail::keccakP12<uint64_t>(s);
        while (true) {
            const size_t k = std::min(kRate, outLen);
            std::memcpy(out, bytes(), k);
            out += k;
            outLen -= k;
            if (outLen == 0) break;
            detail::keccakP12<uint64_t>(s);
        }
        pos = 0;
    }
};

// Chaining value of leaf data: `m` bytes (+ optional 0x00).
void leafCv(const uint8_t* m, size_t mLen, bool appendZero, uint8_t* cv) {
    Sponge sp;
    sp.absorb(m, mLen);
    if (appendZero) sp.absorbByte(0x00);
    sp.finish(0x0B, cv, kK12CvSize);
}

// length_encode(x): big-endian x without leading zeros followed by the number of bytes.
size_t lengthEncode(uint64_t x, uint8_t* out) {
    uint8_t tmp[8];
    size_t n = 0;
    while (x != 0) {
        tmp[n++] = static_cast<uint8_t>(x & 0xFF);
        x >>= 8;
    }
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = static_cast<uint8_t>(n);
    return n + 1;
}

constexpr uint8_t kTreePad[8] = {0x03, 0, 0, 0, 0, 0, 0, 0};

void finishTree(Sponge& outer, uint64_t n, uint8_t* out, size_t outLen) {
    uint8_t enc[12];
    size_t e = lengthEncode(n, enc);
    enc[e++] = 0xFF;
    enc[e++] = 0xFF;
    outer.absorb(enc, e);
    outer.finish(0x06, out, outLen);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Tree primitives

uint64_t k12ChunkCount(uint64_t messageLength) {
    const uint64_t sLen = messageLength + 1;
    if (sLen <= kK12ChunkSize) return 0;
    return (sLen - kK12ChunkSize + kK12ChunkSize - 1) / kK12ChunkSize;
}

void k12LeafChainingValue(const uint8_t* chunk, size_t length, bool appendZero, uint8_t cv[kK12CvSize]) {
    leafCv(chunk, length, appendZero, cv);
}

void k12LeafChainingValues(const uint8_t* data, size_t count, uint8_t* cvs) {
    size_t i = 0;
    if (useAvx2()) {
        for (; i + 4 <= count; i += 4) {
            detail::k12LeafChainingValues4Avx2(data + i * kK12ChunkSize, cvs + i * kK12CvSize);
        }
    }
    for (; i < count; i++) {
        leafCv(data + i * kK12ChunkSize, kK12ChunkSize, false, cvs + i * kK12CvSize);
    }
}

void k12AllChainingValues(const uint8_t* data, uint64_t length, uint8_t* cvs) {
    const uint64_t n = k12ChunkCount(length);
    if (n == 0) return;
    // Chunks 1..n of S. Chunk j covers S[j*8192, min((j+1)*8192, length+1)).
    const uint64_t fullChunks = (length - kK12ChunkSize) / kK12ChunkSize;  // chunks entirely inside M after S_0
    k12LeafChainingValues(data + kK12ChunkSize, static_cast<size_t>(fullChunks), cvs);
    uint64_t consumed = kK12ChunkSize + fullChunks * kK12ChunkSize;  // bytes of M handled
    for (uint64_t j = fullChunks; j < n; j++) {
        const size_t rest = static_cast<size_t>(length - consumed);  // < 8192 once all full chunks are done
        const size_t take = std::min<size_t>(rest, kK12ChunkSize);
        // The final chunk carries the trailing 0x00; it is a separate chunk when rest == 8192.
        const bool zero = (take < kK12ChunkSize);
        leafCv(data + consumed, take, zero, cvs + j * kK12CvSize);
        consumed += take;
    }
}

void k12FromChainingValues(const uint8_t* firstChunk, const uint8_t* cvs, uint64_t n, uint8_t* out, size_t outLen) {
    Sponge outer;
    outer.absorb(firstChunk, kK12ChunkSize);
    outer.absorb(kTreePad, sizeof(kTreePad));
    outer.absorb(cvs, static_cast<size_t>(n) * kK12CvSize);
    finishTree(outer, n, out, outLen);
}

bool k12UsesAvx2() { return useAvx2(); }

// ---------------------------------------------------------------------------------------------------------------
// Incremental hasher. The outer sponge (state_/pos_) either hashes the whole short message directly or, once the
// message is known to exceed one chunk, absorbs S_0 || 03 00^7 || CV_1 .. CV_n || length_encode(n) || FF FF.

void K12::reset() {
    std::memset(state_, 0, sizeof(state_));
    pos_ = 0;
    treeStarted_ = false;
    leafCount_ = 0;
    total_ = 0;
    bufLen_ = 0;
}

void K12::absorb(const uint8_t* data, size_t length) {
    // Reuse Sponge's logic on the member state.
    Sponge sp;
    std::memcpy(sp.s, state_, sizeof(state_));
    sp.pos = pos_;
    sp.absorb(data, length);
    std::memcpy(state_, sp.s, sizeof(state_));
    pos_ = static_cast<uint32_t>(sp.pos);
}

void K12::absorbChunks(const uint8_t* data, size_t chunkCount) {
    if (chunkCount == 0) return;
    if (!treeStarted_) {
        absorb(data, kK12ChunkSize);
        absorb(kTreePad, sizeof(kTreePad));
        treeStarted_ = true;
        data += kK12ChunkSize;
        chunkCount--;
    }
    constexpr size_t kBatch = 64;
    uint8_t cvs[kBatch * kK12CvSize];
    while (chunkCount > 0) {
        const size_t k = std::min(chunkCount, kBatch);
        k12LeafChainingValues(data, k, cvs);
        absorb(cvs, k * kK12CvSize);
        leafCount_ += k;
        data += k * kK12ChunkSize;
        chunkCount -= k;
    }
}

void K12::flushFullBuffer() {
    absorbChunks(buf_.data(), 1);
    bufLen_ = 0;
}

void K12::update(const void* data, size_t length) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += length;
    while (length > 0) {
        if (bufLen_ == kK12ChunkSize) flushFullBuffer();  // more data follows, so this chunk is not the last one
        if (bufLen_ == 0 && length > kK12ChunkSize) {
            // Hash whole chunks straight from the caller's memory, keeping at least one byte back.
            const size_t chunks = (length - 1) / kK12ChunkSize;
            absorbChunks(p, chunks);
            p += chunks * kK12ChunkSize;
            length -= chunks * kK12ChunkSize;
            continue;
        }
        const size_t k = std::min<size_t>(length, kK12ChunkSize - bufLen_);
        std::memcpy(buf_.data() + bufLen_, p, k);
        bufLen_ += static_cast<uint32_t>(k);
        p += k;
        length -= k;
    }
}

void K12::finalize(uint8_t* out, size_t outLen) {
    Sponge outer;
    std::memcpy(outer.s, state_, sizeof(state_));
    outer.pos = pos_;

    if (!treeStarted_ && bufLen_ < kK12ChunkSize) {
        // |S| <= 8192: single TurboSHAKE128 call over M || 0x00 with domain byte 0x07.
        outer.absorb(buf_.data(), bufLen_);
        outer.absorbByte(0x00);
        outer.finish(0x07, out, outLen);
    } else {
        if (!treeStarted_) {
            // Exactly 8192 message bytes: S_0 = M, and the 0x00 byte forms a one byte chunk S_1.
            outer.absorb(buf_.data(), kK12ChunkSize);
            outer.absorb(kTreePad, sizeof(kTreePad));
            uint8_t cv[kK12CvSize];
            leafCv(nullptr, 0, true, cv);
            outer.absorb(cv, kK12CvSize);
            finishTree(outer, 1, out, outLen);
        } else {
            uint64_t n = leafCount_;
            uint8_t cv[kK12CvSize];
            if (bufLen_ == kK12ChunkSize) {
                leafCv(buf_.data(), kK12ChunkSize, false, cv);
                outer.absorb(cv, kK12CvSize);
                n++;
                leafCv(nullptr, 0, true, cv);
            } else {
                leafCv(buf_.data(), bufLen_, true, cv);
            }
            outer.absorb(cv, kK12CvSize);
            n++;
            finishTree(outer, n, out, outLen);
        }
    }
    reset();
}

K12Digest K12::finalize32() {
    K12Digest d;
    finalize(d.data(), d.size());
    return d;
}

void k12(const void* data, size_t length, uint8_t* out, size_t outLen) {
    if (length < kK12ChunkSize) {
        Sponge sp;
        sp.absorb(static_cast<const uint8_t*>(data), length);
        sp.absorbByte(0x00);
        sp.finish(0x07, out, outLen);
        return;
    }
    K12 h;
    h.update(data, length);
    h.finalize(out, outLen);
}

K12Digest k12Digest(const void* data, size_t length) {
    K12Digest d;
    k12(data, length, d.data(), d.size());
    return d;
}

K12Digest k12Digest64(const uint8_t left[32], const uint8_t right[32]) {
    uint8_t in[64];
    std::memcpy(in, left, 32);
    std::memcpy(in + 32, right, 32);
    return k12Digest(in, sizeof(in));
}

}  // namespace qstate::support
