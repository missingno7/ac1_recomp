#ifndef AC1_DS8_BRIDGE_H
#define AC1_DS8_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AC1_DS8_GUEST_READ = 1u,
    AC1_DS8_GUEST_WRITE = 2u
};

enum {
    AC1_DS8_UNSUPPORTED = 0,
    AC1_DS8_HANDLED = 1,
    AC1_DS8_ERROR = -1,
    AC1_DS8_METHOD_UNSUPPORTED = -2
};

/* Separate from the D3D9 token family (0x7e000000) and Win32 import tokens. */
#define AC1_DS8_TOKEN_BASE UINT32_C(0x7d000000)

typedef void *(*ac1_ds8_guest_resolve_fn)(void *context, uint32_t address,
                                          uint32_t size, uint32_t access);
typedef uint32_t (*ac1_ds8_guest_alloc_fn)(void *context, uint32_t size,
                                           uint32_t alignment);
typedef void (*ac1_ds8_guest_free_fn)(void *context, uint32_t address);
typedef uintptr_t (*ac1_ds8_guest_hwnd_to_host_fn)(void *context,
                                                   uint32_t guest_hwnd);
typedef void (*ac1_ds8_log_fn)(void *context, const char *message);

typedef struct ac1_ds8_callbacks {
    uint32_t struct_size;
    ac1_ds8_guest_resolve_fn guest_resolve;
    ac1_ds8_guest_alloc_fn guest_alloc;
    ac1_ds8_guest_free_fn guest_free;
    ac1_ds8_guest_hwnd_to_host_fn guest_hwnd_to_host;
    ac1_ds8_log_fn log;
    /* Portable backend: signal_event runs under the guest scheduler baton,
     * never from an audio thread. Optional monotonic_ns injection supports
     * focused tests; it must also be safe on lifecycle control threads (it is
     * never called by the real-time mixer callback). */
    int (*signal_event)(void *context, uint32_t guest_event);
    uint64_t (*monotonic_ns)(void *context);
} ac1_ds8_callbacks;

/* Configure with the active process guest-memory and HWND adapters. Must be
 * called before factory/dispatch use and only when no proxy objects are live. */
int ac1_ds8_configure(const ac1_ds8_callbacks *callbacks, void *context);

/* Implements DSOUND ordinal 11 / DirectSoundCreate8. `guid_address` and
 * `output_address` are guest x86 addresses; outer aggregation is unsupported.
 * HANDLED means result_out is the native HRESULT, including native failures.
 * It never places a host COM pointer in guest memory. */
int ac1_ds8_create8(uint32_t guid_address, uint32_t output_address,
                    uint32_t outer_address, uint32_t *result_out);

/* Return x86 stack-word count (including `this`) for implemented tokens,
 * zero for a foreign token, -1 for malformed state, and -2 for an owned but
 * intentionally unsupported COM slot. */
int ac1_ds8_argument_count(uint32_t token);

/* `args[0]` is the guest COM object; following words are its x86 arguments.
 * Return 1 for a native call (and a valid result_out), 0 for a foreign token,
 * -1 for bridge/ABI errors, and -2 for an unsupported method/profile. */
int ac1_ds8_dispatch(uint32_t token, const uint32_t *args,
                     uint32_t arg_count, uint32_t *result_out);

/* Release native COM references and guest proxy allocations before guest
 * address-space teardown. The real System32 DirectSound module unloads here. */
void ac1_ds8_shutdown(void);
const char *ac1_ds8_last_error(void);
#if !defined(_WIN32) || defined(AC1_PORTABLE_TEST)
int ac1_ds8_service(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
