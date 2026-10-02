// Internal: bulk reading of many fixed-size rows with read coalescing.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include "qstate/decode/byte_source.h"
#include "qstate/decode/decoder.h"

namespace qstate::decode {

// Calls fn(i, data, avail) for i in [0, n); the row i starts at offsetOf(i). Rows whose starts are close to each other
// are fetched with one read (runs of at most 1 MiB, gaps up to 32 KiB). `avail` < rowSize when the row is cut by the end
// of the data (data is valid for `avail` bytes). offsetOf need not be monotone, but is fastest when it is.
template <class OffsetFn, class Fn>
void gatherRows(const ByteSource& src, std::uint64_t n, OffsetFn&& offsetOf, std::size_t rowSize, Fn&& fn,
                const std::atomic<bool>* cancel) {
    constexpr std::uint64_t kMaxRun = 1u << 20, kGap = 32u << 10;
    std::vector<std::uint8_t> buf;
    std::uint64_t i = 0;
    while (i < n) {
        if (cancel && cancel->load(std::memory_order_relaxed)) throw CancelledError();
        const std::uint64_t start = offsetOf(i);
        std::uint64_t end = start + rowSize;
        std::uint64_t j = i + 1;
        while (j < n) {
            const std::uint64_t s = offsetOf(j);
            if (s < start || s > end + kGap || s + rowSize - start > kMaxRun) break;
            end = std::max<std::uint64_t>(end, s + rowSize);
            ++j;
        }
        const std::size_t len = static_cast<std::size_t>(end - start);
        if (buf.size() < len) buf.resize(len);
        const std::size_t got = src.read(start, len, buf.data());
        for (std::uint64_t k = i; k < j; ++k) {
            const std::uint64_t rel = offsetOf(k) - start;
            const std::size_t avail = got > rel ? static_cast<std::size_t>(std::min<std::uint64_t>(rowSize, got - rel)) : 0;
            fn(k, buf.data() + rel, avail);
        }
        i = j;
    }
}

} // namespace qstate::decode
