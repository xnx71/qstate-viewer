#include "qstate/decode/byte_source.h"

#include <algorithm>
#include <cstring>

namespace qstate::decode {

bool ByteSource::isAllZero(std::uint64_t offset, std::uint64_t length) const {
    if (length == 0) return true;
    const std::uint64_t total = size();
    if (offset > total || length > total - offset) return false;
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min<std::uint64_t>(length, kChunk)));
    std::uint64_t done = 0;
    while (done < length) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(length - done, buf.size()));
        const std::size_t got = read(offset + done, want, buf.data());
        if (got != want) return false;
        for (std::size_t i = 0; i < got; ++i)
            if (buf[i] != 0) return false;
        done += got;
    }
    return true;
}

std::size_t MemoryByteSource::read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const {
    if (offset >= data_.size()) return 0;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(length, data_.size() - offset));
    if (n) std::memcpy(out, data_.data() + offset, n);
    return n;
}

bool MemoryByteSource::isAllZero(std::uint64_t offset, std::uint64_t length) const {
    if (length == 0) return true;
    if (offset > data_.size() || length > data_.size() - offset) return false;
    const std::uint8_t* p = data_.data() + offset;
    for (std::uint64_t i = 0; i < length; ++i)
        if (p[i] != 0) return false;
    return true;
}

} // namespace qstate::decode
