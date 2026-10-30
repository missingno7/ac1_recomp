/* Android has one SurfaceView. Keep x86 HWND/message/WndProc semantics here;
 * the real native-window lifetime and DXVK WSI handoff belong to JNI. */
#include "ac1_input_script.h"
#include "ac1_window_win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uintptr_t ac1_android_d3d9_guest_hwnd(void *, uint32_t);
extern void ac1_android_surface_size(int32_t *, int32_t *);
#define HWND_GAME 0xe5000001u
#define HWND_DESKTOP 0xe5000002u
struct ac1_window_win32 {
    ac1_window_win32_callbacks cb;
    uint32_t procedure, owner, style;
    int live, visible, cursor_count;
    int32_t width, height;
    uint32_t messages[16][7];
    ac1_input_message snapshots[16], last_snapshot;
    unsigned char has_snapshot[16];
    int last_has_snapshot;
    unsigned count;
};
static int rd(ac1_window_win32 *w, uint32_t a, void *v, size_t n) {
    return w->cb.read_guest(w->cb.context, a, v, n);
}
static int wr(ac1_window_win32 *w, uint32_t a, const void *v, size_t n) {
    return w->cb.write_guest(w->cb.context, a, v, n);
}
static int done(ac1_window_win32 *w, CPU *cpu, unsigned count, uint32_t result) {
    uint32_t ret;
    if (!rd(w, cpu->esp, &ret, 4))
        return -1;
    cpu->eax = result;
    cpu->esp += 4 + count * 4;
    cpu->eip = ret;
    return 1;
}
static void queue(ac1_window_win32 *w, uint32_t m, uint32_t wp, uint32_t lp) {
    if (w->count >= 16)
        return;
    uint32_t *p = w->messages[w->count++];
    memset(p, 0, 28);
    w->has_snapshot[w->count - 1] = 0;
    p[0] = HWND_GAME;
    p[1] = m;
    p[2] = wp;
    p[3] = lp;
}
ac1_window_win32 *ac1_window_win32_create(const ac1_window_win32_callbacks *cb, char *error,
                                          size_t cap) {
    if (!cb || !cb->read_guest || !cb->write_guest || !cb->invoke_wndproc || !cb->allocate_guest ||
        !cb->free_guest || !cb->current_guest_thread_id)
        return NULL;
    ac1_window_win32 *w = calloc(1, sizeof(*w));
    if (!w) {
        snprintf(error, cap, "Android window allocation failed");
        return NULL;
    }
    w->cb = *cb;
    ac1_android_surface_size(&w->width, &w->height);
    return w;
}
void ac1_window_win32_destroy(ac1_window_win32 *w) {
    ac1_input_session_shutdown();
    ac1_input_physical_reset();
    free(w);
}
uintptr_t ac1_window_win32_resolve_hwnd(ac1_window_win32 *w, uint32_t hwnd) {
    return w && ((w->live && hwnd == HWND_GAME) || hwnd == HWND_DESKTOP)
               ? ac1_android_d3d9_guest_hwnd(NULL, hwnd)
               : 0;
}
int ac1_window_win32_call(ac1_window_win32 *w, CPU *cpu, const char *name, uint32_t *last_error,
                          char *error, size_t cap) {
    uint32_t a[12] = {0};
    unsigned argc = 0;
    uint32_t result = 0;
    if (!ac1_input_session_init(error, cap))
        return -1;
#define IS(s) (!strcmp(name, s))
    if (IS("LoadCursorA") || IS("RegisterClassExA") || IS("ShowCursor") || IS("GetSystemMetrics") ||
        IS("PostQuitMessage") || IS("ClipCursor") || IS("GetCursorPos") || IS("GetKeyboardState"))
        argc = IS("LoadCursorA") ? 2 : 1;
    else if (IS("CreateWindowExA"))
        argc = 12;
    else if (IS("ShowWindow") || IS("GetClientRect") || IS("GetWindowRect") ||
             IS("ScreenToClient") || IS("ReleaseDC"))
        argc = 2;
    else if (IS("UpdateWindow") || IS("TranslateMessage") || IS("DispatchMessageA") ||
             IS("DestroyWindow") || IS("IsWindow") || IS("IsWindowVisible") || IS("IsIconic") ||
             IS("GetDC"))
        argc = 1;
    else if (IS("DefWindowProcA") || IS("EnumDisplayMonitors"))
        argc = 4;
    else if (IS("PeekMessageA"))
        argc = 5;
    else if (IS("AdjustWindowRect"))
        argc = 3;
    else if (IS("ToUnicode"))
        argc = 6;
    else if (IS("GetDesktopWindow"))
        argc = 0;
    else {
        snprintf(error, cap, "Android USER32 unsupported: %s", name);
        return 0;
    }
    if (argc && !rd(w, cpu->esp + 4, a, argc * 4))
        goto bad;
    if (IS("EnumDisplayMonitors")) {
        /* Single-monitor callback/clipping follows recomp-kit user32_vcl.cpp
         * (MIT). Handle 1 agrees with ports/dxvk_android/android_wsi.cpp. */
        if (a[0]) {
            snprintf(error, cap, "Android EnumDisplayMonitors: unsupported HDC=0x%08x", a[0]);
            return -1;
        }
        if (!a[2]) goto bad;
        int32_t rect[4] = {0, 0, w->width, w->height};
        if (a[1]) {
            int32_t clip[4];
            if (!rd(w, a[1], clip, sizeof(clip))) goto bad;
            for (unsigned i = 0; i < 4; ++i)
                if (i < 2 ? clip[i] > rect[i] : clip[i] < rect[i]) rect[i] = clip[i];
        }
        result = 1;
        if (rect[0] < rect[2] && rect[1] < rect[3]) {
            uint32_t address = 0, value = 0;
            if (!w->cb.allocate_guest(w->cb.context, sizeof(rect), &address)) goto bad;
            int ok = wr(w, address, rect, sizeof(rect)) &&
                w->cb.invoke_wndproc(w->cb.context, cpu, a[2], 1, 0, address, a[3], &value);
            w->cb.free_guest(w->cb.context, address);
            if (!ok) goto bad;
            result = value != 0;
        }
    } else if (IS("LoadCursorA")) {
        if (a[0] || a[1] < 32512 || a[1] > 32672)
            goto unsupported;
        result = 0xe5100000u + a[1];
    } else if (IS("RegisterClassExA")) {
        uint32_t c[12];
        if (!rd(w, a[0], c, 48) || c[0] != 48 || !c[2])
            goto bad;
        w->procedure = c[2];
        result = 1;
    } else if (IS("CreateWindowExA")) {
        if (w->live || !w->procedure || a[8] || a[9])
            goto unsupported;
        w->style = a[3];
        w->live = 1;
        w->owner = w->cb.current_guest_thread_id(w->cb.context);
        // CREATESTRUCTA is pack-4, temporary guest storage valid during WM_NCCREATE/WM_CREATE.
        uint32_t create[] = {a[11], a[10], a[9], a[8], a[7], a[6],
                             a[5],  a[4],  a[3], a[2], a[1], a[0]},
                 address = 0, value = 0;
        if (!w->cb.allocate_guest(w->cb.context, 48, &address))
            goto bad;
        if (!wr(w, address, create, 48)) {
            w->cb.free_guest(w->cb.context, address);
            goto bad;
        }
        int ok = w->cb.invoke_wndproc(w->cb.context, cpu, w->procedure, HWND_GAME, 0x81, 0, address,
                                      &value);
        if (ok && value)
            ok = w->cb.invoke_wndproc(w->cb.context, cpu, w->procedure, HWND_GAME, 1, 0, address,
                                      &value);
        else
            ok = 0;
        w->cb.free_guest(w->cb.context, address);
        if (!ok || value == UINT32_MAX) {
            w->live = 0;
            *last_error = 1407;
            result = 0;
        } else
            result = HWND_GAME;
    } else if (IS("ShowWindow")) {
        result = w->visible;
        w->visible = a[1] != 0;
        queue(w, 5, 0, ((uint32_t)w->width & 65535) | ((uint32_t)w->height << 16));
        queue(w, 0x1c, 1, 0);
        queue(w, 6, 1, 0);
        queue(w, 7, 0, 0);
    } else if (IS("UpdateWindow"))
        result = w->live;
    else if (IS("DefWindowProcA"))
        result = a[1] == 0x81 ? 1 : 0;
    else if (IS("PeekMessageA")) {
        if (w->owner && w->owner != w->cb.current_guest_thread_id(w->cb.context))
            return done(w, cpu, argc, 0);
        if (a[1] && a[1] != HWND_GAME && a[1] != UINT32_MAX)
            goto unsupported;
        if (a[4] & ~3u)
            goto unsupported;
        if (!w->count) {
            ac1_input_tick();
            ac1_input_message m;
            if (ac1_input_next_message(&m)) {
                queue(w, m.message, m.wparam, m.lparam);
                w->snapshots[0] = m;
                w->has_snapshot[0] = 1;
            }
        }
        unsigned index = 0;
        while (index < w->count &&
               ((a[2] || a[3]) && (w->messages[index][1] < a[2] || w->messages[index][1] > a[3])))
            ++index;
        if (index < w->count) {
            if (!wr(w, a[0], w->messages[index], 28))
                goto bad;
            result = 1;
            w->last_has_snapshot = w->has_snapshot[index];
            w->last_snapshot = w->snapshots[index];
            if (a[4] & 1) {
                --w->count;
                memmove(w->messages[index], w->messages[index + 1], (w->count - index) * 28);
                memmove(w->snapshots + index, w->snapshots + index + 1,
                        (w->count - index) * sizeof(w->snapshots[0]));
                memmove(w->has_snapshot + index, w->has_snapshot + index + 1, w->count - index);
            }
        }
    } else if (IS("TranslateMessage"))
        result = 0;
    else if (IS("DispatchMessageA")) {
        uint32_t m[7];
        if (!rd(w, a[0], m, 28))
            goto bad;
        if (m[1] != 0x12 && w->live) {
            const ac1_input_message *snapshot =
                w->last_has_snapshot && w->last_snapshot.message == m[1] &&
                        w->last_snapshot.wparam == m[2] && w->last_snapshot.lparam == m[3]
                    ? &w->last_snapshot
                    : NULL;
            const ac1_input_message *previous = ac1_input_dispatch_state(snapshot);
            int ok = w->cb.invoke_wndproc(w->cb.context, cpu, w->procedure, m[0], m[1], m[2], m[3],
                                          &result);
            ac1_input_dispatch_state(previous);
            if (!ok)
                goto bad;
        }
    } else if (IS("DestroyWindow")) {
        if (a[0] != HWND_GAME || !w->live) {
            *last_error = 1400;
        } else {
            if (!w->cb.invoke_wndproc(w->cb.context, cpu, w->procedure, HWND_GAME, 2, 0, 0,
                                      &result))
                goto bad;
            w->live = w->visible = 0;
            result = 1;
        }
    } else if (IS("PostQuitMessage")) {
        queue(w, 0x12, a[0], 0);
        result = 0;
    } else if (IS("GetDesktopWindow"))
        result = HWND_DESKTOP;
    else if (IS("IsWindow"))
        result = (a[0] == HWND_GAME && w->live) || a[0] == HWND_DESKTOP;
    else if (IS("IsWindowVisible"))
        result = a[0] == HWND_GAME && w->visible;
    else if (IS("IsIconic"))
        result = 0;
    else if (IS("ShowCursor"))
        result = a[0] ? ++w->cursor_count : --w->cursor_count;
    else if (IS("ClipCursor")) {
        int32_t rect[4];
        if (a[0] && !rd(w, a[0], rect, 16))
            goto bad;
        result = 1;
    } else if (IS("GetClientRect") || IS("GetWindowRect")) {
        int32_t rect[] = {0, 0, w->width, w->height};
        if (!wr(w, a[1], rect, 16))
            goto bad;
        result = 1;
    } else if (IS("GetCursorPos")) {
        int32_t xy[2];
        ac1_input_cursor(xy, xy + 1);
        if (!wr(w, a[0], xy, 8))
            goto bad;
        result = 1;
    } else if (IS("ScreenToClient")) {
        int32_t xy[2];
        if (!rd(w, a[1], xy, 8))
            goto bad;
        result = 1;
    } else if (IS("AdjustWindowRect")) {
        int32_t rect[4];
        if (!rd(w, a[0], rect, 16))
            goto bad;
        result = 1;
    } else if (IS("GetSystemMetrics")) {
        switch (a[0]) {
        case 0:
        case 16:
        case 59:
            result = w->width;
            break;
        case 1:
        case 17:
        case 60:
            result = w->height;
            break;
        case 19:
            result = 1;
            break;
        case 43:
            result = 3;
            break;
        case 23: /* SM_SWAPBUTTON: standard left/right physical mouse mapping. */
            result = 0;
            break;
        default:
            snprintf(error, cap, "Android USER32 GetSystemMetrics: unsupported index=%u", a[0]);
            return -1;
        }
    } else if (IS("GetDC"))
        result = 0xe5200001;
    else if (IS("ReleaseDC"))
        result = a[1] == 0xe5200001;
    else if (IS("GetKeyboardState")) {
        uint8_t keys[256];
        ac1_input_tick();
        ac1_input_vk_state(keys);
        if (!wr(w, a[0], keys, 256))
            goto bad;
        result = 1;
    } else if (IS("ToUnicode")) { // US text entry sufficient for the first-run profile name.
        uint8_t keys[256];
        if (!rd(w, a[2], keys, 256))
            goto bad;
        unsigned vk = a[0];
        uint16_t c = 0;
        if (vk >= 'A' && vk <= 'Z')
            c = (keys[16] & 0x80) ? vk : vk + 32;
        else if ((vk >= '0' && vk <= '9') || vk == 32)
            c = vk;
        if (c && a[4]) {
            if (!wr(w, a[3], &c, 2))
                goto bad;
            result = 1;
        }
    }
    return done(w, cpu, argc, result);
bad:
    snprintf(error, cap, "Android USER32 %s: invalid guest argument/callback", name);
    return -1;
unsupported:
    snprintf(error, cap, "Android USER32 %s: unsupported argument profile", name);
    return -1;
#undef IS
}
