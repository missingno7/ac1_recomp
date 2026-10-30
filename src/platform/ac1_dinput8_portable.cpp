/* Portable system keyboard/mouse DirectInput8 proxies using the bridge ABI. */
#include "ac1_dinput8_bridge.h"
#include "ac1_input_script.h"
#include "ac1_pad_discovery.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
namespace {
struct Object {
    unsigned kind, input;
    uint32_t table, refs = 1, size = 0;
    bool acquired = false;
};
ac1_dinput8_callbacks cb{};
void *ctx;
std::mutex mutex;
std::map<uint32_t, Object> objects;
thread_local char error[256];
thread_local std::vector<uint8_t> pending;
thread_local uint32_t pending_size;
int fail(const char *s) {
    std::snprintf(error, sizeof(error), "DirectInput portable: %s", s);
    return -1;
}
void *mem(uint32_t a, uint32_t n, unsigned access) {
    return a && n && cb.guest_resolve ? cb.guest_resolve(ctx, a, n, access) : nullptr;
}
bool read(uint32_t a, void *p, uint32_t n) {
    auto q = mem(a, n, 1);
    if (!q)
        return false;
    std::memcpy(p, q, n);
    return true;
}
bool write(uint32_t a, const void *p, uint32_t n) {
    auto q = mem(a, n, 2);
    if (!q)
        return false;
    std::memcpy(q, p, n);
    return true;
}
bool word(uint32_t a, uint32_t v) { return write(a, &v, 4); }
unsigned slots(unsigned k) { return k == 1 ? 3 : k <= 3 ? 11 : 32; }
uint32_t wrap(unsigned k, unsigned input = 0) {
    uint32_t table = cb.guest_alloc(ctx, slots(k) * 4, 4), obj = cb.guest_alloc(ctx, 4, 4);
    if (!table || !obj) {
        if (table)
            cb.guest_free(ctx, table);
        if (obj)
            cb.guest_free(ctx, obj);
        return 0;
    }
    for (unsigned i = 0; i < slots(k); ++i)
        if (!word(table + 4 * i, AC1_DINPUT8_TOKEN_BASE + k * 0x1000 + i * 16)) {
            cb.guest_free(ctx, table);
            cb.guest_free(ctx, obj);
            return 0;
        }
    if (!word(obj, table)) {
        cb.guest_free(ctx, table);
        cb.guest_free(ctx, obj);
        return 0;
    }
    objects.emplace(obj, Object{k, input, table});
    return obj;
}
unsigned iid(const uint32_t *g) {
    static const uint32_t ids[][4] = {{0, 0, 0xc0, 0x46000000},
                                      {0xbf798030, 0x4da2483a, 0x645d99aa, 0x009736ed},
                                      {0xbf798031, 0x4da2483a, 0x645d99aa, 0x009736ed},
                                      {0x54d41080, 0x4833dc15, 0x8f741ba4, 0x7981a373},
                                      {0x54d41081, 0x4833dc15, 0x8f741ba4, 0x7981a373}};
    for (unsigned i = 0; i < 5; ++i)
        if (!std::memcmp(g, ids[i], 16))
            return i + 1;
    return 0;
}
void record(unsigned size) { pending.resize(pending.size() + size, 0); }
void setword(size_t offset, uint32_t value) { std::memcpy(pending.data() + offset, &value, 4); }
void name(size_t offset, const char *s) {
    while (*s) {
        pending[offset++] = uint8_t(*s++);
        pending[offset++] = 0;
    }
}
void device(unsigned input) {
    size_t base = pending.size();
    record(0x44c);
    setword(base, 0x44c);
    uint32_t guid[4] = {input == 1 ? 0x6f1d2b61u : 0x6f1d2b60u, 0x11cfd5a0, 0x4544c7bf, 0x00005453};
    std::memcpy(pending.data() + base + 4, guid, 16);
    std::memcpy(pending.data() + base + 20, guid, 16);
    setword(base + 36, input == 1 ? 0x413 : 0x212);
    name(base + 40, input == 1 ? "Android keyboard" : "Android mouse");
    name(base + 560, input == 1 ? "AC1 keyboard" : "AC1 mouse");
}
void object(unsigned input, unsigned index) {
    size_t base = pending.size();
    record(0x240);
    setword(base, 0x240);
    uint32_t guid[4] = {input == 1  ? 0x55728220u
                        : index < 3 ? 0xa36d02e0u + index
                                    : 0xa36d02f0u,
                        input == 1 ? 0x11cfd33cu : 0x11cfc9f3u, 0x4544c7bf, 0x00005453};
    std::memcpy(pending.data() + base + 4, guid, 16);
    setword(base + 20, input == 1 ? index : index < 3 ? index * 4 : 12 + index - 3);
    setword(base + 24, (input == 1 || index >= 3 ? 0x0c : 0x01) |
                           ((input == 1 || index < 3 ? index : index - 3) << 8));
    name(base + 32, input == 1 ? "Key" : index < 3 ? "Axis" : "Button");
}
} // namespace
extern "C" int ac1_dinput8_configure(const ac1_dinput8_callbacks *c, void *p) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!c || c->struct_size < sizeof(*c) || !c->guest_alloc || !c->guest_free ||
        !c->guest_resolve || !objects.empty())
        return 0;
    cb = *c;
    ctx = p;
    error[0] = 0;
    return 1;
}
extern "C" int ac1_dinput8_create8(uint32_t instance, uint32_t version, uint32_t guid, uint32_t out,
                                   uint32_t outer, uint32_t *hr) {
    std::lock_guard<std::mutex> lock(mutex);
    uint32_t g[4];
    if (!hr || !read(guid, g, 16) || !mem(out, 4, 2))
        return fail("Create8 arguments unmapped");
    word(out, 0);
    unsigned k = iid(g);
    *hr = 0;
    if (outer) {
        *hr = 0x80040110;
        return 1;
    }
    if (version != 0x800) {
        *hr = 0x8007000a;
        return 1;
    }
    if (k != 2 && k != 3) {
        *hr = 0x80004002;
        return 1;
    }
    if (!cb.guest_hinstance_to_host || !cb.guest_hinstance_to_host(ctx, instance)) {
        *hr = 0x80070006;
        return 1;
    }
    uint32_t obj = wrap(k);
    if (!obj)
        return fail("proxy allocation failed");
    word(out, obj);
    return 1;
}
extern "C" int ac1_dinput8_argument_count(uint32_t t) {
    if (t < AC1_DINPUT8_TOKEN_BASE || t >= AC1_DINPUT8_TOKEN_BASE + 0x10000)
        return 0;
    unsigned k = (t - AC1_DINPUT8_TOKEN_BASE) / 0x1000, s = t % 0x1000 / 16;
    if (t % 16 || !k || k > 5 || s >= slots(k))
        return -1;
    if (s < 3)
        return s ? 1 : 3;
    if (k == 2 || k == 3) {
        if (s == 3)
            return 4;
        if (s == 4)
            return 5;
    }
    if (k == 4 || k == 5) {
        switch (s) {
        case 4:
            return 4;
        case 7:
        case 8:
        case 25:
            return 1;
        case 5:
        case 9:
            return 3;
        case 11:
            return 2;
        case 13:
            return 3;
        }
    }
    return -2;
}
extern "C" int ac1_dinput8_dispatch(uint32_t t, const uint32_t *a, uint32_t n, uint32_t *hr) {
    std::lock_guard<std::mutex> lock(mutex);
    error[0] = 0;
    int arity = ac1_dinput8_argument_count(t);
    if (!arity)
        return 0;
    if (arity < 0) {
        fail("unsupported or malformed COM token");
        return arity;
    }
    if (!a || !hr || n != unsigned(arity))
        return fail("dispatch arity mismatch");
    auto it = objects.find(a[0]);
    unsigned k = (t - AC1_DINPUT8_TOKEN_BASE) / 0x1000, s = t % 0x1000 / 16;
    if (it == objects.end() || it->second.kind != k)
        return fail("unknown proxy/interface");
    auto &o = it->second;
    *hr = 0;
    if (s == 0) {
        uint32_t g[4];
        if (!read(a[1], g, 16) || !mem(a[2], 4, 2))
            return fail("QueryInterface arguments unmapped");
        unsigned kind = iid(g);
        word(a[2], 0);
        if (kind == 1 || kind == k) {
            ++o.refs;
            word(a[2], a[0]);
        } else
            *hr = 0x80004002;
        return 1;
    }
    if (s == 1) {
        *hr = ++o.refs;
        return 1;
    }
    if (s == 2) {
        *hr = --o.refs;
        if (!o.refs) {
            cb.guest_free(ctx, o.table);
            cb.guest_free(ctx, a[0]);
            objects.erase(it);
        }
        return 1;
    }
    if (k <= 3 && s == 3) {
        uint32_t g[4];
        if (!read(a[1], g, 16) || !mem(a[2], 4, 2))
            return fail("CreateDevice arguments unmapped");
        word(a[2], 0);
        if (a[3]) {
            *hr = 0x80040110;
            return 1;
        }
        if(ac1_pad_is_discovery(g)) {
            uint32_t obj=wrap(k+2,3);if(!obj)return fail("Xbox discovery allocation failed");
            word(a[2],obj);return 1;
        }
        unsigned input = g[0] == 0x6f1d2b61 ? 1 : g[0] == 0x6f1d2b60 ? 2 : 0;
        if (!input || g[1] != 0x11cfd5a0 || g[2] != 0x4544c7bf || g[3] != 0x5453) {
            *hr = 0x80040154;
            return 1;
        }
        uint32_t obj = wrap(k + 2, input);
        if (!obj)
            return fail("device allocation failed");
        word(a[2], obj);
        return 1;
    }
    if (o.input == 3 && s > 2) {
        if(s==5 && a[1]==24) {
            uint32_t prop[5];
            if(!read(a[2],prop,20)||prop[0]!=20||prop[1]!=16||prop[2]||prop[3])
                return fail("Xbox discovery DIPROP_VIDPID requires 20-byte DIPH_DEVICE property");
            prop[4]=0x028e045e;
            return write(a[2],prop,20)?1:fail("Xbox discovery property output unmapped");
        }
        return fail("Xbox discovery only supports DIPROP_VIDPID; gameplay uses XInput");
    }
    if (s == 4) {
        pending.clear();
        pending_size = k <= 3 ? 0x44c : 0x240;
        // The runtime delivers W records and callbacks; refuse A enumeration.
        if (k == 2 || k == 4)
            return fail("ANSI enumeration is unsupported");
        if (k == 3) {
            if ((!a[1] || a[1] == 4 || a[1] == 0x15) && ac1_pad_discovery_connected()) {
                size_t base=pending.size();record(0x44c);ac1_pad_discovery_record(pending.data()+base);
            }
            if (!a[1] || a[1] == 3 || a[1] == 0x13)
                device(1);
            if (!a[1] || a[1] == 2 || a[1] == 0x12)
                device(2);
        } else
            for (unsigned i = 0; i < (o.input == 1 ? 256u : 11u); ++i) {
                unsigned type = o.input == 1 || i >= 3 ? 0x0c : 0x03;
                if (!a[3] || (a[3] & type))
                    object(o.input, i);
            }
        return 1;
    }
    if (s == 7) {
        o.acquired = true;
        return 1;
    }
    if (s == 8) {
        o.acquired = false;
        return 1;
    }
    if (s == 25)
        return 1;
    if (s == 13) {
        if (!cb.guest_hwnd_to_host || !cb.guest_hwnd_to_host(ctx, a[1]))
            *hr = 0x80070006;
        return 1;
    }
    if (s == 11) {
        uint32_t fmt[6];
        if (!read(a[1], fmt, 24))
            return fail("data format unmapped");
        if (fmt[0] != 24 || fmt[1] != 16 || fmt[2] != 2 ||
            (o.input == 1 ? fmt[3] != 256 : fmt[3] != 16 && fmt[3] != 20) || fmt[4] > 256)
            return fail("only canonical keyboard/mouse data formats supported");
        auto p = static_cast<const uint32_t *>(mem(fmt[5], fmt[4] * 16, 1));
        if (fmt[4] && !p)
            return fail("format objects unmapped");
        for (unsigned i = 0; i < fmt[4]; ++i) {
            unsigned off = p[i * 4 + 1], type = p[i * 4 + 2], instance = (type >> 8) & 65535;
            bool ok = o.input == 1 ? off < 256 && (type & 0xff) == 0x0c &&
                                         (instance == off || instance == 65535)
                      : off < 12 ? off % 4 == 0 && (type & 3) != 0 &&
                                       (instance == off / 4 || instance == 65535)
                                 : off < fmt[3] && (type & 0x0c) != 0 &&
                                       (instance == off - 12 || instance == 65535);
            if (!ok)
                return fail("custom DirectInput object layout unsupported");
        }
        o.size = fmt[3];
        return 1;
    }
    if (s == 9) {
        if (!o.acquired) {
            *hr = 0x8007000c;
            return 1;
        }
        if (!o.size || a[1] != o.size)
            return fail("GetDeviceState size differs from canonical format");
        auto out = mem(a[2], a[1], 2);
        if (!out)
            return fail("state output unmapped");
        char detail[256];
        if (!ac1_input_session_init(detail, sizeof(detail)))
            return fail(detail);
        int replay = ac1_input_replace_state(o.input == 1, out, a[1]);
        if (replay < 0)
            return fail("replay format unsupported");
        if (!replay && ac1_input_physical_state(o.input == 1, out, a[1]) < 0)
            return fail("physical format unsupported");
        ac1_input_record_state(o.input == 1, out, a[1]);
        return 1;
    }
    return fail("unsupported method");
}
extern "C" int ac1_dinput8_enum_batch_take(ac1_dinput8_enum_batch *b) {
    if (!b)
        return -1;
    if (!pending_size) {
        *b = {};
        return 0;
    }
    b->opaque = new std::vector<uint8_t>(std::move(pending));
    b->record_size = pending_size;
    b->count = static_cast<std::vector<uint8_t> *>(b->opaque)->size() / pending_size;
    b->native_hresult = 0;
    pending_size = 0;
    return 1;
}
extern "C" const void *ac1_dinput8_enum_batch_record(const ac1_dinput8_enum_batch *b, uint32_t i) {
    if (!b || !b->opaque || i >= b->count)
        return nullptr;
    return static_cast<std::vector<uint8_t> *>(b->opaque)->data() + size_t(i) * b->record_size;
}
extern "C" void ac1_dinput8_enum_batch_free(ac1_dinput8_enum_batch *b) {
    if (b) {
        delete static_cast<std::vector<uint8_t> *>(b->opaque);
        *b = {};
    }
}
extern "C" void ac1_dinput8_shutdown(void) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto &o : objects) {
        cb.guest_free(ctx, o.second.table);
        cb.guest_free(ctx, o.first);
    }
    objects.clear();
    pending.clear();
    pending_size = 0;
    ac1_input_session_shutdown();
    ac1_input_physical_reset();
    cb = {};
    ctx = nullptr;
}
extern "C" const char *ac1_dinput8_last_error(void) { return error; }
