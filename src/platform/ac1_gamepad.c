/* XInput state/capability layout and disconnected behavior adapted from
 * recomp-kit dx/xinput.cpp, copyright 2026 veritr1x, MIT.
 * See src/runtime/LICENSE.recomp-kit.txt for the full permission notice. */
#include "ac1_gamepad.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK mutex = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&mutex)
#define UNLOCK() ReleaseSRWLockExclusive(&mutex)
#else
#include <pthread.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mutex)
#define UNLOCK() pthread_mutex_unlock(&mutex)
#endif
_Static_assert(sizeof(ac1_pad) == 12, "XINPUT_GAMEPAD ABI");
_Static_assert(sizeof(ac1_pad_state) == 16, "XINPUT_STATE ABI");
_Static_assert(sizeof(ac1_pad_caps) == 20, "XINPUT_CAPABILITIES ABI");
static struct { int connected; ac1_pad_state state; } pads[4];

#ifdef _WIN32
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HMODULE library;
static BOOL CALLBACK load_xinput(PINIT_ONCE init, PVOID parameter, PVOID *context) {
    (void)init; (void)parameter; (void)context;
    library = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) library = LoadLibraryExW(L"xinput1_3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) library = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return TRUE;
}
static FARPROC native(const char *name) {
    const char *setting = getenv("AC1_XINPUT_NATIVE");
    if (setting && !strcmp(setting, "0")) return NULL;
    InitOnceExecuteOnce(&once, load_xinput, NULL, NULL);
    return library ? GetProcAddress(library, name) : NULL;
}
#endif
void ac1_pad_submit(unsigned user, int connected, const ac1_pad *pad) {
    if (user >= 4) return;
    ac1_pad value = {0};
    if (connected && pad) value = *pad;
    LOCK();
    if (pads[user].connected != !!connected || memcmp(&pads[user].state.pad, &value, 12))
        ++pads[user].state.packet;
    pads[user].connected = !!connected;
    pads[user].state.pad = value;
    UNLOCK();
}
void ac1_pad_reset(void) {
    for (unsigned i = 0; i < 4; ++i) ac1_pad_submit(i, 0, NULL);
}
/* Return 1 for replay (including disconnected replay), 2 for submitted source,
 * 0 when the native Windows provider should be polled. Never hold the pad lock
 * while taking the input-script lock. */
static int snapshot(unsigned user, ac1_pad_state *out, int *connected) {
#if defined(_WIN32) || defined(__ANDROID__)
    ac1_pad p; uint32_t packet; int attached;
    if (ac1_input_session_pad(&p, &attached, &packet)) {
        *connected = user == 0 && attached;
        out->pad = p; out->packet = packet;
        return 1;
    }
#endif
    LOCK();
    *connected = pads[user].connected;
    *out = pads[user].state;
    UNLOCK();
    return *connected ? 2 : 0;
}
uint32_t ac1_pad_get_state(unsigned user, ac1_pad_state *out) {
    if (user >= 4 || !out) return 160;
    ac1_pad_state value; int connected;
    int source = snapshot(user, &value, &connected);
#ifdef _WIN32
    if (!source) {
        typedef DWORD (WINAPI *fn_t)(DWORD, void *);
        FARPROC p = native("XInputGetState"); fn_t fn;
        if (p) { memcpy(&fn, &p, sizeof(fn)); return fn(user, out); }
    }
#else
    (void)source;
#endif
    if (getenv("AC1_PAD_TRACE") && user == 0) {
        static int previous_connected = -1;
        static uint32_t previous_packet = UINT32_MAX;
        LOCK();
        if (connected != previous_connected || (connected && value.packet != previous_packet)) {
            fprintf(stderr, "AC1 XInput: user=%u connected=%d packet=%u buttons=%04x LT=%u RT=%u L=%d,%d R=%d,%d\n",
                    user, connected, value.packet, value.pad.buttons, value.pad.lt, value.pad.rt,
                    value.pad.lx, value.pad.ly, value.pad.rx, value.pad.ry);
            fflush(stderr); previous_connected = connected; previous_packet = value.packet;
        }
        UNLOCK();
    }
    if (!connected) return 1167;
    *out = value;
    return 0;
}
uint32_t ac1_pad_get_caps(unsigned user, unsigned flags, ac1_pad_caps *out) {
    if (user >= 4 || !out || (flags & ~1u)) return 160;
    ac1_pad_state value; int connected;
    int source = snapshot(user, &value, &connected);
#ifdef _WIN32
    if (!source) {
        typedef DWORD (WINAPI *fn_t)(DWORD, DWORD, void *);
        FARPROC p = native("XInputGetCapabilities"); fn_t fn;
        if (p) { memcpy(&fn, &p, sizeof(fn)); return fn(user, flags, out); }
    }
#else
    (void)source;
#endif
    if (!connected) return 1167;
    memset(out, 0, sizeof(*out));
    out->type = out->subtype = 1;
    out->pad.buttons = 0xf3ff;
    out->pad.lt = out->pad.rt = 255;
    out->pad.lx = out->pad.ly = out->pad.rx = out->pad.ry = 32767;
    /* No virtual rumble provider: zero motors, no force-feedback capability. */
    return 0;
}
uint32_t ac1_pad_set_state(unsigned user, const uint16_t motors[2]) {
    if (user >= 4 || !motors) return 160;
    ac1_pad_state value; int connected;
    int source = snapshot(user, &value, &connected);
#ifdef _WIN32
    if (!source) {
        typedef DWORD (WINAPI *fn_t)(DWORD, const void *);
        FARPROC p = native("XInputSetState"); fn_t fn;
        if (p) { memcpy(&fn, &p, sizeof(fn)); return fn(user, motors); }
    }
#else
    (void)source;
#endif
    return connected ? 0 : 1167;
}
