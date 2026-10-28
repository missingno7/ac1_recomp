#ifndef AC1_NETWORK_H
#define AC1_NETWORK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Handles only the Winsock services implemented by this compatibility layer.
 * arguments contains the 32-bit stdcall arguments, excluding the return
 * address. On success, eax_out is the API result and stack_bytes_out is the
 * exact number of bytes for the caller to pop. Returns 1 when handled and 0
 * for any unimplemented Winsock import. */
int ac1_network_import(const char *dll_name, const char *name,
                       uint16_t ordinal, int by_ordinal,
                       const uint32_t *arguments, uint32_t *eax_out,
                       uint32_t *stack_bytes_out);

/* Winsock error state is per native thread, as Windows specifies. The runtime
 * calls reset after joining guest workers and before releasing guest memory. */
uint32_t ac1_network_last_error(void);
void ac1_network_set_last_error(uint32_t error);
void ac1_network_reset(void);

#ifdef __cplusplus
}
#endif

#endif
