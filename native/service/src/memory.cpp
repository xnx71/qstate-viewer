#include "memory.h"

#if defined(_WIN32)
#define NOMINMAX
#include <malloc.h>
#include <windows.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace qstate::service {

void releaseFreeHeapMemory() {
#if defined(_WIN32)
    _heapmin();
    // Pages that are not touched again drop out of the working set (they stay valid and are faulted back on use).
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

} // namespace qstate::service
