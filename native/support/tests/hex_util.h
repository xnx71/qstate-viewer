// Tiny helpers shared by the support tests.
#pragma once

#include <string>

namespace testutil {

inline std::string toHex(const unsigned char* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

}  // namespace testutil
