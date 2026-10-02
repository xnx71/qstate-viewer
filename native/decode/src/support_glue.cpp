#include "qstate/decode/support_glue.h"

#if defined(QSTATE_DECODE_HAS_SUPPORT) && QSTATE_DECODE_HAS_SUPPORT

#include <cstring>

#include "qstate/support/identity.h"

namespace qstate::decode {

std::size_t FileReaderByteSource::read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const {
    try {
        return reader_->read(offset, length, out);
    } catch (const std::exception&) {
        return 0;
    }
}

bool FileReaderByteSource::isAllZero(std::uint64_t offset, std::uint64_t length) const {
    try {
        return reader_->isAllZero(offset, length);
    } catch (const std::exception&) {
        return false;
    }
}

std::string SupportIdentityCodec::encode(const std::uint8_t* key32) const {
    support::PublicKey key;
    std::memcpy(key.data(), key32, 32);
    return support::publicKeyToIdentity(key);
}

bool SupportIdentityCodec::decode(std::string_view identity, std::uint8_t* out32, std::string* error) const {
    support::PublicKey key;
    support::IdentityError err = support::IdentityError::None;
    if (!support::identityToPublicKey(identity, key, &err)) {
        if (error) *error = support::identityErrorText(err);
        return false;
    }
    std::memcpy(out32, key.data(), 32);
    return true;
}

} // namespace qstate::decode

#endif
