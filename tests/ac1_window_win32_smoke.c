#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac1_window_win32.h"
#include "guest_memory.h"

#define GUEST_BASE 0x00001000u
#define GUEST_BYTES 0x00100000u
#define GUEST_STACK 0x00009000u
#define GUEST_CLASS 0x00011000u
#define GUEST_CLASS_NAME 0x00012000u
#define GUEST_TITLE 0x00012100u
#define GUEST_MSG 0x00013000u
#define GUEST_NESTED_STACK 0x0000A000u
#define RETURN_SENTINEL 0xfffff000u

typedef struct smoke_context {
    ac1_window_win32 *window;
    uint32_t next_allocation;
    unsigned callbacks;
    unsigned creation_callbacks;
    unsigned close_callbacks;
    unsigned destruction_callbacks;
    int inside_native_pump;
    int failed;
} smoke_context;

typedef struct smoke_msg32 {
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t x, y;
} smoke_msg32;

static uint8_t guest_bytes[GUEST_BYTES];

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "window smoke failed at %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; \
} } while (0)

static int memory_read(void *context, uint32_t address, void *output, size_t bytes)
{
    (void)context;
    return guest_memory_read(address, output, bytes);
}

static int memory_write(void *context, uint32_t address,
                        const void *input, size_t bytes)
{
    (void)context;
    void *target = guest_memory_try_ptr(address, bytes);
    if (!target || !input) return 0;
    memcpy(target, input, bytes);
    return 1;
}

static int memory_allocate(void *opaque, size_t bytes, uint32_t *address)
{
    smoke_context *context = (smoke_context *)opaque;
    uint32_t aligned = (context->next_allocation + 15u) & ~15u;
    if (!address || bytes > UINT32_MAX ||
        !guest_memory_try_ptr(aligned, bytes)) return 0;
    memset(guest_ptr(aligned, bytes), 0, bytes);
    *address = aligned;
    context->next_allocation = aligned + (uint32_t)bytes;
    return 1;
}

static void memory_free(void *context, uint32_t address)
{
    (void)context;
    (void)address;
}

static uint32_t guest_thread_id(void *context)
{
    (void)context;
    return 0x1000u;
}

static int invoke_guest_wndproc(void *opaque, CPU *cpu, uint32_t procedure,
    uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam,
    uint32_t *result)
{
    smoke_context *context = (smoke_context *)opaque;
    (void)cpu;
    (void)wparam;
    (void)lparam;
    if (context->inside_native_pump) {
        context->failed = 1;
        fprintf(stderr, "guest WndProc invoked during native Peek/Dispatch pump\n");
        return 0;
    }
    if (procedure != 0x004013e3u || !hwnd || !result) return 0;
    ++context->callbacks;
    if (message == WM_NCCREATE) {
        ++context->creation_callbacks;
        if (!ac1_window_win32_resolve_hwnd(context->window, hwnd)) {
            context->failed = 1;
            return 0;
        }
        *result = TRUE;
    } else if (message == WM_CREATE) {
        ++context->creation_callbacks;
        *result = 0;
    } else if (message == WM_CLOSE) {
        ++context->close_callbacks;
        *result = 0;
        CPU saved_cpu = *cpu;
        uint8_t saved_stack[64];
        memcpy(saved_stack, guest_ptr(GUEST_STACK, sizeof(saved_stack)), sizeof(saved_stack));
        uint32_t destroy_arg = hwnd;
        CPU nested_cpu;
        memset(&nested_cpu, 0, sizeof(nested_cpu));
        nested_cpu.esp = GUEST_NESTED_STACK;
        uint32_t sentinel = RETURN_SENTINEL;
        memcpy(guest_ptr(nested_cpu.esp, 4), &sentinel, 4);
        memcpy(guest_ptr(nested_cpu.esp + 4u, 4), &destroy_arg, 4);
        char error[128];
        uint32_t last_error = 0;
        int status = ac1_window_win32_call(context->window, &nested_cpu,
            "DestroyWindow", &last_error, error, sizeof(error));
        if (status != AC1_WINDOW_WIN32_HANDLED || !nested_cpu.eax) {
            context->failed = 1;
        } else {
            nested_cpu.esp = GUEST_NESTED_STACK;
            memcpy(guest_ptr(nested_cpu.esp, 4), &sentinel, 4);
            uint32_t quit_code = 0;
            memcpy(guest_ptr(nested_cpu.esp + 4u, 4), &quit_code, 4);
            status = ac1_window_win32_call(context->window, &nested_cpu,
                "PostQuitMessage", &last_error, error, sizeof(error));
            if (status != AC1_WINDOW_WIN32_HANDLED) context->failed = 1;
        }
        *cpu = saved_cpu;
        memcpy(guest_ptr(GUEST_STACK, sizeof(saved_stack)), saved_stack, sizeof(saved_stack));
    } else if (message == WM_DESTROY || message == WM_NCDESTROY) {
        ++context->destruction_callbacks;
        *result = 0;
    } else {
        *result = 0;
    }
    return 1;
}

