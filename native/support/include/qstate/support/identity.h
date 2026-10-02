// Qubic identities: 32-byte public key <-> 60 letter text (56 base-26 letters from four little-endian uint64
// words + 4 checksum letters derived from KangarooTwelve of the key), state digests as identities, hex helpers,
// contract ids and asset names. All functions are pure and thread-safe.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qstate::support {

using PublicKey = std::array<uint8_t, 32>;

inline constexpr size_t kIdentityLength = 60;
inline constexpr uint32_t kMaxNumberOfContracts = 1024;

// 60 letters, upper case by default (public keys); digests use lower case.
std::string publicKeyToIdentity(const PublicKey& key, bool lowerCase = false);

enum class IdentityError { None, BadLength, BadCharacter, ValueOutOfRange, BadChecksum };
const char* identityErrorText(IdentityError error);

// Parses a 60 letter identity (either case) and verifies the checksum. On failure `key` is left untouched.
bool identityToPublicKey(std::string_view identity, PublicKey& key, IdentityError* error = nullptr);
// True when `identity` is a well formed identity with a valid checksum.
bool isValidIdentity(std::string_view identity);

// Identity text of a 32-byte digest (the state digest shown by the node), lower case.
std::string digestToIdentity(const std::array<uint8_t, 32>& digest);
std::string digestToIdentity(const uint8_t* digest32);

// Contract ids are id(index, 0, 0, 0): the first uint64 is the index, the other 24 bytes are zero.
// Returns the index (0 for the all-zero key) or nullopt when the key is not of that shape or index >= maxContracts.
std::optional<uint32_t> isContractId(const PublicKey& key, uint32_t maxContracts = kMaxNumberOfContracts);
PublicKey contractIdOf(uint32_t contractIndex);

// Asset names are up to 8 ASCII bytes packed little endian into a uint64 (QX = 0x5851); trailing zero bytes end the
// name. stringToAssetName() returns nullopt for names longer than 8 characters or containing NUL.
std::string assetNameToString(uint64_t assetName);
std::optional<uint64_t> stringToAssetName(std::string_view name);

// Lower case hex.
std::string toHex(const uint8_t* data, size_t size);
std::string toHex(const std::vector<uint8_t>& data);
template <size_t N>
std::string toHex(const std::array<uint8_t, N>& data) {
    return toHex(data.data(), N);
}
// Accepts both cases, no separators, even length. On failure `out` is cleared and false is returned.
bool fromHex(std::string_view hex, std::vector<uint8_t>& out);
bool hexToPublicKey(std::string_view hex, PublicKey& key);

} // namespace qstate::support
