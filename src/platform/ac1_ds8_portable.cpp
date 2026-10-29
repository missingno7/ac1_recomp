/* Clock-driven DirectSound backend. Sample ownership and notification servicing
 * follow recomp-kit dx/dsound.cpp, Copyright (c) 2026 veritr1x, MIT.
 * See LICENSE.recomp-kit.txt. This first Android backend is explicitly silent.
 * All guest access is through the existing bridge; no native COM pointers leak. */
#include "ac1_ds8_bridge.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {
constexpr uint32_t invalid = 0x80070057, nointerface = 0x80004002;
struct Samples {
    uint32_t address = 0, bytes = 0;
    ~Samples();
};
struct Buffer {
    std::shared_ptr<Samples> samples;
    std::array<uint8_t, 18> format{};
    uint32_t flags = 0, rate = 44100, frequency = 44100, align = 4;
    int32_t volume = 0, pan = 0;
    bool playing = false, looping = false, locked = false;
    uint32_t lock[4]{};
    uint64_t last_ns = 0, frames = 0, remainder = 0;
    std::array<uint32_t, 16> spatial{}, pending{};
    std::array<uint32_t, 15> listener{}, pending_listener{};
    std::vector<std::pair<uint32_t, uint32_t>> notifications;
    Buffer() {
        uint16_t fmt[] = {1, 2, 44100, 0, 45328, 2, 4, 16, 0};
        std::memcpy(format.data(), fmt, 18);
        spatial[0] = 64;
        spatial[7] = spatial[8] = 360;
        float one = 1, billion = 1e9f;
        std::memcpy(&spatial[13], &one, 4);
        std::memcpy(&spatial[14], &billion, 4);
        listener[0] = 60;
        std::memcpy(&listener[9], &one, 4);
        std::memcpy(&listener[11], &one, 4);
        for (unsigned i = 12; i < 15; ++i)
            std::memcpy(&listener[i], &one, 4);
        pending = spatial;
        pending_listener = listener;
    }
};
struct Proxy {
    uint32_t kind, table, refs;
    std::shared_ptr<Buffer> buffer;
};
ac1_ds8_callbacks cb{};
void *ctx;
std::map<uint32_t, Proxy> objects;
std::recursive_mutex mutex;
thread_local char error[256];
uint32_t speaker = 4;
int fail(const char *s) {
    std::snprintf(error, sizeof(error), "DirectSound portable: %s", s);
    return -1;
}
void *mem(uint32_t a, uint32_t n, uint32_t access) {
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
Samples::~Samples() {
    if (address)
        cb.guest_free(ctx, address);
}
uint64_t now() {
    return cb.monotonic_ns ? cb.monotonic_ns(ctx)
                           : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
}
unsigned slots(unsigned k) {
    static const unsigned counts[] = {0, 3, 12, 21, 18, 24, 21, 4};
    return k < 8 ? counts[k] : 0;
}
uint32_t token(unsigned k, unsigned s) { return AC1_DS8_TOKEN_BASE + k * 0x1000 + s * 16; }
uint32_t wrap(unsigned k, std::shared_ptr<Buffer> b = {}) {
    uint32_t table = cb.guest_alloc(ctx, slots(k) * 4, 4), obj = cb.guest_alloc(ctx, 4, 4);
    if (!table || !obj) {
        if (table)
            cb.guest_free(ctx, table);
        if (obj)
            cb.guest_free(ctx, obj);
        fail("proxy allocation failed");
        return 0;
    }
    for (unsigned s = 0; s < slots(k); ++s)
        if (!word(table + s * 4, token(k, s))) {
            cb.guest_free(ctx, table);
            cb.guest_free(ctx, obj);
            return 0;
        }
    if (!word(obj, table)) {
        cb.guest_free(ctx, table);
        cb.guest_free(ctx, obj);
        return 0;
    }
    objects.emplace(obj, Proxy{k, table, 1, std::move(b)});
    return obj;
}
void signal(uint32_t e) {
    if (!cb.signal_event || !cb.signal_event(ctx, e))
        fail("notification event is invalid or closed");
}
void stopped(Buffer &b) {
    for (auto n : b.notifications)
        if (n.first == UINT32_MAX)
            signal(n.second);
}
void update(Buffer &b) {
    uint64_t t = now();
    if (!b.playing) {
        b.last_ns = t;
        return;
    }
    uint64_t dt = t - b.last_ns;
    b.last_ns = t;
    uint64_t scaled = (dt % 1000000000) * b.frequency + b.remainder;
    uint64_t advance = dt / 1000000000 * b.frequency + scaled / 1000000000;
    b.remainder = scaled % 1000000000;
    uint64_t total = b.samples ? b.samples->bytes / b.align : 0;
    if (!total)
        return;
    uint64_t before = b.frames, after = before + advance;
    if (!b.looping)
        after = std::min(after, total);
    for (auto n : b.notifications)
        if (n.first != UINT32_MAX) {
            uint64_t pos = n.first / b.align, next = (before / total) * total + pos;
            if (next <= before)
                next += total;
            if (next <= after)
                signal(n.second);
        }
    b.frames = after;
    if (!b.looping && after == total) {
        b.playing = false;
        stopped(b);
    }
}
bool setformat(Buffer &b, uint32_t address) {
    std::array<uint8_t, 18> f;
    if (!read(address, f.data(), 18))
        return false;
    uint16_t tag, ch, align, bits, extra;
    uint32_t rate, bytes;
    std::memcpy(&tag, f.data(), 2);
    std::memcpy(&ch, f.data() + 2, 2);
    std::memcpy(&rate, f.data() + 4, 4);
    std::memcpy(&bytes, f.data() + 8, 4);
    std::memcpy(&align, f.data() + 12, 2);
    std::memcpy(&bits, f.data() + 14, 2);
    std::memcpy(&extra, f.data() + 16, 2);
    if (tag != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || align != ch * bits / 8 ||
        !rate || rate > 200000 || bytes != rate * align || extra)
        return false;
    update(b);
    b.format = f;
    b.align = align;
    b.rate = b.frequency = rate;
    return true;
}
unsigned iid_kind(const uint32_t *g) {
    static const uint32_t guids[][4] = {{0, 0, 0xc0, 0x46000000},
                                        {0xc50a7e93, 0x4834f395, 0xa97ff69e, 0x6609e59d},
                                        {0x279afa85, 0x11ce4981, 0x200021a5, 0x60e50baf},
                                        {0x279afa84, 0x11ce4981, 0x200021a5, 0x60e50baf},
                                        {0x6825a449, 0x4d827524, 0xe3500f92, 0x1eabba6a},
                                        {0x279afa86, 0x11ce4981, 0x200021a5, 0x60e50baf},
                                        {0xb0210783, 0x11d089cd, 0xa00008af, 0x16cd25c9}};
    for (unsigned i = 0; i < 7; ++i)
        if (!std::memcmp(g, guids[i], 16))
            return i + 1;
    return 0;
}
} // namespace
extern "C" int ac1_ds8_configure(const ac1_ds8_callbacks *c, void *p) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!c || c->struct_size < sizeof(*c) || !c->guest_alloc || !c->guest_free ||
        !c->guest_resolve || !objects.empty())
        return 0;
    cb = *c;
    ctx = p;
    error[0] = 0;
    speaker = 4;
    return 1;
}
extern "C" int ac1_ds8_create8(uint32_t guid, uint32_t out, uint32_t outer, uint32_t *hr) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!hr || !mem(out, 4, 2))
        return fail("Create8 output is unmapped");
    if (outer) {
        *hr = 0x80040110;
        word(out, 0);
        return 1;
    }
    if (guid) {
        uint32_t g[4];
        if (!read(guid, g, 16))
            return fail("Create8 GUID unmapped");
        if ((g[0] != 0xdef00000 && g[0] != 0xdef00002) || g[1] != 0x47ed9c6d ||
            g[2] != 0xda4df1aa || g[3] != 0x035c2b8f) {
            *hr = 0x88780078;
            word(out, 0);
            return 1;
        }
    }
    uint32_t obj = wrap(2);
    if (!obj)
        return -1;
    word(out, obj);
    *hr = 0;
    std::fprintf(stderr, "AC1 DirectSound: clock-driven silent backend (no AAudio output)\n");
    return 1;
}
extern "C" int ac1_ds8_argument_count(uint32_t t) {
    if (t < AC1_DS8_TOKEN_BASE || t >= AC1_DS8_TOKEN_BASE + 0x10000)
        return 0;
    unsigned k = (t - AC1_DS8_TOKEN_BASE) / 0x1000, s = (t % 0x1000) / 16;
    if (t % 16 || !slots(k) || s >= slots(k))
        return -1;
    if (s < 3)
        return s ? 1 : 3;
    static const int dev[] = {4, 2, 3, 3, 1, 2, 2, 2, 2};
    static const int buf[] = {2, 3, 4, 2, 2, 2, 2, 4, 8, 4, 2, 2, 2, 2, 2, 1, 5, 1};
    static const int listener[] = {2, 2, 2, 3, 2, 2, 2, 3, 3, 3, 8, 5, 3, 5, 1};
    static const int spatial[] = {2, 3, 2, 2, 2, 2, 2, 2, 2, 3, 4, 5, 3, 3, 3, 3, 5, 5};
    if (k == 2)
        return dev[s - 3];
    if (k == 3 || k == 5)
        return s < 21 ? buf[s - 3] : -2;
    if (k == 4)
        return listener[s - 3];
    if (k == 6)
        return spatial[s - 3];
    if (k == 7)
        return 3;
    return -2;
}
extern "C" int ac1_ds8_service(void) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    error[0] = 0;
    // Aliases share a Buffer; update's elapsed time is zero on subsequent aliases.
    for (auto &o : objects)
        if (o.second.buffer)
            update(*o.second.buffer);
    return error[0] ? -1 : 1;
}
extern "C" int ac1_ds8_dispatch(uint32_t t, const uint32_t *a, uint32_t n, uint32_t *hr) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    error[0] = 0;
    int count = ac1_ds8_argument_count(t);
    if (!count)
        return 0;
    if (count < 0) {
        fail("unsupported or malformed COM token");
        return count;
    }
    if (!a || !hr || n != unsigned(count))
        return fail("dispatch arity mismatch");
    unsigned k = (t - AC1_DS8_TOKEN_BASE) / 0x1000, s = t % 0x1000 / 16;
    auto it = objects.find(a[0]);
    if (it == objects.end() || it->second.kind != k)
        return fail("unknown proxy/interface");
    auto &o = it->second;
    *hr = 0;
    if (s == 0) {
        uint32_t g[4];
        if (!read(a[1], g, 16) || !mem(a[2], 4, 2))
            return fail("QueryInterface arguments unmapped");
        unsigned kind = iid_kind(g);
        word(a[2], 0);
        if (!kind || (k == 2 && kind != 1 && kind != 2) || (k != 2 && kind == 2)) {
            *hr = nointerface;
            return 1;
        }
        if (kind == k || kind == 1) {
            ++o.refs;
            word(a[2], a[0]);
            return 1;
        }
        uint32_t obj = wrap(kind, o.buffer);
        if (!obj)
            return -1;
        word(a[2], obj);
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
    auto put = [&](uint32_t addr, uint32_t v) {
        if (!word(addr, v))
            return fail("output unmapped");
        return 1;
    };
    if (k == 2) {
        if (s == 3) {
            uint32_t d[9];
            if (!read(a[1], d, 36) || d[0] != 36 || !mem(a[2], 4, 2))
                return fail("CreateSoundBuffer descriptor/output invalid");
            word(a[2], 0);
            if (a[3]) {
                *hr = 0x80040110;
                return 1;
            }
            auto b = std::make_shared<Buffer>();
            b->flags = d[1];
            if (d[4] && !setformat(*b, d[4])) {
                *hr = 0x88780064;
                return 1;
            }
            if (!(d[1] & 1) && !d[4]) {
                *hr = invalid;
                return 1;
            }
            uint32_t bytes = (d[1] & 1) ? b->rate * b->align : d[2];
            if (!bytes || bytes > 0x0fffffff || bytes % b->align) {
                *hr = invalid;
                return 1;
            }
            b->samples = std::make_shared<Samples>();
            b->samples->address = cb.guest_alloc(ctx, bytes, 16);
            b->samples->bytes = bytes;
            void *pcm = mem(b->samples->address, bytes, 2);
            if (!pcm)
                return fail("sample allocation failed");
            std::memset(pcm, b->format[14] == 8 ? 128 : 0, bytes);
            uint32_t obj = wrap(3, b);
            if (!obj)
                return -1;
            return put(a[2], obj);
        }
        if (s == 4) {
            uint32_t size;
            if (!read(a[1], &size, 4) || size != 96)
                return fail("DSCAPS size/range invalid");
            uint32_t caps[24]{};
            caps[0] = 96;
            caps[1] = 0xf3f;
            caps[2] = 100;
            caps[3] = 200000;
            if (!write(a[1], caps, 96))
                return fail("DSCAPS unmapped");
            return 1;
        }
        if (s == 5) {
            auto src = objects.find(a[1]);
            if (src == objects.end() || !src->second.buffer)
                return fail("duplicate source invalid");
            auto b = std::make_shared<Buffer>(*src->second.buffer);
            b->playing = b->locked = false;
            b->frames = b->remainder = 0;
            b->notifications.clear();
            uint32_t duplicate = wrap(3, b);
            if (!duplicate)
                return fail("duplicate allocation failed");
            return put(a[2], duplicate);
        }
        if (s == 6) {
            if (!cb.guest_hwnd_to_host || !cb.guest_hwnd_to_host(ctx, a[1])) {
                *hr = invalid;
            }
            return 1;
        }
        if (s == 7)
            return 1;
        if (s == 8)
            return put(a[1], speaker);
        if (s == 9) {
            speaker = a[1];
            return 1;
        }
        if (s == 10) {
            *hr = 0x88780082;
            return 1;
        } // already initialized
        if (s == 11)
            return put(a[1], 1);
    }
    if (!o.buffer)
        return fail("interface has no buffer");
    Buffer &b = *o.buffer;
    update(b);
    if (error[0])
        return -1;
    if (k == 7) {
        if (a[1] > 1024)
            return fail("notification count exceeds limit");
        auto p = static_cast<uint32_t *>(mem(a[2], a[1] * 8, 1));
        if (a[1] && !p)
            return fail("notification array unmapped");
        if (a[1] && !cb.signal_event)
            return fail("notification event service missing");
        std::vector<std::pair<uint32_t, uint32_t>> list;
        for (unsigned i = 0; i < a[1]; ++i) {
            if ((p[2 * i] != UINT32_MAX && p[2 * i] >= b.samples->bytes) || !p[2 * i + 1]) {
                *hr = invalid;
                return 1;
            }
            list.emplace_back(p[2 * i], p[2 * i + 1]);
        }
        b.notifications = std::move(list);
        return 1;
    }
    if (k == 4 || k == 6) {
        uint32_t *current = k == 4 ? b.listener.data() : b.spatial.data();
        uint32_t *pending = k == 4 ? b.pending_listener.data() : b.pending.data();
        unsigned size = k == 4 ? 60 : 64;
        if (s == 3) {
            uint32_t caller_size;
            if (!read(a[1], &caller_size, 4) || caller_size != size)
                return fail("3D parameter size/range invalid");
            if (!write(a[1], current, size))
                return fail("3D parameters output unmapped");
            return 1;
        }
        if (k == 4 && s == 17) {
            for (auto &entry : objects)
                if (entry.second.buffer) {
                    auto &x = *entry.second.buffer;
                    x.listener = x.pending_listener;
                    x.spatial = x.pending;
                }
            return 1;
        }
        if (s == (k == 4 ? 10u : 12u)) {
            uint32_t data[16];
            if (!read(a[1], data, size) || data[0] != size)
                return fail("3D parameter size/range invalid");
            std::memcpy(pending, data, size);
            if (!a[2])
                std::memcpy(current, data, size);
            else if (a[2] != 1)
                *hr = invalid;
            return 1;
        }
        // Getters and setters use the documented pack-4 DS3D structures.
        static const unsigned loff[] = {12, 14, 7, 1, 13, 4};        // listener getters 4..9
        static const unsigned boff[] = {7, 9, 12, 14, 13, 15, 1, 4}; // buffer getters 4..11
        bool get = s < (k == 4 ? 10u : 12u);
        unsigned index = get ? s - 4 : s - (k == 4 ? 11 : 13);
        unsigned off = k == 4 ? loff[index] : boff[index];
        unsigned words =
            k == 4 ? ((index == 2) ? 6 : ((index == 3 || index == 5) ? 3 : 1))
                   : (index == 0 ? 2 : ((index == 1 || index == 6 || index == 7) ? 3 : 1));
        if (get) {
            if (!write(a[1], current + off,
                       (k == 4 && index == 2)   ? 12
                       : (k == 6 && index == 0) ? 4
                                                : words * 4))
                return fail("3D getter output unmapped");
            if (k == 4 && index == 2 && !write(a[2], current + 10, 12))
                return fail("orientation output unmapped");
            if (k == 6 && index == 0 && !word(a[2], current[8]))
                return fail("cone output unmapped");
        } else {
            uint32_t apply = a[words + 1];
            if (apply > 1) {
                *hr = invalid;
                return 1;
            }
            std::memcpy(pending + off, a + 1, words * 4);
            if (!apply)
                std::memcpy(current + off, a + 1, words * 4);
        }
        return 1;
    }
    if (k != 3 && k != 5)
        return fail("unsupported interface method");
    uint32_t bytes = b.samples->bytes, cursor = uint32_t(b.frames % (bytes / b.align)) * b.align;
    if (s == 3) {
        uint32_t size;
        if (!read(a[1], &size, 4) || size != 20)
            return fail("DSBCAPS size/range invalid");
        uint32_t caps[] = {20, b.flags, bytes, 0, 0};
        if (!write(a[1], caps, 20))
            return fail("DSBCAPS output unmapped");
        return 1;
    }
    if (s == 4) {
        if (a[1] && !word(a[1], cursor))
            return fail("play cursor output unmapped");
        if (a[2] && !word(a[2], (cursor + b.frequency / 100 * b.align) % bytes))
            return fail("write cursor output unmapped");
        return 1;
    }
    if (s == 5) {
        if (!a[1])
            return put(a[3], 18);
        if (a[2] < 18) {
            *hr = invalid;
            return 1;
        }
        if (!write(a[1], b.format.data(), 18))
            return fail("format output unmapped");
        return a[3] ? put(a[3], 18) : 1;
    }
    if (s >= 6 && s <= 9)
        return put(a[1], s == 6   ? uint32_t(b.volume)
                         : s == 7 ? uint32_t(b.pan)
                         : s == 8 ? b.frequency
                                  : (b.playing ? 1u : 0u) | (b.playing && b.looping ? 4u : 0u));
    if (s == 10) {
        *hr = 0x88780082;
        return 1;
    }
    if (s == 11) {
        if (b.locked) {
            *hr = invalid;
            return 1;
        }
        uint32_t offset = a[1], length = a[2];
        if (a[7] & ~3u) {
            *hr = invalid;
            return 1;
        }
        if (a[7] & 1)
            offset = (cursor + b.frequency / 100 * b.align) % bytes;
        if (a[7] & 2) {
            offset = 0;
            length = bytes;
        }
        if (offset >= bytes || length > bytes || !length || offset % b.align || length % b.align) {
            *hr = invalid;
            return 1;
        }
        uint32_t first = std::min(length, bytes - offset), second = length - first;
        if (!mem(a[3], 4, 2) || !mem(a[4], 4, 2) ||
            (second && (!mem(a[5], 4, 2) || !mem(a[6], 4, 2))))
            return fail("Lock outputs unmapped");
        b.lock[0] = b.samples->address + offset;
        b.lock[1] = first;
        b.lock[2] = second ? b.samples->address : 0;
        b.lock[3] = second;
        for (unsigned i = 0; i < 4; ++i)
            if (a[i + 3])
                word(a[i + 3], b.lock[i]);
        b.locked = true;
        return 1;
    }
    if (s == 12) {
        if (a[1] || a[2] || a[3] & ~1u) {
            *hr = invalid;
            return 1;
        }
        if (!b.playing)
            for (auto pos : b.notifications)
                if (pos.first == cursor)
                    signal(pos.second);
        b.frames %= bytes / b.align;
        b.playing = true;
        b.looping = a[3] & 1;
        b.last_ns = now();
        return error[0] ? -1 : 1;
    }
    if (s == 13) {
        if (a[1] >= bytes) {
            *hr = invalid;
            return 1;
        }
        b.frames = a[1] / b.align;
        b.remainder = 0;
        return 1;
    }
    if (s == 14) {
        if (!(b.flags & 1) || !setformat(b, a[1]))
            *hr = 0x88780064;
        return 1;
    }
    if (s == 15 || s == 16) {
        int32_t value = int32_t(a[1]);
        if (value < -10000 || value > (s == 15 ? 0 : 10000)) {
            *hr = invalid;
            return 1;
        }
        (s == 15 ? b.volume : b.pan) = value;
        return 1;
    }
    if (s == 17) {
        uint32_t rate = a[1] ? a[1] : b.rate;
        if (rate < 100 || rate > 200000) {
            *hr = invalid;
            return 1;
        }
        b.frequency = rate;
        return 1;
    }
    if (s == 18) {
        if (b.playing) {
            b.playing = false;
            stopped(b);
        }
        return error[0] ? -1 : 1;
    }
    if (s == 19) {
        if (!b.locked || std::memcmp(a + 1, b.lock, 16)) {
            *hr = invalid;
            return 1;
        }
        b.locked = false;
        return 1;
    }
    if (s == 20)
        return 1;
    return fail("unsupported buffer method");
}
extern "C" void ac1_ds8_shutdown(void) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto &o : objects) {
        cb.guest_free(ctx, o.second.table);
        cb.guest_free(ctx, o.first);
    }
    objects.clear();
    cb = {};
    ctx = nullptr;
}
extern "C" const char *ac1_ds8_last_error(void) { return error; }
