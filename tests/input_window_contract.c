/* Reuse the real hidden-window/guest-memory fixture, without invoking its main. */
#define main ac1_window_base_smoke_main
#include "ac1_window_win32_smoke.c"
#undef main
#include "ac1_input_script.h"

static unsigned key_messages, mouse_messages;
static int replay_callback(void *opaque, CPU *cpu, uint32_t procedure, uint32_t hwnd,
                           uint32_t message, uint32_t wp, uint32_t lp, uint32_t *result) {
    smoke_context *context = opaque;
    if (message == WM_KEYDOWN || message == WM_KEYUP) {
        ++key_messages;
        CPU nested;
        uint32_t address = 0x00014000;
        if (!call_api(context, &nested, "GetKeyboardState", 4, &address, 1, 0) || !nested.eax)
            return 0;
        uint8_t *keys = guest_ptr(address, 256);
        /* All four events were queued together: each must retain its own state. */
        if (key_messages == 1 && (wp != VK_SHIFT || keys[VK_SHIFT] != 0x80))
            return 0;
        if (key_messages == 2) {
            if (wp != 'A' || keys[VK_SHIFT] != 0x80 || keys['A'] != 0x80)
                return 0;
            uint32_t args[] = {'A', 0x1e, address, 0x00015000, 8, 0};
            if (!call_api(context, &nested, "ToUnicode", 24, args, 6, 0) || nested.eax != 1 ||
                *(uint16_t *)guest_ptr(0x00015000, 2) != 'A')
                return 0;
        }
        if (key_messages == 3 && (message != WM_KEYUP || keys['A'] || !keys[VK_SHIFT]))
            return 0;
        if (key_messages == 4 && (wp != VK_SHIFT || keys[VK_SHIFT]))
            return 0;
        if (key_messages > 4)
            return 0; /* Native physical B must be suppressed. */
    }
    if (message == WM_MOUSEMOVE) {
        ++mouse_messages;
        CPU nested;
        uint32_t address = 0x00016000, args[] = {hwnd, address};
        if (!call_api(context, &nested, "GetCursorPos", 4, &address, 1, 0) || !nested.eax ||
            !call_api(context, &nested, "ScreenToClient", 8, args, 2, 0) || !nested.eax)
            return 0;
        POINT *point = guest_ptr(address, 8);
        if (point->x != 55 || point->y != 66 || lp != 0x00420037)
            return 0;
    }
    return invoke_guest_wndproc(opaque, cpu, procedure, hwnd, message, wp, lp, result);
}
int main(void) {
    FILE *file = fopen("work/input_window_contract.txt", "wb");
    CHECK(file);
    fputs("at 0 key LSHIFT down\nat 0 key A down\nat 0 key A up\nat 0 key LSHIFT up\nat 0 mouse "
          "move 55 66\n",
          file);
    fclose(file);
    _putenv_s("AC1_INPUT_SCRIPT", "work/input_window_contract.txt");
    _putenv_s("AC1_INPUT_RECORD", "");
    guest_memory_bind(guest_bytes, GUEST_BASE, sizeof(guest_bytes));
    smoke_context context = {0};
    context.next_allocation = 0x00070000;
    ac1_window_win32_callbacks callbacks = {&context,        memory_read, memory_write,
                                            memory_allocate, memory_free, replay_callback,
                                            guest_thread_id};
    char error[512];
    CPU cpu;
    context.window = ac1_window_win32_create(&callbacks, error, sizeof(error));
    CHECK(context.window);
    uint32_t hwnd;
    CHECK(register_guest_class(&context, &cpu));
    CHECK(create_guest_window(&context, &cpu, &hwnd));
    HWND native = (HWND)ac1_window_win32_resolve_hwnd(context.window, hwnd);
    CHECK(PostMessageW(native, WM_KEYDOWN, 'B', 0x00300001));
    CHECK(PostMessageW(native, WM_MOUSEMOVE, 0, 0x00990088));
    uint32_t peek[] = {GUEST_MSG, hwnd, 0, 0, PM_REMOVE}, msg_arg = GUEST_MSG;
    unsigned drained = 0;
    while (1) {
        CHECK(call_api(&context, &cpu, "PeekMessageA", 20, peek, 5, 1));
        if (!cpu.eax)
            break;
        CHECK(++drained < 30);
        CHECK(call_api(&context, &cpu, "DispatchMessageA", 4, &msg_arg, 1, 0));
    }
    CHECK(key_messages == 4 && mouse_messages == 1);
    ac1_window_win32_destroy(context.window);
    guest_memory_unbind();
    CHECK(!ac1_input_replaying());
    _putenv_s("AC1_INPUT_SCRIPT", "");
    puts("input window contract passed: isolated physical messages, per-event VK state, Unicode "
         "text, virtual cursor");
    return 0;
}
