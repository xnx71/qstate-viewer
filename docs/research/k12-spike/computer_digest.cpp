// Spike: per-contract state digests + "computer digest" (Merkle root) for a directory of Qubic epoch files.
// Mirrors getComputerDigest() in core src/qubic.cpp:819-887:
//   leaf[i]  = K12(contract state i, stateSize bytes) -> 32 bytes   for i < contractCount, 32 zero bytes otherwise
//   node     = K12(left || right) (KangarooTwelve64To32)
//   root     = contractStateDigests[2 * 1024 - 2]   (tree over MAX_NUMBER_OF_CONTRACTS = 1024 leaves, depth 10)
//
// Usage: computer_digest <dir> <epoch-extension, e.g. 229 or 000> [threads]
// Build: g++ -std=c++17 -O3 -pthread -o computer_digest computer_digest.cpp
#include "k12.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static const unsigned MAX_NUMBER_OF_CONTRACTS = 1024;

static std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// getIdentity(digest, chars, /*isLowerCase=*/true) from core src/four_q.h:1777 - the form printed in node logs ("Computer digest = ...")
static std::string identityLower(const uint8_t d[32])
{
    std::string s(60, 'a');
    for (int i = 0; i < 4; i++)
    {
        uint64_t f; std::memcpy(&f, d + 8 * i, 8);
        for (int j = 0; j < 14; j++) { s[i * 14 + j] = char('a' + f % 26); f /= 26; }
    }
    uint8_t c[4] = { 0, 0, 0, 0 };
    k12::hash(d, 32, c, 3);
    uint32_t cs; std::memcpy(&cs, c, 4); cs &= 0x3FFFF;
    for (int i = 0; i < 4; i++) { s[56 + i] = char('a' + cs % 26); cs /= 26; }
    return s;
}

// Multi-threaded K12 of a memory range (leaf chunks are independent).
static void k12Parallel(const uint8_t* data, uint64_t len, uint8_t out[32], unsigned threads)
{
    const uint64_t sLen = len + 1;
    if (sLen <= k12::CHUNK || threads <= 1 || len < (1u << 22)) { k12::hash(data, len, out, 32); return; }
    const uint64_t n = (sLen - k12::CHUNK + k12::CHUNK - 1) / k12::CHUNK; // leaf chunks after S_0
    std::vector<uint8_t> cvs(n * k12::CV);
    auto work = [&](uint64_t from, uint64_t to) {
        static const uint8_t zero = 0;
        for (uint64_t k = from; k < to; k++)
        {
            const uint64_t off = k12::CHUNK * (k + 1);
            uint64_t l = sLen - off; if (l > k12::CHUNK) l = k12::CHUNK;
            const uint64_t fromM = (off + l <= len) ? l : (len - off);
            k12::Sponge q;
            q.absorb(data + off, fromM);
            if (fromM < l) q.absorb(&zero, 1); // trailing 0x00 = length_encode(|C| = 0)
            q.finish(0x0B, cvs.data() + k * k12::CV, k12::CV);
        }
    };
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; t++) ts.emplace_back(work, n * t / threads, n * (t + 1) / threads);
    for (auto& t : ts) t.join();
    k12::hashFromCVs(data, cvs.data(), n, out, 32);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <dir> <ext> [threads]\n", argv[0]); return 2; }
    const std::string dir = argv[1], ext = argv[2];
    const unsigned threads = argc > 3 ? std::atoi(argv[3]) : std::thread::hardware_concurrency();
    std::vector<uint8_t> digests((MAX_NUMBER_OF_CONTRACTS * 2 - 1) * 32, 0);
    unsigned count = 0; uint64_t total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < MAX_NUMBER_OF_CONTRACTS; i++)
    {
        char name[64]; std::snprintf(name, sizeof(name), "contract%04u.%s", i, ext.c_str());
        const std::string path = dir + "/" + name;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) break; // contracts are contiguous 0..contractCount-1
        struct stat st; fstat(fd, &st);
        const uint8_t* p = st.st_size ? (const uint8_t*)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        if (st.st_size && p == MAP_FAILED) { perror("mmap"); return 1; }
        if (p) madvise((void*)p, st.st_size, MADV_SEQUENTIAL);
        auto a = std::chrono::steady_clock::now();
        k12Parallel(p, st.st_size, &digests[i * 32], threads);
        auto b = std::chrono::steady_clock::now();
        std::printf("%s  %12lld bytes  %s  (%lld ms)\n", name, (long long)st.st_size, hex(&digests[i * 32], 32).c_str(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
        if (p) munmap((void*)p, st.st_size);
        close(fd);
        count++; total += st.st_size;
    }
    // Merkle tree exactly as in getComputerDigest(): levels of 1024, 512, ..., 1 stored consecutively
    unsigned prev = 0, idx = MAX_NUMBER_OF_CONTRACTS, leafs = MAX_NUMBER_OF_CONTRACTS;
    while (leafs > 1)
    {
        for (unsigned i = 0; i < leafs; i += 2) { k12::hash64to32(&digests[(prev + i) * 32], &digests[idx * 32]); idx++; }
        prev += leafs; leafs >>= 1;
    }
    const uint8_t* root = &digests[(MAX_NUMBER_OF_CONTRACTS * 2 - 2) * 32];
    auto t1 = std::chrono::steady_clock::now();
    std::printf("contracts: %u, bytes: %llu, time: %lld ms, threads: %u\n", count, (unsigned long long)total,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(), threads);
    std::printf("computer digest (hex)      = %s\n", hex(root, 32).c_str());
    std::printf("computer digest (identity) = %s\n", identityLower(root).c_str());
    return 0;
}
