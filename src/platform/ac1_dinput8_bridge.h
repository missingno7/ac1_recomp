#ifndef AC1_DINPUT8_BRIDGE_H
#define AC1_DINPUT8_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AC1_DINPUT8_GUEST_READ = 1u,
    AC1_DINPUT8_GUEST_WRITE = 2u,
    AC1_DINPUT8_NOT_HANDLED = 0,
    AC1_DINPUT8_HANDLED = 1,
    AC1_DINPUT8_ERROR = -1,
    AC1_DINPUT8_METHOD_UNSUPPORTED = -2
};

/* Dedicated synthetic COM-token range: DS8 uses 0x7d and D3D9 uses 0x7e. */
#define AC1_DINPUT8_TOKEN_BASE UINT32_C(0x7c000000)

typedef void *(*ac1_dinput8_guest_resolve_fn)(void *context, uint32_t address,
                                               uint32_t size, uint32_t access);
typedef uint32_t (*ac1_dinput8_guest_alloc_fn)(void *context, uint32_t size,
                                                uint32_t alignment);
typedef void (*ac1_dinput8_guest_free_fn)(void *context, uint32_t address);
typedef uintptr_t (*ac1_dinput8_guest_hwnd_to_host_fn)(void *context,
                                                       uint32_t guest_hwnd);
typedef uintptr_t (*ac1_dinput8_guest_hinstance_to_host_fn)(void *context,
                                                            uint32_t guest_hinstance);
typedef void (*ac1_dinput8_log_fn)(void *context, const char *message);

typedef struct ac1_dinput8_callbacks {
    uint32_t struct_size;
    ac1_dinput8_guest_resolve_fn guest_resolve;
    ac1_dinput8_guest_alloc_fn guest_alloc;
    ac1_dinput8_guest_free_fn guest_free;
    ac1_dinput8_guest_hwnd_to_host_fn guest_hwnd_to_host;
    ac1_dinput8_guest_hinstance_to_host_fn guest_hinstance_to_host;
    ac1_dinput8_log_fn log;
} ac1_dinput8_callbacks;

/* EnumObjects and EnumDevices are collected natively first, then guest
 * callbacks are delivered by the runtime after dispatch returns. Records use
 * exact pack-4 x86 W layouts: DIDEVICEOBJECTINSTANCEW 0x240 bytes, or
 * DIDEVICEINSTANCEW 0x44c bytes (DI 8.0). `opaque` is bridge-owned storage
 * and must be released with ac1_dinput8_enum_batch_free. */
typedef struct ac1_dinput8_enum_batch {
    void *opaque;
    uint32_t count;
    uint32_t record_size;
    int32_t native_hresult;
} ac1_dinput8_enum_batch;

/* Configure the active x86 guest address space and host-handle translations.
 * This must be called before use and only when no proxy objects are live. */
int ac1_dinput8_configure(const ac1_dinput8_callbacks *callbacks,
                          void *context);

/* DirectInput8Create import adapter. The guest IID is read as a 16-byte GUID;
 * guest HINSTANCE is translated through the callback and never cast to a host
 * pointer. `native_result_out` receives the native HRESULT on HANDLED, whether
 * that HRESULT indicates success or failure. Aggregation is unsupported. */
int ac1_dinput8_create8(uint32_t guest_hinstance, uint32_t version,
                        uint32_t guest_iid, uint32_t guest_output,
                        uint32_t guest_outer, uint32_t *native_result_out);

/* x86 stack words including `this`: 0 foreign token, positive supported
 * method arity, -1 malformed owned token/slot, -2 owned unsupported method. */
int ac1_dinput8_argument_count(uint32_t token);

/* args[0] is guest `this`; remaining words are x86 ABI arguments. Returns 1
 * for a native call (result_out valid, including ordinary HRESULT failures),
 * 0 for foreign token, -1 for invalid proxy/ABI state, -2 for an unsupported
 * method. Unsupported does not fabricate an HRESULT or stack arity. */
int ac1_dinput8_dispatch(uint32_t token, const uint32_t *args,
                         uint32_t arg_count, uint32_t *result_out);

/* Immediately after a handled device EnumObjects dispatch, take the native
 * records here. Invoke the guest callback from the runtime safe boundary with
 * (record_guest_address, original_ref_data), stopping when it returns FALSE.
 * A new batch is valid until freed. Returns 1 if a batch was pending, 0 if
 * none, and -1 for invalid output. */
int ac1_dinput8_enum_batch_take(ac1_dinput8_enum_batch *batch);
const void *ac1_dinput8_enum_batch_record(const ac1_dinput8_enum_batch *batch,
                                         uint32_t index);
void ac1_dinput8_enum_batch_free(ac1_dinput8_enum_batch *batch);

/* Release native COM references and guest proxy allocations before guest
 * address-space teardown. */
void ac1_dinput8_shutdown(void);
const char *ac1_dinput8_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
