/* Small host-only behavioral contracts; no game code or assets. */
#include "../src/platform/ac1_dinput8_bridge.h"
#include "../src/platform/ac1_ds8_bridge.h"
#include "../src/platform/ac1_input_script.h"
#include "../src/platform/ac1_window_win32.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>
static std::array<uint8_t, 4 * 1024 * 1024> ram;
static uint32_t next = 4096;
static std::map<uint32_t, uint32_t> allocations;
static uint64_t clock_ns, presents;
static std::vector<uint32_t> events;
extern "C" uint64_t ac1_d3d9_present_count(void) { return presents; }
static void *resolve(void *, uint32_t a, uint32_t n, uint32_t) {
    return a && uint64_t(a) + n <= ram.size() ? ram.data() + a : nullptr;
}
static uint32_t alloc(void *, uint32_t n, uint32_t align) {
    next = (next + align - 1) & ~(align - 1);
    uint32_t a = next;
    next += n;
    assert(next < ram.size());
    allocations.emplace(a, n);
    return a;
}
static void release(void *, uint32_t a) { assert(allocations.erase(a) == 1); }
static uintptr_t hwnd(void *, uint32_t a) { return a ? 1 : 0; }
static uint64_t now(void *) { return clock_ns; }
static int signal(void *, uint32_t a) {
    events.push_back(a);
    return 1;
}
static void put(uint32_t a, uint32_t v) { std::memcpy(ram.data() + a, &v, 4); }
static uint32_t get(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, ram.data() + a, 4);
    return v;
}
static uint32_t data(std::initializer_list<uint32_t> list) {
    uint32_t a = alloc(nullptr, list.size() * 4, 4);
    std::memcpy(ram.data() + a, list.begin(), list.size() * 4);
    return a;
}
static uint32_t call(bool ds, uint32_t obj, unsigned slot,
                     std::initializer_list<uint32_t> args = {}) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args);
    uint32_t token = get(get(obj) + slot * 4), hr = ~0u;
    int arity = ds ? ac1_ds8_argument_count(token) : ac1_dinput8_argument_count(token);
    if (arity != int(a.size()))
        std::fprintf(stderr, "arity: ds=%d slot=%u token=%08x expected=%d actual=%zu\n", ds, slot,
                     token, arity, a.size());
    assert(arity == int(a.size()));
    int status = ds ? ac1_ds8_dispatch(token, a.data(), a.size(), &hr)
                    : ac1_dinput8_dispatch(token, a.data(), a.size(), &hr);
    if (status != 1)
        std::fprintf(stderr, "slot %u: %s\n", slot,
                     ds ? ac1_ds8_last_error() : ac1_dinput8_last_error());
    assert(status == 1);
    return hr;
}
static uint32_t qi(uint32_t obj, std::initializer_list<uint32_t> guid) {
    uint32_t g = data(guid), out = alloc(nullptr, 4, 4);
    assert(!call(true, obj, 0, {g, out}));
    return get(out);
}
static void sound() {
    ac1_ds8_callbacks cb{};
    cb.struct_size = sizeof(cb);
    cb.guest_resolve = resolve;
    cb.guest_alloc = alloc;
    cb.guest_free = release;
    cb.guest_hwnd_to_host = hwnd;
    cb.monotonic_ns = now;
    cb.signal_event = signal;
    assert(ac1_ds8_configure(&cb, nullptr));
    uint32_t out = alloc(nullptr, 4, 4), hr;
    assert(ac1_ds8_create8(0, out, 0, &hr) == 1 && !hr);
    uint32_t dev = get(out);
    uint32_t caps = alloc(nullptr, 96, 4);
    put(caps, 96);
    assert(!call(true, dev, 4, {caps}) && (get(caps + 4) & 0x20));
    // 1000 Hz mono s16, one second. Deliberately exercise wrap and frequency changes.
    uint32_t fmt = data({0x00010001, 1000, 2000, 0x00100002, 0});
    uint32_t desc = data({36, 0x90, 2000, 0, fmt, 0, 0, 0, 0});
    assert(!call(true, dev, 3, {desc, out, 0}));
    uint32_t b = get(out);
    // SDK IID_IDirectSoundBuffer8: 6825A449-7524-4D82-920F-50E36AB3AB1E.
    uint32_t buffer8 = qi(b, {0x6825a449, 0x4d827524, 0xe3500f92, 0x1eabb36a});
    assert(buffer8 && !call(true, buffer8, 9, {out}) && get(out) == 0);
    assert(call(true, buffer8, 22, {0, 0, 0}) == 0x88780032); // Non-LOCDEFER native result.
    uint32_t deferred_desc = data({36, 0x40090, 2000, 0, fmt, 0, 0, 0, 0});
    assert(!call(true, dev, 3, {deferred_desc, out, 0}));
    uint32_t deferred = get(out);
    uint32_t deferred8 = qi(deferred, {0x6825a449, 0x4d827524, 0xe3500f92, 0x1eabb36a});
    assert(!call(true, deferred8, 9, {out}) && get(out) == 0);
    for (uint32_t flags : {0u, 1u, 4u, 5u, 8u, 12u, 16u, 32u, 33u}) {
        assert(!call(true, deferred8, 12, {0, 1, flags}));
        assert(!call(true, deferred8, 9, {out}) && get(out) == (0x11u | (flags & 1 ? 4u : 0u)));
        assert(!call(true, deferred8, 18));
    }
    assert(call(true, deferred8, 12, {0, 1, 2}) == 0x88780032); // No hardware mixing.
    for (uint32_t flags : {6u, 24u, 128u})
        assert(call(true, deferred8, 12, {0, 1, flags}) == 0x80070057);
    assert(call(true, buffer8, 12, {0, 1, 0}) == 0x80070057); // Priority needs LOCDEFER.
    assert(!call(true, deferred8, 22, {0, 0, 0})); // No effects/results; software resources.
    assert(!call(true, deferred8, 9, {out}) && get(out) == 0x10);
    assert(!call(true, deferred8, 18));
    assert(!call(true, deferred8, 9, {out}) && get(out) == 0);
    uint32_t effects_args[] = {deferred8, 0, 1, out}, effects_hr = 0;
    assert(ac1_ds8_dispatch(get(get(deferred8) + 22 * 4), effects_args, 4, &effects_hr) < 0);
    assert(std::strstr(ac1_ds8_last_error(), "zero effects"));
    assert(call(true, deferred8, 2) == 0 && call(true, deferred, 2) == 0);
    assert(call(true, buffer8, 2) == 0);
    uint32_t io = alloc(nullptr, 32, 4);
    assert(!call(true, b, 11, {1996, 8, io, io + 4, io + 8, io + 12, 0}));
    assert(get(io + 4) == 4 && get(io + 12) == 4);
    put(get(io), 0x12345678);
    put(get(io + 8), 0xabcdef01);
    assert(call(true, b, 19, {get(io), 3, get(io + 8), 4}) != 0);
    assert(!call(true, b, 19, {get(io), 4, get(io + 8), 4}));
    uint32_t notify = qi(b, {0xb0210783, 0x11d089cd, 0xa00008af, 0x16cd25c9});
    uint32_t positions = data({1000, 51, 0, 52, 0xffffffff, 53});
    assert(!call(true, notify, 3, {3, positions}));
    assert(!call(true, b, 12, {0, 0, 1}));
    assert(events.size() == 1 && events[0] == 52);
    clock_ns += 250000000;
    assert(!call(true, b, 4, {io, io + 4}));
    assert(get(io) == 500);
    assert(!call(true, b, 17, {2000}));
    clock_ns += 375000000;
    ac1_ds8_service();
    assert(!call(true, b, 4, {io, 0}));
    assert(get(io) == 0);
    assert(events.size() == 3);
    assert(!call(true, b, 18));
    assert(events.back() == 53);
    clock_ns += 1000000000;
    assert(!call(true, b, 4, {io, 0}) && get(io) == 0);
    uint32_t spatial = qi(b, {0x279afa86, 0x11ce4981, 0x200021a5, 0x60e50baf});
    assert(!call(true, spatial, 13, {90, 180, 0}));
    put(io + 4, 0xdeadbeef);
    assert(!call(true, spatial, 4, {io, io + 8}));
    assert(get(io) == 90 && get(io + 8) == 180 && get(io + 4) == 0xdeadbeef);
    uint32_t primary_desc = data({36, 0x11, 0, 0, 0, 0, 0, 0, 0});
    assert(!call(true, dev, 3, {primary_desc, out, 0}));
    uint32_t primary = get(out);
    uint32_t listener = qi(primary, {0x279afa84, 0x11ce4981, 0x200021a5, 0x60e50baf});
    uint32_t params = alloc(nullptr, 68, 4);
    put(params, 64);
    put(params + 64, 0xdeadbeef);
    assert(!call(true, listener, 3, {params}));
    assert(get(params + 36) == 0x3f800000 && get(params + 44) == 0x3f800000);
    assert(get(params + 48) == 0 && get(params + 52) == 0x3f800000);
    assert(get(params + 56) == 0x3f800000 && get(params + 60) == 0x3f800000);
    put(params + 52, 0x3f000000); // distance 0.5, rolloff 2, Doppler 3 at SDK offsets.
    put(params + 56, 0x40000000);
    put(params + 60, 0x40400000);
    assert(!call(true, listener, 10, {params, 1}));
    assert(!call(true, listener, 4, {io}) && get(io) == 0x3f800000); // Deferred.
    assert(!call(true, listener, 17));
    assert(!call(true, listener, 4, {io}) && get(io) == 0x3f000000);
    assert(!call(true, listener, 8, {io}) && get(io) == 0x40000000);
    assert(!call(true, listener, 5, {io}) && get(io) == 0x40400000);
    assert(!call(true, listener, 3, {params}) && get(params + 64) == 0xdeadbeef);
    assert(call(true, listener, 2) == 0 && call(true, primary, 2) == 0);
    assert(!call(true, dev, 5, {b, out}));
    uint32_t dup = get(out);
    assert(call(true, notify, 2) == 0);
    assert(call(true, spatial, 2) == 0);
    assert(call(true, b, 2) == 0);
    assert(!call(true, dup, 11, {0, 4, io, io + 4, io + 8, io + 12, 0}));
    assert(get(get(io)) == 0xabcdef01);
    assert(!call(true, dup, 19, {get(io), 4, 0, 0}));
    assert(!call(true, dup, 13, {1900}));
    assert(!call(true, dup, 12, {0, 0, 0}));
    clock_ns += 100000000;
    ac1_ds8_service();
    assert(!call(true, dup, 9, {io}));
    assert(!(get(io) & 1));
    // A completed nonlooping buffer can restart from its wrapped cursor.
    assert(!call(true, dup, 12, {0, 0, 0}));
    clock_ns += 25000000;
    assert(!call(true, dup, 4, {io, 0}) && get(io) == 100);
    uint32_t bogus = AC1_DS8_TOKEN_BASE + 3 * 0x1000 + 23 * 16;
    assert(ac1_ds8_argument_count(bogus) < 0);
    ac1_ds8_shutdown();
}
static void input() {
    ac1_dinput8_callbacks cb{};
    cb.struct_size = sizeof(cb);
    cb.guest_resolve = resolve;
    cb.guest_alloc = alloc;
    cb.guest_free = release;
    cb.guest_hwnd_to_host = hwnd;
    cb.guest_hinstance_to_host = hwnd;
    assert(ac1_dinput8_configure(&cb, nullptr));
    uint32_t out = alloc(nullptr, 4, 4), hr;
    uint32_t iid = data({0xbf798031, 0x4da2483a, 0x645d99aa, 0x009736ed});
    assert(ac1_dinput8_create8(1, 0x800, iid, out, 0, &hr) == 1 && !hr);
    uint32_t di = get(out);
    assert(!call(false, di, 4, {0, 1, 0, 0}));
    ac1_dinput8_enum_batch batch{};
    assert(ac1_dinput8_enum_batch_take(&batch) == 1 && batch.count == 2 &&
           batch.record_size == 0x44c);
    ac1_dinput8_enum_batch_free(&batch);
    uint32_t guid = data({0x6f1d2b61, 0x11cfd5a0, 0x4544c7bf, 0x5453});
    assert(!call(false, di, 3, {guid, out, 0}));
    uint32_t kb = get(out);
    uint32_t fmt = data({24, 16, 2, 256, 0, 0}), state = alloc(nullptr, 256, 4);
    assert(!call(false, kb, 11, {fmt}));
    assert(call(false, kb, 9, {256, state}) != 0);
    assert(!call(false, kb, 7));
    ac1_input_physical_key(0x1c, 1);
    assert(!call(false, kb, 9, {256, state}));
    assert(ram[state + 0x1c] == 0x80);
    ac1_input_physical_key(0x1c, 0);
    assert(!call(false, kb, 9, {256, state}));
    assert(!ram[state + 0x1c]);
    assert(!call(false, kb, 4, {1, 0, 0}));
    assert(ac1_dinput8_enum_batch_take(&batch) == 1 && batch.count == 256);
    ac1_dinput8_enum_batch_free(&batch);
    // Mouse deltas are consumed once, buttons persist.
    ac1_input_physical_mouse(4, -3, 120, 1);
    uint32_t mouse[5];
    assert(ac1_input_physical_state(0, mouse, 20) == 1 && mouse[0] == 4 &&
           int32_t(mouse[1]) == -3 && mouse[3] == 0x80);
    ac1_input_physical_state(0, mouse, 20);
    assert(mouse[0] == 0 && mouse[3] == 0x80);
    ac1_dinput8_shutdown();
    // Parser uses the exact existing present-indexed script grammar.
    char error[256];
    auto script = ac1_input_parse("at 2 key ENTER down\nat 3 key ENTER up\n", error, sizeof(error));
    assert(script);
    uint8_t keys[256];
    ac1_input_advance(script, 1);
    ac1_input_keyboard(script, keys);
    assert(!keys[28]);
    ac1_input_advance(script, 2);
    ac1_input_keyboard(script, keys);
    assert(keys[28] == 0x80);
    ac1_input_message msg;
    assert(ac1_input_pop(script, &msg) && msg.message == 0x100 && msg.wparam == 13);
    ac1_input_advance(script, 3);
    ac1_input_keyboard(script, keys);
    assert(!keys[28]);
    ac1_input_free(script);
#ifdef _WIN32
    _putenv_s("AC1_INPUT_SCRIPT", "tools/input/to_gameplay.txt");
#else
    setenv("AC1_INPUT_SCRIPT", "tools/input/to_gameplay.txt", 1);
#endif
    assert(ac1_input_session_init(error, sizeof(error)));
    ac1_input_physical_key(28, 1); // replay isolates physical injection
    presents = 0;
    assert(ac1_input_replace_state(1, keys, 256) == 1 && !keys[28]);
    presents = 200;
    assert(ac1_input_replace_state(1, keys, 256) == 1 && keys[28] == 0x80);
    presents = 203;
    assert(ac1_input_replace_state(1, keys, 256) == 1 && !keys[28]);
    ac1_input_session_shutdown();
#ifdef _WIN32
    _putenv_s("AC1_INPUT_SCRIPT", "");
#else
    unsetenv("AC1_INPUT_SCRIPT");
#endif
}
extern "C" uintptr_t ac1_android_d3d9_guest_hwnd(void *, uint32_t a) { return a ? 1 : 0; }
extern "C" void ac1_android_surface_size(int32_t *w, int32_t *h) {
    *w = 1280;
    *h = 720;
}
static bool key_dispatched;
static int window_read(void *, uint32_t a, void *p, size_t n) {
    auto q = resolve(nullptr, a, n, 1);
    if (!q)
        return 0;
    std::memcpy(p, q, n);
    return 1;
}
static int window_write(void *, uint32_t a, const void *p, size_t n) {
    auto q = resolve(nullptr, a, n, 2);
    if (!q)
        return 0;
    std::memcpy(q, p, n);
    return 1;
}
static int window_alloc(void *, size_t n, uint32_t *a) {
    *a = alloc(nullptr, n, 4);
    return 1;
}
static uint32_t thread_id(void *) { return 1; }
static int wndproc(void *, CPU *, uint32_t proc, uint32_t, uint32_t msg, uint32_t wp, uint32_t lp,
                   uint32_t *out) {
    assert(proc == 0x12345678);
    *out = msg == 0x81 ? 1 : 0;
    if (msg == 0x81) {
        assert(get(lp + 4) == 0x400000);
    }
    if (msg == 0x100) {
        assert(wp == 13);
        uint8_t vk[256];
        ac1_input_vk_state(vk);
        assert(vk[13] == 0x80);
        key_dispatched = true;
    }
    return 1;
}
static void window() {
    ac1_window_win32_callbacks cb{};
    cb.read_guest = window_read;
    cb.write_guest = window_write;
    cb.allocate_guest = window_alloc;
    cb.free_guest = release;
    cb.invoke_wndproc = wndproc;
    cb.current_guest_thread_id = thread_id;
    char error[256];
    auto *w = ac1_window_win32_create(&cb, error, sizeof(error));
    assert(w);
    uint32_t stack = alloc(nullptr, 64, 4), last_error = 0;
    auto invoke = [&](const char *name, std::initializer_list<uint32_t> args) {
        CPU cpu{};
        cpu.esp = stack;
        put(stack, 0x1234);
        unsigned offset = 4;
        for (auto arg : args) {
            put(stack + offset, arg);
            offset += 4;
        }
        int status = ac1_window_win32_call(w, &cpu, name, &last_error, error, sizeof(error));
        if (status != 1)
            std::fprintf(stderr, "%s\n", error);
        assert(status == 1 && cpu.esp == stack + offset && cpu.eip == 0x1234);
        return cpu.eax;
    };
    uint32_t cls = data({48, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    assert(invoke("RegisterClassExA", {cls}) == 1);
    uint32_t win = invoke("CreateWindowExA", {0, 1, 0, 0, 0, 0, 1280, 720, 0, 0, 0x400000, 0});
    assert(win);
    assert(ac1_window_win32_resolve_hwnd(w, win));
    uint32_t rect = alloc(nullptr, 28, 4);
    assert(invoke("GetClientRect", {win, rect}) == 1 && get(rect + 8) == 1280 &&
           get(rect + 12) == 720);
    ac1_input_physical_key(28, 1);
    ac1_input_physical_key(28, 0);
    assert(invoke("PeekMessageA", {rect, 0, 0, 0, 1}) == 1 && get(rect + 4) == 0x100);
    invoke("DispatchMessageA", {rect});
    assert(key_dispatched);
    assert(invoke("DestroyWindow", {win}) == 1);
    ac1_window_win32_destroy(w);
}
int main() {
    sound();
    input();
    window();
    std::puts("PASS portable DS8 cursors/locks/notifications/3D/duplicate lifetime and "
              "DirectInput/replay/window");
}
