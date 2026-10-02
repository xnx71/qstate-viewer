#include "qstate/decode/identity.h"

#include <cstring>

namespace qstate::decode {
namespace {

std::uint64_t loadLe64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void storeLe64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

} // namespace

std::string BasicIdentityCodec::encode(const std::uint8_t* key32) const {
    std::string out(60, 'A');
    for (int i = 0; i < 4; ++i) {
        std::uint64_t frag = loadLe64(key32 + 8 * i);
        for (int j = 0; j < 14; ++j) {
            out[static_cast<std::size_t>(14 * i + j)] = static_cast<char>('A' + frag % 26);
            frag /= 26;
        }
    }
    if (checksum_) {
        std::uint32_t c = checksum_(key32) & 0x3FFFFu;
        for (int i = 0; i < 4; ++i) {
            out[static_cast<std::size_t>(56 + i)] = static_cast<char>('A' + c % 26);
            c /= 26;
        }
    } else {
        for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(56 + i)] = '?';
    }
    return out;
}

bool BasicIdentityCodec::decode(std::string_view identity, std::uint8_t* out32, std::string* error) const {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (identity.size() != 60) return fail("identity must have 60 letters");
    for (char c : identity)
        if (c < 'A' || c > 'Z') return fail("identity must consist of the letters A-Z");
    std::uint8_t key[32];
    for (int i = 0; i < 4; ++i) {
        std::uint64_t frag = 0;
        for (int j = 13; j >= 0; --j)
            frag = frag * 26 + static_cast<std::uint64_t>(identity[static_cast<std::size_t>(14 * i + j)] - 'A');
        storeLe64(key + 8 * i, frag);
    }
    if (checksum_) {
        std::uint32_t c = checksum_(key) & 0x3FFFFu;
        for (int i = 0; i < 4; ++i) {
            if (identity[static_cast<std::size_t>(56 + i)] != static_cast<char>('A' + c % 26))
                return fail("identity checksum mismatch");
            c /= 26;
        }
    }
    std::memcpy(out32, key, 32);
    return true;
}

} // namespace qstate::decode
