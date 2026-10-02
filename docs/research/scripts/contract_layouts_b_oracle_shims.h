// p05 oracle: g++ shims for MSVC-only intrinsics / literal suffixes used in function bodies of the core headers
#pragma once
#include <immintrin.h>
static inline unsigned long long _umul128(unsigned long long a, unsigned long long b, unsigned long long* hi)
{ unsigned __int128 p = (unsigned __int128)a * b; *hi = (unsigned long long)(p >> 64); return (unsigned long long)p; }
static inline long long _mul128(long long a, long long b, long long* hi)
{ __int128 p = (__int128)a * b; *hi = (long long)(p >> 64); return (long long)p; }
static inline unsigned long long __shiftleft128(unsigned long long lo, unsigned long long hi, unsigned char s)
{ s &= 63; return s ? ((hi << s) | (lo >> (64 - s))) : hi; }
static inline unsigned long long __shiftright128(unsigned long long lo, unsigned long long hi, unsigned char s)
{ s &= 63; return s ? ((lo >> s) | (hi << (64 - s))) : lo; }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wliteral-suffix"
constexpr long long operator""i64(unsigned long long v) { return (long long)v; }   // QReservePool.h uses 0i64
#pragma GCC diagnostic pop
