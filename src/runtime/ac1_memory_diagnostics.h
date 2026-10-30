#ifndef AC1_MEMORY_DIAGNOSTICS_H
#define AC1_MEMORY_DIAGNOSTICS_H
#include <stddef.h>
#include <stdint.h>

/* Call only while holding the guest baton, like mapping/access operations. */
void ac1_guest_memory_dump_span(uint32_t address, size_t width);
#endif
