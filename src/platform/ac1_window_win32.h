#ifndef AC1_WINDOW_WIN32_H
#define AC1_WINDOW_WIN32_H

#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ac1_window_win32 ac1_window_win32;

typedef int (*ac1_window_guest_read_fn)(void *context, uint32_t address,
                                        void *output, size_t bytes);
typedef int (*ac1_window_guest_write_fn)(void *context, uint32_t address,
                                         const void *input, size_t bytes);
typedef int (*ac1_window_guest_alloc_fn)(void *context, size_t bytes,
                                         uint32_t *guest_address);
typedef void (*ac1_window_guest_free_fn)(void *context, uint32_t guest_address);
/* Called only from a guest USER32 import handler, never from native WndProc. */
typedef int (*ac1_window_invoke_wndproc_fn)(void *context, CPU *active_cpu,
    uint32_t procedure, uint32_t guest_hwnd, uint32_t message,
    uint32_t wparam, uint32_t lparam, uint32_t *result);
typedef uint32_t (*ac1_window_thread_id_fn)(void *context);

typedef struct ac1_window_win32_callbacks {
    void *context;
    ac1_window_guest_read_fn read_guest;
    ac1_window_guest_write_fn write_guest;
    ac1_window_guest_alloc_fn allocate_guest;
    ac1_window_guest_free_fn free_guest;
    ac1_window_invoke_wndproc_fn invoke_wndproc;
    ac1_window_thread_id_fn current_guest_thread_id;
} ac1_window_win32_callbacks;

enum {
    AC1_WINDOW_WIN32_UNSUPPORTED = 0,
    AC1_WINDOW_WIN32_HANDLED = 1,
    AC1_WINDOW_WIN32_ERROR = -1
};

ac1_window_win32 *ac1_window_win32_create(
    const ac1_window_win32_callbacks *callbacks,
    char *error, size_t error_capacity);
void ac1_window_win32_destroy(ac1_window_win32 *window);

/* Resolves only a live guest HWND owned by this bridge to its real HWND. */
uintptr_t ac1_window_win32_resolve_hwnd(ac1_window_win32 *window,
                                        uint32_t guest_hwnd);

/* Handles the evidenced USER32 import subset. Unknown APIs return
 * AC1_WINDOW_WIN32_UNSUPPORTED with a diagnostic in error. */
int ac1_window_win32_call(ac1_window_win32 *window, CPU *cpu,
    const char *import_name, uint32_t *last_error,
    char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