static int prepare_cpu(CPU *cpu, uint32_t argument_bytes,
                       const uint32_t *arguments, unsigned argument_count)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->esp = GUEST_STACK;
    const uint32_t ret = RETURN_SENTINEL;
    memcpy(guest_ptr(cpu->esp, 4), &ret, 4);
    if (argument_count)
        memcpy(guest_ptr(cpu->esp + 4u, argument_bytes), arguments, argument_bytes);
    return 1;
}

static int call_api(smoke_context *context, CPU *cpu, const char *name,
                    uint32_t argument_bytes, const uint32_t *arguments,
                    unsigned argument_count, int host_pump)
{
    char error[256];
    uint32_t last_error = 0;
    if (!prepare_cpu(cpu, argument_bytes, arguments, argument_count)) return 0;
    context->inside_native_pump = host_pump;
    int status = ac1_window_win32_call(context->window, cpu, name,
        &last_error, error, sizeof(error));
    context->inside_native_pump = 0;
    if (status != AC1_WINDOW_WIN32_HANDLED) {
        fprintf(stderr, "%s status=%d error=%s\n", name, status, error);
        return 0;
    }
    return cpu->eip == RETURN_SENTINEL &&
           cpu->esp == GUEST_STACK + 4u + argument_bytes;
}

static int register_guest_class(smoke_context *context, CPU *cpu)
{
    const char class_name[] = "ScimitarEngineWindowClass";
    memcpy(guest_ptr(GUEST_CLASS_NAME, sizeof(class_name)), class_name, sizeof(class_name));
    uint32_t args[] = {0u, 0x7f00u};
    if (!call_api(context, cpu, "LoadCursorA", 8, args, 2, 0) ||
        cpu->eax != 0xE5000004u) return 0;
    uint32_t klass[12] = {
        48u, 0x0bu, 0x004013e3u, 0u, 0u,
        0u, 0u, 0xE5000004u, 0u, 0u,
        GUEST_CLASS_NAME, 0u
    };
    memcpy(guest_ptr(GUEST_CLASS, sizeof(klass)), klass, sizeof(klass));
    uint32_t arg = GUEST_CLASS;
    if (!call_api(context, cpu, "RegisterClassExA", 4, &arg, 1, 0)) return 0;
    return cpu->eax != 0;
}

static int create_guest_window(smoke_context *context, CPU *cpu,
                               uint32_t *guest_hwnd)
{
    const char title[] = "AC1 host window smoke";
    memcpy(guest_ptr(GUEST_TITLE, sizeof(title)), title, sizeof(title));
    uint32_t args[12] = {
        0u, GUEST_CLASS_NAME, GUEST_TITLE, WS_POPUP,
        8u, 8u, 640u, 480u, 0u, 0u, 0x100000u, 0u
    };
    if (!call_api(context, cpu, "CreateWindowExA", 48, args, 12, 0)) return 0;
    *guest_hwnd = cpu->eax;
    return *guest_hwnd != 0 &&
           context->creation_callbacks == 2u &&
           ac1_window_win32_resolve_hwnd(context->window, *guest_hwnd) != 0;
}

static int dispatch_close_and_quit(smoke_context *context, CPU *cpu,
                                   uint32_t guest_hwnd)
{
    HWND native = (HWND)ac1_window_win32_resolve_hwnd(context->window, guest_hwnd);
    if (!native || !PostMessageW(native, WM_CLOSE, 0, 0)) return 0;
    uint32_t peek_args[5] = {GUEST_MSG, 0u, 0u, 0u, PM_REMOVE};
    if (!call_api(context, cpu, "PeekMessageA", 20, peek_args, 5, 1) ||
        cpu->eax != 1u) return 0;
    smoke_msg32 msg;
    memcpy(&msg, guest_ptr(GUEST_MSG, sizeof(msg)), sizeof(msg));
    if (msg.hwnd != guest_hwnd || msg.message != WM_CLOSE ||
        context->close_callbacks != 0u) return 0;

    uint32_t dispatch_arg = GUEST_MSG;
    if (!call_api(context, cpu, "DispatchMessageA", 4, &dispatch_arg, 1, 0) ||
        context->close_callbacks != 1u || context->failed) return 0;
    if (ac1_window_win32_resolve_hwnd(context->window, guest_hwnd) != 0 ||
        context->destruction_callbacks != 2u) return 0;

    uint32_t quit_peek[5] = {GUEST_MSG, 0u, 0u, 0u, PM_REMOVE};
    if (!call_api(context, cpu, "PeekMessageA", 20, quit_peek, 5, 0) ||
        cpu->eax != 1u) return 0;
    memcpy(&msg, guest_ptr(GUEST_MSG, sizeof(msg)), sizeof(msg));
    return msg.hwnd == 0u && msg.message == WM_QUIT && msg.wparam == 0u;
}

