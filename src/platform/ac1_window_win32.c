#define WIN32_LEAN_AND_MEAN
#include "ac1_window_win32.h"

#ifndef _WIN32
#error ac1_window_win32.c is Windows-only
#endif

#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define AC1_WINDOW_MAX_CLASSES 8u
#define AC1_WINDOW_MAX_WINDOWS 8u
#define AC1_WINDOW_MAX_MESSAGES 128u
#define AC1_WINDOW_TOKEN_BASE UINT32_C(0xE4000004)
#define AC1_CURSOR_TOKEN UINT32_C(0xE5000004)
#define AC1_DESKTOP_TOKEN UINT32_C(0x0000FFF0)
#define AC1_GUEST_MSG_BYTES 28u

typedef struct ac1_guest_msg32 {
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t x, y;
} ac1_guest_msg32;

typedef struct ac1_guest_wndclassex32 {
    uint32_t cb_size, style, wndproc;
    int32_t class_extra, window_extra;
    uint32_t instance, icon, cursor, background;
    uint32_t menu_name, class_name, small_icon;
} ac1_guest_wndclassex32;

typedef struct ac1_guest_createstruct32 {
    uint32_t create_params, instance, menu, parent;
    int32_t height, width, y, x, style;
    uint32_t name, class_name, ex_style;
} ac1_guest_createstruct32;

typedef struct ac1_window_class {
    char guest_name[256];
    wchar_t native_name[96];
    uint32_t atom, procedure, style, instance;
    HCURSOR cursor;
    int registered;
} ac1_window_class;

typedef struct ac1_window_record {
    struct ac1_window_win32 *owner;
    HWND native;
    uint32_t token, class_index, guest_thread_id, style, ex_style;
    uint32_t guest_instance, guest_menu, guest_parent, guest_param;
    int32_t x, y, width, height;
    int destroying;
} ac1_window_record;

typedef struct ac1_queued_message {
    ac1_guest_msg32 message;
    uint32_t guest_thread_id;
    LRESULT native_default_result;
    int removed;
} ac1_queued_message;

struct ac1_window_win32 {
    ac1_window_win32_callbacks callbacks;
    ac1_window_class classes[AC1_WINDOW_MAX_CLASSES];
    ac1_window_record windows[AC1_WINDOW_MAX_WINDOWS];
    ac1_queued_message messages[AC1_WINDOW_MAX_MESSAGES];
    unsigned class_count, window_count, message_count;
    uint32_t next_token;
    int show_cursor_balance;
    int queue_overflow;
    uint32_t active_default_hwnd, active_default_message;
    uint32_t active_default_wparam, active_default_lparam;
    LRESULT active_default_result;
    unsigned active_default_depth;
    int active_default_valid;
};

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) (void)snprintf(error, capacity, "%s", message ? message : "");
}

static int guest_read(ac1_window_win32 *ui, uint32_t address,
                      void *output, size_t bytes)
{
    return ui && address && output && bytes &&
           ui->callbacks.read_guest(ui->callbacks.context, address, output, bytes);
}

static int guest_write(ac1_window_win32 *ui, uint32_t address,
                       const void *input, size_t bytes)
{
    return ui && address && input && bytes &&
           ui->callbacks.write_guest(ui->callbacks.context, address, input, bytes);
}

static int guest_string(ac1_window_win32 *ui, uint32_t address,
                        char *output, size_t capacity)
{
    if (!address || !output || capacity < 2) return 0;
    for (size_t i = 0; i + 1 < capacity; ++i) {
        if (!guest_read(ui, address + (uint32_t)i, &output[i], 1)) return 0;
        if (!output[i]) return 1;
    }
    output[capacity - 1] = '\0';
    return 0;
}

static int ansi_to_wide(const char *source, wchar_t *output, size_t capacity)
{
    if (!source || !output || !capacity || capacity > INT_MAX) return 0;
    int converted = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1,
                                         output, (int)capacity);
    return converted > 0;
}

static uint32_t current_guest_thread(ac1_window_win32 *ui)
{
    return ui->callbacks.current_guest_thread_id
        ? ui->callbacks.current_guest_thread_id(ui->callbacks.context) : 0u;
}

