// KangarooTwelve (KT128, RFC 9861) with an empty customization string, implemented from the public
// specification: Keccak-p[1600,12] sponge (rate 168 bytes), 8192-byte leaves, 32-byte chaining values.
//
// Thread-safety: a K12 object is not thread-safe (use one per thread); all free functions are reentrant.
// Little-endian hosts only. An AVX2 path (4 leaves in parallel) is selected at run time when the CPU has it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qstate::support {

inline constexpr size_t kK12ChunkSize = 8192;     // leaf size of the K12 tree
inline constexpr size_t kK12CvSize = 32;          // chaining value size
inline constexpr size_t kK12DefaultDigestSize = 32;

using K12Digest = std::array<uint8_t, kK12DefaultDigestSize>;

// Incremental hasher. update() may be called with arbitrary slices; finalize() produces `outLen` output bytes
// (any length, including 0) and resets the object, so it can hash the next message right away.
class K12 {
public:
    K12() { reset(); }

    void reset();
    void update(const void* data, size_t length);
    void finalize(uint8_t* out, size_t outLen);
    K12Digest finalize32();

    // Total number of message bytes absorbed so far.
    uint64_t bytesProcessed() const { return total_; }

private:
    void flushFullBuffer();
    void absorbChunks(const uint8_t* data, size_t chunkCount);
    void absorb(const uint8_t* data, size_t length);
    void permute();

    alignas(16) uint64_t state_[25];
    uint32_t pos_ = 0;           // bytes absorbed into the current rate block of the outer sponge
    bool treeStarted_ = false;   // S_0 and the 0x03 padding have been absorbed into the outer sponge
    uint64_t leafCount_ = 0;     // leaf chaining values absorbed (chunks after S_0)
    uint64_t total_ = 0;
    uint32_t bufLen_ = 0;
    std::array<uint8_t, kK12ChunkSize> buf_;
};

// One-shot digest of a memory block: `outLen` bytes of KT128(data, C = "").
void k12(const void* data, size_t length, uint8_t* out, size_t outLen);
K12Digest k12Digest(const void* data, size_t length);

// Qubic's Merkle inner node: K12 of 64 concatenated bytes -> 32 bytes.
K12Digest k12Digest64(const uint8_t left[32], const uint8_t right[32]);

// ----------------------------------------------------------------------------------------------------------------
// Tree primitives (for parallel hashing of huge files and for per-chunk change detection).
//
// The hashed string is S = M || 0x00 (length_encode of the empty customization string). S is split into
// 8192-byte chunks S_0, S_1, ..., S_n. When |S| <= 8192 the digest is a single TurboSHAKE128 call and there are
// no chaining values; otherwise the digest is derived from S_0 and the chaining values CV_1..CV_n.
// ----------------------------------------------------------------------------------------------------------------

// Number n of chaining values (chunks after the first) of a message of `messageLength` bytes; 0 for short messages.
uint64_t k12ChunkCount(uint64_t messageLength);

// Chaining values of `count` consecutive full 8192-byte chunks starting at `data` (count * 32 bytes output).
void k12LeafChainingValues(const uint8_t* data, size_t count, uint8_t* cvs);

// Chaining value of one (possibly short) leaf; `appendZero` appends the 0x00 byte of the length encoding when this
// is the last chunk of S and the message does not already end on a chunk border.
void k12LeafChainingValue(const uint8_t* chunk, size_t length, bool appendZero, uint8_t cv[kK12CvSize]);

// All chaining values CV_1..CV_n of an in-memory message (k12ChunkCount(length) * 32 bytes output).
void k12AllChainingValues(const uint8_t* data, uint64_t length, uint8_t* cvs);

// Digest from S_0 (the first 8192 bytes of the message) and the chaining values; requires n >= 1.
void k12FromChainingValues(const uint8_t* firstChunk, const uint8_t* cvs, uint64_t n, uint8_t* out, size_t outLen);

// True when the AVX2 four-way leaf hashing is in use on this machine.
bool k12UsesAvx2();

} // namespace qstate::support
