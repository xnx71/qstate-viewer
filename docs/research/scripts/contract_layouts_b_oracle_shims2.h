// p05 oracle: after qpi.h -- Qdraw.h:207 calls mod(uint64_t&, const QPI::uint64&) which fails template deduction
// on LP64 (unsigned long vs unsigned long long); provide an exact-match non-template overload.
#pragma once
namespace QPI { inline constexpr unsigned long long mod(unsigned long a, unsigned long long b) { return b ? (a % b) : 0; } }