static ac1_window_record *record_from_native(HWND hwnd)
{
    return (ac1_window_record *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static ac1_window_record *record_from_guest(ac1_window_win32 *ui, uint32_t hwnd)
{
    if (!ui || !hwnd) return NULL;
    for (unsigned i = 0; i < AC1_WINDOW_MAX_WINDOWS; ++i)
        if (ui->windows[i].token == hwnd)
            return &ui->windows[i];
    return NULL;
}

static uint32_t guest_from_native(ac1_window_win32 *ui, HWND hwnd)
{
    if (!ui || !hwnd) return 0;
    for (unsigned i = 0; i < AC1_WINDOW_MAX_WINDOWS; ++i)
        if (ui->windows[i].native == hwnd) return ui->windows[i].token;
    return 0;
}

static void remove_message(ac1_window_win32 *ui, unsigned index)
{
    if (index + 1 < ui->message_count)
        memmove(&ui->messages[index], &ui->messages[index + 1],
                (ui->message_count - index - 1) * sizeof(ui->messages[0]));
    --ui->message_count;
}

static void remove_window_messages(ac1_window_win32 *ui, uint32_t hwnd)
{
    for (unsigned i = 0; i < ui->message_count;) {
        if (ui->messages[i].message.hwnd == hwnd) remove_message(ui, i);
        else ++i;
    }
}

static int queue_message(ac1_window_win32 *ui, ac1_window_record *record,
                         HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                         LRESULT native_default_result)
{
    (void)hwnd;
    if (!ui) return 0;
    if (ui->message_count == AC1_WINDOW_MAX_MESSAGES) {
        ui->queue_overflow = 1;
        return 0;
    }
    ac1_queued_message *queued = &ui->messages[ui->message_count++];
    memset(queued, 0, sizeof(*queued));
    queued->message.hwnd = record ? record->token : 0u;
    queued->message.message = message;
    queued->message.wparam = (uint32_t)wparam;
    queued->message.lparam = (uint32_t)lparam;
    if (message == WM_ACTIVATE && lparam)
        queued->message.lparam = guest_from_native(ui, (HWND)lparam);
    queued->message.time = GetMessageTime();
    const DWORD position = GetMessagePos();
    queued->message.x = (int16_t)LOWORD(position);
    queued->message.y = (int16_t)HIWORD(position);
    queued->guest_thread_id = record ? record->guest_thread_id : current_guest_thread(ui);
    queued->native_default_result = native_default_result;
    return 1;
}

static int guest_visible_message(UINT message)
{
    return message == WM_ACTIVATE || message == WM_CLOSE ||
           message == WM_SYSCOMMAND || message == WM_KEYUP ||
           message == WM_CHAR || message == WM_SYSCHAR ||
           message == WM_MOUSEMOVE || message == WM_LBUTTONDBLCLK ||
           message == WM_PAINT;
}

/* Native WndProc never enters lifted code. It performs host default behavior
 * and records only messages for the guest pump. */
static LRESULT CALLBACK ac1_native_wndproc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam)
{
    ac1_window_record *record = record_from_native(hwnd);
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        record = create ? (ac1_window_record *)create->lpCreateParams : NULL;
        if (!record || !record->owner) return FALSE;
        record->native = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)record);
        return TRUE;
    }
    if (message == WM_CREATE) return 0;
    if (!record || !record->owner) return DefWindowProcW(hwnd, message, wparam, lparam);
    if (message == WM_CLOSE) {
        (void)queue_message(record->owner, record, hwnd, message, wparam,
                            lparam, 0);
        return 0; /* Original guest WndProc decides when to DestroyWindow. */
    }
    if (!guest_visible_message(message)) {
        LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
        if (message == WM_NCDESTROY)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return result;
    }
    /* Queue before default processing so nested host messages preserve order. */
    unsigned queued_at = record->owner->message_count;
    (void)queue_message(record->owner, record, hwnd, message, wparam, lparam, 0);
    LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
    if (queued_at < record->owner->message_count)
        record->owner->messages[queued_at].native_default_result = result;
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return result;
}

static void return_api(CPU *cpu, uint32_t argument_bytes)
{
    cpu->eip = rd32(cpu->esp);
    cpu->esp += 4u + argument_bytes;
}

