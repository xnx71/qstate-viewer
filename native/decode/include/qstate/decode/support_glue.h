// Glue between decode's abstract seams (ByteSource, IdentityCodec) and qstate::support. Only available when the
// decode library was built with qstate::support (QSTATE_DECODE_HAS_SUPPORT).
#pragma once

#if defined(QSTATE_DECODE_HAS_SUPPORT) && QSTATE_DECODE_HAS_SUPPORT

#include <memory>

#include "qstate/decode/byte_source.h"
#include "qstate/decode/identity.h"
#include "qstate/support/file_reader.h"

namespace qstate::decode {

// ByteSource over a support::FileReader. I/O errors are reported as short reads (never thrown).
class FileReaderByteSource final : public ByteSource {
public:
    explicit FileReaderByteSource(std::shared_ptr<const support::FileReader> reader) : reader_(std::move(reader)) {}

    std::uint64_t size() const override { return reader_->size(); }
    std::size_t read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const override;
    bool isAllZero(std::uint64_t offset, std::uint64_t length) const override;

    const support::FileReader& reader() const { return *reader_; }

private:
    std::shared_ptr<const support::FileReader> reader_;
};

// The real identity codec: support::publicKeyToIdentity / identityToPublicKey (K12 checksum verified on decode).
class SupportIdentityCodec final : public IdentityCodec {
public:
    std::string encode(const std::uint8_t* key32) const override;
    bool decode(std::string_view identity, std::uint8_t* out32, std::string* error) const override;
};

} // namespace qstate::decode

#endif
