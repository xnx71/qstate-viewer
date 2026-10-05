// 128-bit integers for table filters / formatting: the compiler's __int128 where it exists (GCC, Clang), a small
// portable implementation elsewhere (MSVC). The portable types are always compiled (namespace `portable`) so they can be
// tested against __int128 on every platform; define QSTATE_PORTABLE_INT128 to select them as i128 / u128.
//
// Only the operations the decoder needs are provided. Semantics follow __int128: arithmetic wraps modulo 2^128,
// conversions to narrower integer types truncate, shifts of 128 or more bits give 0.
#pragma once

#include <cstdint>
#include <type_traits>

namespace qstate::decode::wide {

namespace portable {

struct i128;

template <typename T>
constexpr bool isNegative(T v) {
    if constexpr (std::is_signed_v<T>) return v < 0;
    else return false;
}

struct u128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    constexpr u128() = default;
    constexpr u128(std::uint64_t high, std::uint64_t low, int) : lo(low), hi(high) {}

    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    constexpr u128(T v) : lo(static_cast<std::uint64_t>(v)), hi(isNegative(v) ? ~std::uint64_t{0} : 0) {}

    constexpr explicit u128(const i128& v);

    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    constexpr explicit operator T() const {
        return static_cast<T>(lo);
    }

    friend constexpr bool operator==(const u128& a, const u128& b) { return a.lo == b.lo && a.hi == b.hi; }
    friend constexpr bool operator!=(const u128& a, const u128& b) { return !(a == b); }
    friend constexpr bool operator<(const u128& a, const u128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
    friend constexpr bool operator>(const u128& a, const u128& b) { return b < a; }
    friend constexpr bool operator<=(const u128& a, const u128& b) { return !(b < a); }
    friend constexpr bool operator>=(const u128& a, const u128& b) { return !(a < b); }

    friend constexpr u128 operator~(const u128& a) { return u128(~a.hi, ~a.lo, 0); }
    friend constexpr u128 operator|(const u128& a, const u128& b) { return u128(a.hi | b.hi, a.lo | b.lo, 0); }
    friend constexpr u128 operator&(const u128& a, const u128& b) { return u128(a.hi & b.hi, a.lo & b.lo, 0); }
    friend constexpr u128 operator^(const u128& a, const u128& b) { return u128(a.hi ^ b.hi, a.lo ^ b.lo, 0); }

    friend constexpr u128 operator+(const u128& a, const u128& b) {
        const std::uint64_t lo = a.lo + b.lo;
        return u128(a.hi + b.hi + (lo < a.lo ? 1 : 0), lo, 0);
    }
    friend constexpr u128 operator-(const u128& a, const u128& b) {
        return u128(a.hi - b.hi - (a.lo < b.lo ? 1 : 0), a.lo - b.lo, 0);
    }

    friend constexpr u128 operator<<(const u128& a, int n) {
        if (n <= 0) return a;
        if (n >= 128) return u128();
        if (n >= 64) return u128(a.lo << (n - 64), 0, 0);
        return u128((a.hi << n) | (a.lo >> (64 - n)), a.lo << n, 0);
    }
    friend constexpr u128 operator>>(const u128& a, int n) {
        if (n <= 0) return a;
        if (n >= 128) return u128();
        if (n >= 64) return u128(0, a.hi >> (n - 64), 0);
        return u128(a.hi >> n, (a.lo >> n) | (a.hi << (64 - n)), 0);
    }

    // Low 128 bits of the product.
    friend constexpr u128 operator*(const u128& a, const u128& b) {
        const std::uint64_t a0 = a.lo & 0xFFFFFFFFull, a1 = a.lo >> 32, b0 = b.lo & 0xFFFFFFFFull, b1 = b.lo >> 32;
        const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
        const std::uint64_t lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
        const std::uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32) + a.hi * b.lo + a.lo * b.hi;
        return u128(hi, lo, 0);
    }

    // Quotient and remainder; division by zero gives 0 / 0 (undefined for __int128).
    static constexpr void divmod(const u128& n, const u128& d, u128& q, u128& r) {
        q = u128();
        r = u128();
        if (d == u128()) return;
        for (int bit = 127; bit >= 0; --bit) {
            r = r << 1;
            r.lo |= (bit >= 64 ? (n.hi >> (bit - 64)) : (n.lo >> bit)) & 1;
            if (r >= d) {
                r = r - d;
                if (bit >= 64) q.hi |= std::uint64_t{1} << (bit - 64);
                else q.lo |= std::uint64_t{1} << bit;
            }
        }
    }
    friend constexpr u128 operator/(const u128& a, const u128& b) {
        u128 q, r;
        divmod(a, b, q, r);
        return q;
    }
    friend constexpr u128 operator%(const u128& a, const u128& b) {
        u128 q, r;
        divmod(a, b, q, r);
        return r;
    }

    constexpr u128& operator+=(const u128& o) { return *this = *this + o; }
    constexpr u128& operator-=(const u128& o) { return *this = *this - o; }
    constexpr u128& operator*=(const u128& o) { return *this = *this * o; }
    constexpr u128& operator/=(const u128& o) { return *this = *this / o; }
    constexpr u128& operator%=(const u128& o) { return *this = *this % o; }
};

// Two's complement.
struct i128 {
    std::uint64_t lo = 0;
    std::int64_t hi = 0;

    constexpr i128() = default;

    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    constexpr i128(T v)
        : lo(static_cast<std::uint64_t>(v)), hi(isNegative(v) ? std::int64_t{-1} : std::int64_t{0}) {}

    constexpr explicit i128(const u128& v) : lo(v.lo), hi(static_cast<std::int64_t>(v.hi)) {}

    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    constexpr explicit operator T() const {
        return static_cast<T>(lo);
    }

    friend constexpr bool operator==(const i128& a, const i128& b) { return a.lo == b.lo && a.hi == b.hi; }
    friend constexpr bool operator!=(const i128& a, const i128& b) { return !(a == b); }
    friend constexpr bool operator<(const i128& a, const i128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
    friend constexpr bool operator>(const i128& a, const i128& b) { return b < a; }
    friend constexpr bool operator<=(const i128& a, const i128& b) { return !(b < a); }
    friend constexpr bool operator>=(const i128& a, const i128& b) { return !(a < b); }

    friend constexpr i128 operator-(const i128& a) {
        i128 r;
        r.lo = ~a.lo + 1;
        r.hi = static_cast<std::int64_t>(~static_cast<std::uint64_t>(a.hi) + (a.lo == 0 ? 1 : 0));
        return r;
    }
};

constexpr u128::u128(const i128& v) : lo(v.lo), hi(static_cast<std::uint64_t>(v.hi)) {}

} // namespace portable

#if defined(__SIZEOF_INT128__) && !defined(QSTATE_PORTABLE_INT128)
__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;
#else
using i128 = portable::i128;
using u128 = portable::u128;
#endif

} // namespace qstate::decode::wide