static void fail_invalid(CPU *cpu, uint32_t arguments, uint32_t *last_error,
                         DWORD win32_error, char *error, size_t error_capacity,
                         const char *operation)
{
    cpu->eax = 0;
    if (last_error) *last_error = win32_error;
    return_api(cpu, arguments);
    if (error && error_capacity)
        (void)snprintf(error, error_capacity, "%s failed (Win32 error %lu)",
                       operation, (unsigned long)win32_error);
}

static ac1_window_class *class_by_name(ac1_window_win32 *ui,
                                       uint32_t class_value,
                                       char *name, size_t name_capacity)
{
    if (class_value < 0x10000u) {
        for (unsigned i = 0; i < ui->class_count; ++i)
            if (ui->classes[i].atom == class_value) return &ui->classes[i];
        return NULL;
    }
    if (!guest_string(ui, class_value, name, name_capacity)) return NULL;
    for (unsigned i = 0; i < ui->class_count; ++i) {
        if (!_stricmp(ui->classes[i].guest_name, name)) return &ui->classes[i];
    }
    return NULL;
}

static int invoke_guest(ac1_window_win32 *ui, CPU *cpu,
                        ac1_window_record *record, uint32_t message,
                        uint32_t wparam, uint32_t lparam, uint32_t *result)
{
    if (!ui->callbacks.invoke_wndproc || !record ||
        record->class_index >= ui->class_count) return 0;
    ac1_window_class *klass = &ui->classes[record->class_index];
    return ui->callbacks.invoke_wndproc(ui->callbacks.context, cpu,
        klass->procedure, record->token, message, wparam, lparam, result);
}

static int call_guest_with_default(ac1_window_win32 *ui, CPU *cpu,
                                   ac1_window_record *record, uint32_t message,
                                   uint32_t wparam, uint32_t lparam,
                                   LRESULT default_result, uint32_t *result)
{
    uint32_t saved_hwnd = ui->active_default_hwnd;
    uint32_t saved_msg = ui->active_default_message;
    uint32_t saved_wp = ui->active_default_wparam;
    uint32_t saved_lp = ui->active_default_lparam;
    LRESULT saved_result = ui->active_default_result;
    int saved_valid = ui->active_default_valid;
    ++ui->active_default_depth;
    ui->active_default_hwnd = record ? record->token : 0;
    ui->active_default_message = message;
    ui->active_default_wparam = wparam;
    ui->active_default_lparam = lparam;
    ui->active_default_result = default_result;
    ui->active_default_valid = 1;
    int ok = invoke_guest(ui, cpu, record, message, wparam, lparam, result);
    ui->active_default_hwnd = saved_hwnd;
    ui->active_default_message = saved_msg;
    ui->active_default_wparam = saved_wp;
    ui->active_default_lparam = saved_lp;
    ui->active_default_result = saved_result;
    ui->active_default_valid = saved_valid;
    --ui->active_default_depth;
    return ok;
}

static int pump_host_messages(ac1_window_win32 *ui, uint32_t guest_thread_id,
                              char *error, size_t error_capacity)
{
    if (ui->queue_overflow) {
        set_error(error, error_capacity, "guest window message queue overflowed");
        return 0;
    }
    const DWORD host_thread = GetCurrentThreadId();
    for (unsigned i = 0; i < AC1_WINDOW_MAX_WINDOWS; ++i) {
        ac1_window_record *record = &ui->windows[i];
        if (!record->native || record->guest_thread_id != guest_thread_id ||
            GetWindowThreadProcessId(record->native, NULL) != host_thread) continue;
        MSG message;
        unsigned drained = 0;
        while (drained < 256 && PeekMessageW(&message, record->native, 0, 0, PM_REMOVE)) {
            ++drained;
            if (message.message == WM_QUIT) {
                (void)queue_message(ui, NULL, NULL, WM_QUIT, message.wParam, 0, 0);
                break;
            }
            (void)DispatchMessageW(&message);
        }
        if (ui->queue_overflow) {
            set_error(error, error_capacity, "guest window message queue overflowed");
            return 0;
        }
        if (drained == 256) {
            set_error(error, error_capacity,
                      "native host queue exceeded the per-window drain bound");
            return 0;
        }
    }
    return 1;
}

