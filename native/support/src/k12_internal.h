// Internal declarations shared by k12.cpp and the optional AVX2 translation unit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qstate::support::detail {

// Four leaves at once: `data` holds four consecutive 8192-byte chunks; writes four 32-byte chaining values.
// Only available (and only called) when avx2Compiled() && the CPU supports AVX2.
bool avx2Compiled();
void k12LeafChainingValues4Avx2(const uint8_t* data, uint8_t* cvs);

} // namespace qstate::support::detail
