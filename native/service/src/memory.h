// Internal: returning free heap memory to the operating system.
#pragma once

namespace qstate::service {

// glibc: malloc_trim(0). Windows: _heapmin() and a working set trim of this process (unverified there, see
// docs/MEMORY.md). Other platforms: nothing. Cheap when there is little to return; safe from any thread.
void releaseFreeHeapMemory();

} // namespace qstate::service