ac1_window_win32 *ac1_window_win32_create(
    const ac1_window_win32_callbacks *callbacks,
    char *error, size_t error_capacity)
{
    if (!callbacks || !callbacks->read_guest || !callbacks->write_guest ||
        !callbacks->allocate_guest || !callbacks->free_guest ||
        !callbacks->invoke_wndproc) {
        set_error(error, error_capacity, "window bridge callbacks are incomplete");
        return NULL;
    }
    ac1_window_win32 *ui = (ac1_window_win32 *)calloc(1, sizeof(*ui));
    if (!ui) {
        set_error(error, error_capacity, "window bridge allocation failed");
        return NULL;
    }
    ui->callbacks = *callbacks;
    ui->next_token = AC1_WINDOW_TOKEN_BASE;
    set_error(error, error_capacity, "");
    return ui;
}

void ac1_window_win32_destroy(ac1_window_win32 *ui)
{
    if (!ui) return;
    for (unsigned i = 0; i < AC1_WINDOW_MAX_WINDOWS; ++i)
        if (ui->windows[i].native) (void)DestroyWindow(ui->windows[i].native);
    for (unsigned i = 0; i < ui->class_count; ++i)
        if (ui->classes[i].registered)
            (void)UnregisterClassW(ui->classes[i].native_name, GetModuleHandleW(NULL));
    while (ui->show_cursor_balance > 0) {
        (void)ShowCursor(FALSE);
        --ui->show_cursor_balance;
    }
    while (ui->show_cursor_balance < 0) {
        (void)ShowCursor(TRUE);
        ++ui->show_cursor_balance;
    }
    free(ui);
}

uintptr_t ac1_window_win32_resolve_hwnd(ac1_window_win32 *ui,
                                        uint32_t guest_hwnd)
{
    ac1_window_record *record = record_from_guest(ui, guest_hwnd);
    return record && record->native ? (uintptr_t)record->native : 0;
}

