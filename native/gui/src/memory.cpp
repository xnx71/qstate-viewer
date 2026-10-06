#include "qstate/gui/memory.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace qstate::gui {

void tuneAllocator() {
#if defined(__GLIBC__)
    mallopt(M_MMAP_THRESHOLD, 256 * 1024);
    mallopt(M_ARENA_MAX, 2);
#endif
}

} // namespace qstate::gui
