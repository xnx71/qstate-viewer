// Identity text <-> 32-byte public key. Real checksum computation needs KangarooTwelve (qstate::support); decode only
// needs the seam, so the checksum is injected.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace qstate::decode {

class IdentityCodec {
public:
    virtual ~IdentityCodec() = default;
    // 60 upper-case letters for the 32 key bytes (as stored). Must be thread-safe.
    virtual std::string encode(const std::uint8_t* key32) const = 0;
    // Parses a 60-letter identity. On failure returns false and (optionally) fills `error`.
    // Checksum verification is up to the implementation.
    virtual bool decode(std::string_view identity, std::uint8_t* out32, std::string* error) const = 0;
};

// Base-26 body (56 letters from the four little-endian uint64, least significant digit first) + 4 checksum letters
// from an injected 18-bit checksum function (K12(key,32 bytes)[0..3) & 0x3FFFF). Without a function the checksum
// letters are rendered as "????" and are not verified.
class BasicIdentityCodec final : public IdentityCodec {
public:
    using ChecksumFn = std::function<std::uint32_t(const std::uint8_t* key32)>;
    explicit BasicIdentityCodec(ChecksumFn checksum = nullptr) : checksum_(std::move(checksum)) {}

    std::string encode(const std::uint8_t* key32) const override;
    bool decode(std::string_view identity, std::uint8_t* out32, std::string* error) const override;

private:
    ChecksumFn checksum_;
};

} // namespace qstate::decode
