#ifndef AC1_D3D9_BRIDGE_H
#define AC1_D3D9_BRIDGE_H

#include <stdint.h>

#if defined(_WIN32) && defined(AC1_D3D9_BRIDGE_SHARED)
#  if defined(AC1_D3D9_BRIDGE_BUILD)
#    define AC1_D3D9_API __declspec(dllexport)
#  else
#    define AC1_D3D9_API __declspec(dllimport)
#  endif
#elif defined(_WIN32)
#  define AC1_D3D9_API
#else
#  define AC1_D3D9_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum ac1_d3d9_guest_access {
    AC1_D3D9_GUEST_READ = 1,
    AC1_D3D9_GUEST_WRITE = 2
};

typedef void *(*ac1_d3d9_guest_resolve_fn)(void *context, uint32_t address,
                                           uint32_t size, uint32_t access);
typedef uint32_t (*ac1_d3d9_guest_alloc_fn)(void *context, uint32_t size,
                                            uint32_t alignment);
typedef void (*ac1_d3d9_guest_free_fn)(void *context, uint32_t address);
typedef uintptr_t (*ac1_d3d9_guest_hwnd_to_host_fn)(void *context,
                                                    uint32_t guest_hwnd);
typedef uint32_t (*ac1_d3d9_host_monitor_to_guest_fn)(void *context,
                                                      uintptr_t host_monitor);
typedef void (*ac1_d3d9_log_fn)(void *context, const char *message);
typedef void *(*ac1_d3d9_factory_fn)(void *context, uint32_t sdk_version);

typedef struct ac1_d3d9_callbacks {
    uint32_t struct_size;
    ac1_d3d9_guest_resolve_fn guest_resolve;
    ac1_d3d9_guest_alloc_fn guest_alloc;
    ac1_d3d9_guest_free_fn guest_free;
    ac1_d3d9_guest_hwnd_to_host_fn guest_hwnd_to_host;
    ac1_d3d9_host_monitor_to_guest_fn host_monitor_to_guest;
    ac1_d3d9_log_fn log;
    /* Optional platform factory. When null on Windows, the bridge loads
     * Direct3DCreate9 from the absolute system-directory d3d9.dll. */
    ac1_d3d9_factory_fn factory;
} ac1_d3d9_callbacks;

/* Install guest-memory/platform callbacks. Call only with no live bridge
 * objects. Returns 1 on success. The callback table is copied. */
AC1_D3D9_API int ac1_d3d9_configure(const ac1_d3d9_callbacks *callbacks,
                                    void *context);

/* Implements the d3d9.dll import. Returns a 32-bit guest COM object address,
 * or zero with ac1_d3d9_last_error() populated. */
AC1_D3D9_API uint32_t ac1_d3d9_create9(uint32_t sdk_version);

/* Native Windows adapter for the observed D3DXCheckTextureRequirements import.
 * Guest in/out pointers are nullable; return values are 1 when native D3DX ran,
 * -1 for invalid guest/device ABI, and -2 when the platform or required export
 * is unavailable. Only return 1 makes native_hresult_out valid. */
AC1_D3D9_API int ac1_d3d9_check_texture_requirements(
    uint32_t guest_device, uint32_t guest_width, uint32_t guest_height,
    uint32_t guest_mip_levels, uint32_t usage, uint32_t guest_format,
    uint32_t pool, uint32_t *native_hresult_out);

/* Windows native D3DXGetShaderConstantTable import bridge. Validates and
 * stages a bounded SM3 guest token stream, wraps the native table pointer, and
 * writes only its synthetic guest proxy to guest_output. Returns 1 when native
 * D3DX ran (native_hresult_out is valid), -1 for invalid guest ABI/memory, and
 * -2 when the platform/export/profile is unsupported. */
AC1_D3D9_API int ac1_d3d9_get_shader_constant_table(
    uint32_t guest_bytecode, uint32_t guest_output,
    uint32_t *native_hresult_out);

/* Return the exact number of x86 stack words, including guest `this`, for a
 * supported token. Returns 0 for a foreign token, -1 for a malformed owned
 * token/slot, and -2 for an owned but unsupported COM method. */
AC1_D3D9_API int ac1_d3d9_argument_count(uint32_t token);

/* args[0] is guest `this`; subsequent entries are the explicit x86 COM
 * arguments in source order. Returns 1 for a handled native call (including
 * ordinary native HRESULT failure), 0 for a foreign token, -1 for invalid
 * object/ABI state, and -2 for an unsupported method. Only return code 1
 * makes result_out a valid guest EAX value. */
AC1_D3D9_API int ac1_d3d9_dispatch(uint32_t token, const uint32_t *args,
                                   uint32_t arg_count,
                                   uint32_t *result_out);

/* Release host COM objects and bridge-owned guest object/vtable allocations. */
AC1_D3D9_API void ac1_d3d9_shutdown(void);
AC1_D3D9_API const char *ac1_d3d9_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
