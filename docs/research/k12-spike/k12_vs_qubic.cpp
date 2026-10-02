// Compares k12.hpp against Qubic core's own KangarooTwelve() / KangarooTwelve64To32() compiled from the core source tree.
// Build: g++ -std=c++20 -O2 -mavx2 -DNO_UEFI -I<core> -I<core>/src k12_vs_qubic.cpp
#define NO_UEFI
#include "platform/memory.h"
#include "kangaroo_twelve.h"
#include "k12.hpp"
#include <cstdio>
#include <vector>
#include <random>

// platform/memory.h only declares these (defined in lib/platform_efi or lib/platform_os in the real build)
void setMem(void* buffer, unsigned long long size, unsigned char value) { std::memset(buffer, value, size); }
void copyMem(void* destination, const void* source, unsigned long long length) { std::memcpy(destination, source, length); }

int main()
{
    std::mt19937_64 rng(12345);
    const size_t sizes[] = { 0, 1, 31, 32, 64, 167, 168, 169, 8190, 8191, 8192, 8193, 8194, 16383, 16384, 16385, 27040, 100000, 1 << 20, (1 << 20) + 8191, 5000000 };
    int bad = 0;
    for (size_t n : sizes)
    {
        std::vector<unsigned char> m(n + 1);
        for (auto& b : m) b = (unsigned char)rng();
        unsigned char a[32], b[32];
        KangarooTwelve(m.data(), (unsigned int)n, a, 32);
        k12::hash(m.data(), n, b, 32);
        bool ok = std::memcmp(a, b, 32) == 0;
        std::printf("len %8zu: %s\n", n, ok ? "match" : "MISMATCH");
        bad += !ok;
    }
    // 64 -> 32 Merkle node function
    for (int i = 0; i < 16; i++)
    {
        unsigned char in[64], a[32], b[32];
        for (auto& x : in) x = (unsigned char)rng();
        KangarooTwelve64To32(in, a);
        k12::hash64to32(in, b);
        bool ok = std::memcmp(a, b, 32) == 0;
        if (!ok) std::printf("64to32 MISMATCH\n");
        bad += !ok;
    }
    std::printf(bad ? "FAILED\n" : "ALL MATCH (Qubic KangarooTwelve == standard KT128, empty customization; KangarooTwelve64To32 == KT128 of 64 bytes)\n");
    return bad;
}