static int run_one_lifecycle(smoke_context *context, CPU *cpu, int exercise_close)
{
    uint32_t guest_hwnd;
    if (!register_guest_class(context, cpu) ||
        !create_guest_window(context, cpu, &guest_hwnd)) return 0;
    HWND native = (HWND)ac1_window_win32_resolve_hwnd(context->window, guest_hwnd);
    if (!native || !IsWindow(native) || IsWindowVisible(native)) return 0;

    RECT client;
    uint32_t args[] = {guest_hwnd, GUEST_MSG};
    if (!call_api(context, cpu, "GetClientRect", 8, args, 2, 0) || !cpu->eax)
        return 0;
    memcpy(&client, guest_ptr(GUEST_MSG, sizeof(client)), sizeof(client));
    if (client.right - client.left != 640 || client.bottom - client.top != 480)
        return 0;

    uint32_t cursor_arg = GUEST_MSG;
    if (!call_api(context, cpu, "GetCursorPos", 4, &cursor_arg, 1, 0) ||
        cpu->eax != TRUE) return 0;
    POINT screen_point, expected_client_point;
    memcpy(&screen_point, guest_ptr(GUEST_MSG, sizeof(screen_point)),
           sizeof(screen_point));
    expected_client_point = screen_point;
    if (!ScreenToClient(native, &expected_client_point)) return 0;
    uint32_t screen_to_client_args[] = {guest_hwnd, GUEST_MSG};
    if (!call_api(context, cpu, "ScreenToClient", 8,
                  screen_to_client_args, 2, 0) || cpu->eax != TRUE)
        return 0;
    POINT actual_client_point;
    memcpy(&actual_client_point, guest_ptr(GUEST_MSG, sizeof(actual_client_point)),
           sizeof(actual_client_point));
    if (actual_client_point.x != expected_client_point.x ||
        actual_client_point.y != expected_client_point.y)
        return 0;

    if (exercise_close) return dispatch_close_and_quit(context, cpu, guest_hwnd);

    uint32_t destroy_arg = guest_hwnd;
    if (!call_api(context, cpu, "DestroyWindow", 4, &destroy_arg, 1, 0) ||
        !cpu->eax || context->destruction_callbacks < 2u) return 0;
    return ac1_window_win32_resolve_hwnd(context->window, guest_hwnd) == 0;
}

int main(void)
{
    guest_memory_bind(guest_bytes, GUEST_BASE, sizeof(guest_bytes));
    smoke_context context;
    memset(&context, 0, sizeof(context));
    context.next_allocation = 0x00070000u;
    ac1_window_win32_callbacks callbacks = {
        &context, memory_read, memory_write, memory_allocate, memory_free,
        invoke_guest_wndproc, guest_thread_id
    };
    CPU cpu;
    char error[256];

    context.window = ac1_window_win32_create(&callbacks, error, sizeof(error));
    CHECK(context.window != NULL);
    CHECK(call_api(&context, &cpu, "GetDesktopWindow", 0, NULL, 0, 0));
    CHECK(ac1_window_win32_resolve_hwnd(context.window, cpu.eax) ==
          (uintptr_t)GetDesktopWindow());
    CHECK(run_one_lifecycle(&context, &cpu, 1));
    ac1_window_win32_destroy(context.window);
    context.window = NULL;

    memset(&context, 0, sizeof(context));
    context.next_allocation = 0x00070000u;
    callbacks.context = &context;
    context.window = ac1_window_win32_create(&callbacks, error, sizeof(error));
    CHECK(context.window != NULL);
    CHECK(run_one_lifecycle(&context, &cpu, 0));
    ac1_window_win32_destroy(context.window);
    guest_memory_unbind();
    puts("AC1 Win32 window smoke passed: real hidden HWND, guest queue/callback boundary, teardown/recreate");
    return 0;
}
