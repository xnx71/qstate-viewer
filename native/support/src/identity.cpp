#include "qstate/support/identity.h"

#include <cstring>

#include "qstate/support/k12.h"

namespace qstate::support {

namespace {

constexpr size_t kFragmentLetters = 14;  // 26^14 > 2^64
constexpr size_t kChecksumLetters = 4;

uint64_t loadLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void storeLe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

uint32_t checksumOf(const uint8_t* key32) {
    uint8_t c[3];
    k12(key32, 32, c, 3);
    return (static_cast<uint32_t>(c[0]) | (static_cast<uint32_t>(c[1]) << 8) | (static_cast<uint32_t>(c[2]) << 16)) &
           0x3FFFFu;
}

void encode(const uint8_t* key32, char base, char* out) {
    for (size_t i = 0; i < 4; i++) {
        uint64_t frag = loadLe64(key32 + 8 * i);
        for (size_t j = 0; j < kFragmentLetters; j++) {
            out[i * kFragmentLetters + j] = static_cast<char>(base + frag % 26);
            frag /= 26;
        }
    }
    uint32_t c = checksumOf(key32);
    for (size_t i = 0; i < kChecksumLetters; i++) {
        out[56 + i] = static_cast<char>(base + c % 26);
        c /= 26;
    }
}

int letterValue(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    return -1;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

}  // namespace

std::string publicKeyToIdentity(const PublicKey& key, bool lowerCase) {
    std::string s(kIdentityLength, 'A');
    encode(key.data(), lowerCase ? 'a' : 'A', s.data());
    return s;
}

const char* identityErrorText(IdentityError error) {
    switch (error) {
        case IdentityError::None: return "ok";
        case IdentityError::BadLength: return "an identity has exactly 60 letters";
        case IdentityError::BadCharacter: return "an identity consists of the letters A-Z only";
        case IdentityError::ValueOutOfRange: return "identity letters encode a value that is out of range";
        case IdentityError::BadChecksum: return "identity checksum mismatch";
    }
    return "unknown";
}

bool identityToPublicKey(std::string_view identity, PublicKey& key, IdentityError* error) {
    auto fail = [&](IdentityError e) {
        if (error) *error = e;
        return false;
    };
    if (identity.size() != kIdentityLength) return fail(IdentityError::BadLength);
    for (char ch : identity) {
        if (letterValue(ch) < 0) return fail(IdentityError::BadCharacter);
    }
    PublicKey candidate{};
    for (size_t i = 0; i < 4; i++) {
        uint64_t frag = 0;
        for (size_t j = kFragmentLetters; j-- > 0;) {
            const auto digit = static_cast<uint64_t>(letterValue(identity[i * kFragmentLetters + j]));
            // Only values below 2^64 are produced by the encoder; larger ones would alias another key.
            if (frag > (UINT64_MAX - digit) / 26) return fail(IdentityError::ValueOutOfRange);
            frag = frag * 26 + digit;
        }
        storeLe64(candidate.data() + 8 * i, frag);
    }
    uint32_t expected = checksumOf(candidate.data());
    uint32_t given = 0;
    for (size_t i = kChecksumLetters; i-- > 0;) {
        given = given * 26 + static_cast<uint32_t>(letterValue(identity[56 + i]));
    }
    if (given != expected) return fail(IdentityError::BadChecksum);
    key = candidate;
    if (error) *error = IdentityError::None;
    return true;
}

bool isValidIdentity(std::string_view identity) {
    PublicKey k;
    return identityToPublicKey(identity, k);
}

std::string digestToIdentity(const uint8_t* digest32) {
    std::string s(kIdentityLength, 'a');
    encode(digest32, 'a', s.data());
    return s;
}

std::string digestToIdentity(const std::array<uint8_t, 32>& digest) { return digestToIdentity(digest.data()); }

std::optional<uint32_t> isContractId(const PublicKey& key, uint32_t maxContracts) {
    for (size_t i = 8; i < key.size(); i++) {
        if (key[i] != 0) return std::nullopt;
    }
    const uint64_t index = loadLe64(key.data());
    if (index >= maxContracts) return std::nullopt;
    return static_cast<uint32_t>(index);
}

PublicKey contractIdOf(uint32_t contractIndex) {
    PublicKey key{};
    storeLe64(key.data(), contractIndex);
    return key;
}

std::string assetNameToString(uint64_t assetName) {
    std::string s;
    for (int i = 0; i < 8; i++) {
        const char ch = static_cast<char>((assetName >> (8 * i)) & 0xFF);
        if (ch == 0) break;
        s += ch;
    }
    return s;
}

std::optional<uint64_t> stringToAssetName(std::string_view name) {
    if (name.size() > 8) return std::nullopt;
    uint64_t v = 0;
    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] == '\0') return std::nullopt;
        v |= static_cast<uint64_t>(static_cast<uint8_t>(name[i])) << (8 * i);
    }
    return v;
}

std::string toHex(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.resize(size * 2);
    for (size_t i = 0; i < size; i++) {
        s[2 * i] = digits[data[i] >> 4];
        s[2 * i + 1] = digits[data[i] & 15];
    }
    return s;
}

std::string toHex(const std::vector<uint8_t>& data) { return toHex(data.data(), data.size()); }

bool fromHex(std::string_view hex, std::vector<uint8_t>& out) {
    out.clear();
    if (hex.size() % 2 != 0) return false;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexValue(hex[i]);
        const int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return true;
}

bool hexToPublicKey(std::string_view hex, PublicKey& key) {
    std::vector<uint8_t> bytes;
    if (hex.size() != 64 || !fromHex(hex, bytes)) return false;
    std::memcpy(key.data(), bytes.data(), 32);
    return true;
}

}  // namespace qstate::support
