#ifndef DUKEMP_PCRECOMP_HEAP_H
#define DUKEMP_PCRECOMP_HEAP_H
#include <stdint.h>
/* Host-owned context; guest addresses and free-list links remain uint32_t.
 * Use with its owning process address space selected. Allocation and mapping
 * changes require the serialized guest owner, including worker callbacks. */
typedef struct pcrecomp_heap pcrecomp_heap;
pcrecomp_heap *pcrecomp_heap_create(uint32_t initial_size);
void pcrecomp_heap_destroy(pcrecomp_heap *heap);
uint32_t pcrecomp_heap_alloc(pcrecomp_heap *heap, uint32_t size, int zero);
int pcrecomp_heap_free(pcrecomp_heap *heap, uint32_t address);
uint32_t pcrecomp_heap_realloc(pcrecomp_heap *heap, uint32_t address,
                              uint32_t size, int zero, int in_place_only);
/* Returns UINT32_MAX for a pointer that is not a live allocation of this heap. */
uint32_t pcrecomp_heap_size(pcrecomp_heap *heap, uint32_t address);
#endif