int ac1_window_win32_call(ac1_window_win32 *ui, CPU *cpu,
    const char *name, uint32_t *last_error,
    char *error, size_t error_capacity)
{
    if (!ui || !cpu || !name) {
        set_error(error, error_capacity, "window bridge has invalid call state");
        return AC1_WINDOW_WIN32_ERROR;
    }
    uint32_t frame[13] = {0};
    if (!guest_read(ui, cpu->esp, frame, sizeof(uint32_t))) {
        set_error(error, error_capacity, "USER32 call return address is unmapped");
        return AC1_WINDOW_WIN32_ERROR;
    }
    const uint32_t tid = current_guest_thread(ui);

    if (!strcmp(name, "LoadCursorA")) {
        uint32_t args[2];
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args)))
            goto invalid_two;
        if (args[0] || args[1] != 0x7f00u) {
            set_error(error, error_capacity, "LoadCursorA supports only NULL/IDC_ARROW");
            return AC1_WINDOW_WIN32_UNSUPPORTED;
        }
        cpu->eax = AC1_CURSOR_TOKEN;
        return_api(cpu, 8u);
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "RegisterClassExA")) {
        uint32_t class_address;
        ac1_guest_wndclassex32 guest_class;
        char class_name[256];
        wchar_t wide_name[256];
        if (!guest_read(ui, cpu->esp + 4u, &class_address, 4u) ||
            !guest_read(ui, class_address, &guest_class, sizeof(guest_class)) ||
            guest_class.cb_size != sizeof(guest_class) || !guest_class.wndproc ||
            guest_class.class_extra != 0 || guest_class.window_extra != 0 ||
            !guest_string(ui, guest_class.class_name, class_name, sizeof(class_name)) ||
            !ansi_to_wide(class_name, wide_name, 256))
            goto invalid_one;
        if (ui->class_count == AC1_WINDOW_MAX_CLASSES) {
            fail_invalid(cpu, 4u, last_error, ERROR_NOT_ENOUGH_MEMORY,
                         error, error_capacity, "RegisterClassExA");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        for (unsigned i = 0; i < ui->class_count; ++i)
            if (!_stricmp(ui->classes[i].guest_name, class_name)) {
                fail_invalid(cpu, 4u, last_error, ERROR_CLASS_ALREADY_EXISTS,
                             error, error_capacity, "RegisterClassExA");
                return AC1_WINDOW_WIN32_HANDLED;
            }
        unsigned index = ui->class_count;
        ac1_window_class *klass = &ui->classes[index];
        memset(klass, 0, sizeof(*klass));
        (void)snprintf(klass->guest_name, sizeof(klass->guest_name), "%s", class_name);
        klass->procedure = guest_class.wndproc;
        klass->style = guest_class.style;
        klass->instance = guest_class.instance;
        klass->cursor = guest_class.cursor == AC1_CURSOR_TOKEN
            ? LoadCursorW(NULL, MAKEINTRESOURCEW(32512)) : NULL;
        (void)swprintf(klass->native_name,
                       sizeof(klass->native_name) / sizeof(klass->native_name[0]),
                       L"AC1Window_%p_%u", (void *)ui, index);
        WNDCLASSEXW native_class;
        memset(&native_class, 0, sizeof(native_class));
        native_class.cbSize = sizeof(native_class);
        native_class.style = guest_class.style;
        native_class.lpfnWndProc = ac1_native_wndproc;
        native_class.cbClsExtra = guest_class.class_extra;
        native_class.cbWndExtra = guest_class.window_extra;
        native_class.hInstance = GetModuleHandleW(NULL);
        native_class.hCursor = klass->cursor;
        native_class.lpszClassName = klass->native_name;
        ATOM atom = RegisterClassExW(&native_class);
        if (!atom) {
            DWORD code = GetLastError();
            fail_invalid(cpu, 4u, last_error, code, error, error_capacity,
                         "RegisterClassExW");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        klass->atom = atom;
        klass->registered = 1;
        ++ui->class_count;
        cpu->eax = atom;
        if (last_error) *last_error = 0;
        return_api(cpu, 4u);
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "CreateWindowExA")) {
        uint32_t args[12];
        char class_name[256], title[256] = "";
        wchar_t wide_title[256];
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args)))
            goto invalid_twelve;
        ac1_window_class *klass = class_by_name(ui, args[1], class_name,
                                                sizeof(class_name));
        if (!klass || args[0] || args[8] || args[9] || args[11] ||
            (args[2] && !guest_string(ui, args[2], title, sizeof(title))) ||
            !ansi_to_wide(title, wide_title, 256) ||
            (int32_t)args[6] < 0 || (int32_t)args[7] < 0 ||
            args[6] > 16384u || args[7] > 16384u) goto invalid_twelve;
        unsigned slot;
        for (slot = 0; slot < AC1_WINDOW_MAX_WINDOWS; ++slot)
            if (!ui->windows[slot].token) break;
        if (slot == AC1_WINDOW_MAX_WINDOWS) {
            fail_invalid(cpu, 48u, last_error, ERROR_NOT_ENOUGH_MEMORY,
                         error, error_capacity, "CreateWindowExA");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        ac1_window_record *record = &ui->windows[slot];
        memset(record, 0, sizeof(*record));
        record->owner = ui;
        record->token = ui->next_token;
        ui->next_token += 4u;
        record->class_index = (uint32_t)(klass - ui->classes);
        record->guest_thread_id = tid;
        record->style = args[3];
        record->ex_style = args[0];
        record->guest_instance = args[10];
        record->guest_param = args[11];
        record->x = (int32_t)args[4];
        record->y = (int32_t)args[5];
        record->width = (int32_t)args[6];
        record->height = (int32_t)args[7];
        HWND hwnd = CreateWindowExW(args[0], klass->native_name, wide_title,
            args[3], (int)record->x, (int)record->y, record->width,
            record->height, NULL, NULL, GetModuleHandleW(NULL), record);
        if (!hwnd) {
            memset(record, 0, sizeof(*record));
            fail_invalid(cpu, 48u, last_error, GetLastError(), error,
                         error_capacity, "CreateWindowExW");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        record->native = hwnd;
        ++ui->window_count;

        uint32_t createstruct_address = 0;
        if (!ui->callbacks.allocate_guest(ui->callbacks.context,
                                          sizeof(ac1_guest_createstruct32),
                                          &createstruct_address) ||
            !createstruct_address) {
            (void)DestroyWindow(hwnd);
            memset(record, 0, sizeof(*record));
            --ui->window_count;
            fail_invalid(cpu, 48u, last_error, ERROR_NOT_ENOUGH_MEMORY,
                         error, error_capacity, "CreateWindowExA CREATESTRUCT");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        ac1_guest_createstruct32 create = {
            args[11], args[10], args[9], args[8], (int32_t)args[7],
            (int32_t)args[6], (int32_t)args[5], (int32_t)args[4],
            (int32_t)args[3], args[2], args[1], args[0]
        };
        uint32_t callback_result = 0;
        int ok = guest_write(ui, createstruct_address, &create, sizeof(create)) &&
            call_guest_with_default(ui, cpu, record, WM_NCCREATE, 0,
                                    createstruct_address, TRUE, &callback_result) &&
            callback_result != 0;
        if (ok) {
            callback_result = 0;
            ok = call_guest_with_default(ui, cpu, record, WM_CREATE, 0,
                createstruct_address, 0, &callback_result) &&
                (int32_t)callback_result != -1;
        }
        ui->callbacks.free_guest(ui->callbacks.context, createstruct_address);
        if (!ok) {
            (void)DestroyWindow(hwnd);
            record->native = NULL;
            --ui->window_count;
            fail_invalid(cpu, 48u, last_error, ERROR_CANCELLED,
                         error, error_capacity, "guest WndProc rejected window creation");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        cpu->eax = record->token;
        if (last_error) *last_error = 0;
        return_api(cpu, 48u);
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "ShowWindow")) {
        uint32_t args[2];
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args))) goto invalid_two;
        ac1_window_record *record = record_from_guest(ui, args[0]);
        if (!record || !record->native) goto invalid_two;
        cpu->eax = (uint32_t)ShowWindow(record->native, (int)args[1]);
        return_api(cpu, 8u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "UpdateWindow")) {
        uint32_t hwnd;
        if (!guest_read(ui, cpu->esp + 4u, &hwnd, 4u)) goto invalid_one;
        ac1_window_record *record = record_from_guest(ui, hwnd);
        if (!record || !record->native) goto invalid_one;
        cpu->eax = (uint32_t)UpdateWindow(record->native);
        return_api(cpu, 4u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "PeekMessageA")) {
        uint32_t args[5];
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args)) || args[4] > 1u)
            goto invalid_five;
        const uint32_t thread_id = tid;
        if (!pump_host_messages(ui, thread_id, error, error_capacity))
            return AC1_WINDOW_WIN32_ERROR;
        int found = 0;
        for (unsigned i = 0; i < ui->message_count; ++i) {
            ac1_queued_message *queued = &ui->messages[i];
            ac1_guest_msg32 *msg = &queued->message;
            if (queued->removed) continue;
            if (queued->guest_thread_id != thread_id) continue;
            if (msg->message != WM_QUIT && args[1] && msg->hwnd != args[1]) continue;
            if (msg->message != WM_QUIT && (args[2] || args[3]) &&
                (msg->message < args[2] || (args[3] && msg->message > args[3]))) continue;
            if (args[0] && !guest_write(ui, args[0], msg, AC1_GUEST_MSG_BYTES))
                goto invalid_five;
            if (args[4] == PM_REMOVE) {
                if (msg->message == WM_QUIT) remove_message(ui, i);
                else queued->removed = 1;
            }
            found = 1;
            break;
        }
        cpu->eax = found ? 1u : 0u;
        return_api(cpu, 20u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "TranslateMessage")) {
        uint32_t address;
        ac1_guest_msg32 guest_msg;
        if (!guest_read(ui, cpu->esp + 4u, &address, 4u) ||
            !guest_read(ui, address, &guest_msg, sizeof(guest_msg))) goto invalid_one;
        HWND hwnd = guest_msg.hwnd ? (HWND)ac1_window_win32_resolve_hwnd(ui, guest_msg.hwnd) : NULL;
        if (guest_msg.hwnd && !hwnd) goto invalid_one;
        MSG native_msg;
        memset(&native_msg, 0, sizeof(native_msg));
        native_msg.hwnd = hwnd;
        native_msg.message = guest_msg.message;
        native_msg.wParam = guest_msg.wparam;
        native_msg.lParam = (LPARAM)guest_msg.lparam;
        native_msg.time = guest_msg.time;
        native_msg.pt.x = guest_msg.x;
        native_msg.pt.y = guest_msg.y;
        cpu->eax = (uint32_t)TranslateMessage(&native_msg);
        return_api(cpu, 4u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "DispatchMessageA")) {
        uint32_t address;
        ac1_guest_msg32 msg;
        if (!guest_read(ui, cpu->esp + 4u, &address, 4u) ||
            !guest_read(ui, address, &msg, sizeof(msg))) goto invalid_one;
        if (msg.message == WM_QUIT) {
            cpu->eax = 0;
            return_api(cpu, 4u);
            set_error(error, error_capacity, "");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        ac1_window_record *record = record_from_guest(ui, msg.hwnd);
        if (!record || !record->native) goto invalid_one;
        LRESULT default_result = 0;
        for (unsigned i = 0; i < ui->message_count; ++i) {
            ac1_queued_message *queued = &ui->messages[i];
            if (queued->removed && queued->guest_thread_id == tid &&
                !memcmp(&queued->message, &msg, sizeof(msg))) {
                default_result = queued->native_default_result;
                remove_message(ui, i);
                break;
            }
        }
        uint32_t callback_result = 0;
        if (!call_guest_with_default(ui, cpu, record, msg.message, msg.wparam,
                                     msg.lparam, default_result, &callback_result)) {
            set_error(error, error_capacity, "guest WndProc callback could not be invoked");
            return AC1_WINDOW_WIN32_ERROR;
        }
        cpu->eax = callback_result;
        return_api(cpu, 4u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "DefWindowProcA")) {
        uint32_t args[4];
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args))) goto invalid_four;
        ac1_window_record *record = record_from_guest(ui, args[0]);
        if (!record) goto invalid_four;
        if (ui->active_default_valid && ui->active_default_hwnd == args[0] &&
            ui->active_default_message == args[1] &&
            ui->active_default_wparam == args[2] &&
            ui->active_default_lparam == args[3]) {
            cpu->eax = (uint32_t)ui->active_default_result;
        } else if (args[1] == WM_NCCREATE) cpu->eax = TRUE;
        else if (args[1] == WM_CREATE || args[1] == WM_DESTROY ||
                 args[1] == WM_NCDESTROY || args[1] == WM_CLOSE) cpu->eax = 0;
        else {
            cpu->eax = (uint32_t)DefWindowProcA(record->native, args[1],
                args[2], (LPARAM)args[3]);
        }
        return_api(cpu, 16u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "DestroyWindow")) {
        uint32_t hwnd;
        if (!guest_read(ui, cpu->esp + 4u, &hwnd, 4u)) goto invalid_one;
        ac1_window_record *record = record_from_guest(ui, hwnd);
        if (!record || !record->native || record->destroying) goto invalid_one;
        if (record->guest_thread_id != tid) {
            fail_invalid(cpu, 4u, last_error, ERROR_ACCESS_DENIED,
                         error, error_capacity, "DestroyWindow");
            return AC1_WINDOW_WIN32_HANDLED;
        }
        record->destroying = 1;
        uint32_t ignored = 0;
        if (!call_guest_with_default(ui, cpu, record, WM_DESTROY, 0, 0, 0, &ignored) ||
            !call_guest_with_default(ui, cpu, record, WM_NCDESTROY, 0, 0, 0, &ignored)) {
            record->destroying = 0;
            set_error(error, error_capacity, "guest WndProc destruction callback failed");
            return AC1_WINDOW_WIN32_ERROR;
        }
        BOOL result = DestroyWindow(record->native);
        if (result) {
            record->native = NULL;
            record->destroying = 0;
            if (ui->window_count) --ui->window_count;
            remove_window_messages(ui, hwnd);
        }
        cpu->eax = (uint32_t)result;
        return_api(cpu, 4u);
        if (last_error) *last_error = result ? 0u : GetLastError();
        set_error(error, error_capacity, result ? "" : "DestroyWindow failed");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "PostQuitMessage")) {
        uint32_t code;
        if (!guest_read(ui, cpu->esp + 4u, &code, 4u)) goto invalid_one;
        (void)queue_message(ui, NULL, NULL, WM_QUIT, code, 0, 0);
        return_api(cpu, 4u);
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "GetDesktopWindow")) {
        cpu->eax = AC1_DESKTOP_TOKEN;
        return_api(cpu, 0u);
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "IsWindow") || !strcmp(name, "IsWindowVisible") ||
        !strcmp(name, "IsIconic")) {
        uint32_t hwnd;
        if (!guest_read(ui, cpu->esp + 4u, &hwnd, 4u)) goto invalid_one;
        ac1_window_record *record = record_from_guest(ui, hwnd);
        BOOL result = FALSE;
        if (hwnd == AC1_DESKTOP_TOKEN && !strcmp(name, "IsWindow")) result = TRUE;
        else if (record && record->native) {
            if (!strcmp(name, "IsWindow")) result = IsWindow(record->native);
            else if (!strcmp(name, "IsWindowVisible")) result = IsWindowVisible(record->native);
            else result = IsIconic(record->native);
        }
        cpu->eax = (uint32_t)result;
        return_api(cpu, 4u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "ShowCursor")) {
        uint32_t show;
        if (!guest_read(ui, cpu->esp + 4u, &show, 4u)) goto invalid_one;
        cpu->eax = (uint32_t)ShowCursor(show != 0);
        ui->show_cursor_balance += show ? 1 : -1;
        return_api(cpu, 4u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "GetClientRect") || !strcmp(name, "GetWindowRect")) {
        uint32_t args[2];
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args))) goto invalid_two;
        ac1_window_record *record = record_from_guest(ui, args[0]);
        RECT rect;
        BOOL result = FALSE;
        if (args[1] && !strcmp(name, "GetWindowRect") &&
            args[0] == AC1_DESKTOP_TOKEN) {
            result = GetWindowRect(GetDesktopWindow(), &rect);
        } else if (record && record->native && args[1]) {
            result = !strcmp(name, "GetClientRect")
                ? GetClientRect(record->native, &rect)
                : GetWindowRect(record->native, &rect);
        }
        if (result && !guest_write(ui, args[1], &rect, sizeof(rect))) goto invalid_two;
        cpu->eax = result ? 1u : 0u;
        return_api(cpu, 8u);
        if (last_error) *last_error = result ? 0u : ERROR_INVALID_WINDOW_HANDLE;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "AdjustWindowRect")) {
        uint32_t args[3];
        RECT rect;
        if (!guest_read(ui, cpu->esp + 4u, args, sizeof(args)) ||
            !guest_read(ui, args[0], &rect, sizeof(rect))) goto invalid_three;
        BOOL result = AdjustWindowRect(&rect, args[1], args[2]);
        if (result && !guest_write(ui, args[0], &rect, sizeof(rect))) goto invalid_three;
        cpu->eax = (uint32_t)result;
        return_api(cpu, 12u);
        if (last_error) *last_error = result ? 0u : GetLastError();
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "GetSystemMetrics")) {
        uint32_t index;
        if (!guest_read(ui, cpu->esp + 4u, &index, 4u)) goto invalid_one;
        cpu->eax = (uint32_t)GetSystemMetrics((int)index);
        return_api(cpu, 4u);
        if (last_error) *last_error = 0;
        set_error(error, error_capacity, "");
        return AC1_WINDOW_WIN32_HANDLED;
    }

    if (!strcmp(name, "GetDC") || !strcmp(name, "ReleaseDC")) {
        set_error(error, error_capacity, "GDI DCs are outside the D3D9 window bridge subset");
        return AC1_WINDOW_WIN32_UNSUPPORTED;
    }

    set_error(error, error_capacity, "USER32 API is outside the verified AC1 window subset");
    return AC1_WINDOW_WIN32_UNSUPPORTED;

invalid_one:
    fail_invalid(cpu, 4u, last_error, ERROR_INVALID_PARAMETER,
                 error, error_capacity, name);
    return AC1_WINDOW_WIN32_HANDLED;
invalid_two:
    fail_invalid(cpu, 8u, last_error, ERROR_INVALID_PARAMETER,
                 error, error_capacity, name);
    return AC1_WINDOW_WIN32_HANDLED;
invalid_three:
    fail_invalid(cpu, 12u, last_error, ERROR_INVALID_PARAMETER,
                 error, error_capacity, name);
    return AC1_WINDOW_WIN32_HANDLED;
invalid_four:
    fail_invalid(cpu, 16u, last_error, ERROR_INVALID_PARAMETER,
                 error, error_capacity, name);
    return AC1_WINDOW_WIN32_HANDLED;
invalid_five:
    fail_invalid(cpu, 20u, last_error, ERROR_INVALID_PARAMETER,
                 error, error_capacity, name);
    return AC1_WINDOW_WIN32_HANDLED;
invalid_twelve:
    fail_invalid(cpu, 48u, last_error, ERROR_INVALID_PARAMETER,
                 error, error_capacity, name);
    return AC1_WINDOW_WIN32_HANDLED;
}
