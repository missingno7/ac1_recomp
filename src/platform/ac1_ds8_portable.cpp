/* Portable DirectSound mixer. Sample ownership and notification servicing
 * follow recomp-kit dx/dsound.cpp, Copyright (c) 2026 veritr1x, MIT.
 * See LICENSE.recomp-kit.txt.
 * All guest access is through the existing bridge; no native COM pointers leak. */
#include "ac1_ds8_bridge.h"
#include "ac1_audio_mixer.h"
#include "ac1_bink_native.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
    // Published only at Unlock, under the bridge mutex and guest baton. The
    // audio thread never resolves or touches the writable guest allocation.
    std::vector<uint8_t> pcm;
    ~Samples();
};
struct Buffer {
    std::shared_ptr<Samples> samples;
    std::array<uint8_t, 18> format{};
    uint32_t flags = 0, rate = 44100, frequency = 44100, align = 4;
    int32_t volume = 0, pan = 0;
    bool playing = false, looping = false, locked = false;
    bool resources_acquired = false;
    uint32_t lock[4]{};
    uint64_t last_ns = 0, frames = 0, remainder = 0;
    double render_frame = 0;
    struct Span {
        uint64_t begin, end;
        double source, step;
    };
    std::array<Span, 128> spans{};
    unsigned span_head = 0, span_count = 0;
    uint64_t visit = 0;
    bool stop_pending = false;
    std::vector<uint8_t> notify_pending;
    std::array<uint32_t, 16> spatial{}, pending{};
    std::array<uint32_t, 16> listener{}, pending_listener{};
    std::vector<std::pair<uint32_t, uint32_t>> notifications;
    Buffer() {
        uint16_t fmt[] = {1, 2, 44100, 0, 45328, 2, 4, 16, 0};
        std::memcpy(format.data(), fmt, 18);
        spatial[0] = 64;
        spatial[7] = spatial[8] = 360;
        float one = 1, billion = 1e9f;
        std::memcpy(&spatial[13], &one, 4);
        std::memcpy(&spatial[14], &billion, 4);
        listener[0] = 64;
        std::memcpy(&listener[9], &one, 4);
        std::memcpy(&listener[11], &one, 4);
        for (unsigned i = 13; i < 16; ++i)
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
bool output_active = false, output_paused = false;
uint64_t visit = 0;
const char *mixer_fault = nullptr; // Set on the audio thread; reported on guest service.
struct BinkTrack {
    uint32_t rate = 0, channels = 0;
    uint64_t submitted = 0;
    std::vector<int16_t> pcm; // Four seconds; allocated only on the guest thread.
};
struct BinkRecord {
    bool paused = false;
    double seconds = 0;
    uint64_t last_ns = 0;
    std::map<uint32_t, BinkTrack> tracks;
};
std::map<uint32_t, BinkRecord> movies;
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
void stopped(Buffer &b, bool audio = false) {
    b.resources_acquired = false;
    if (audio) {
        b.stop_pending = true;
        return;
    }
    for (auto n : b.notifications)
        if (n.first == UINT32_MAX)
            signal(n.second);
}
void advance(Buffer &b, uint64_t after, bool audio) {
    uint64_t total = b.samples ? b.samples->bytes / b.align : 0;
    if (!total)
        return;
    uint64_t before = b.frames;
    if (!b.looping)
        after = std::min(after, total);
    for (size_t i = 0; i < b.notifications.size(); ++i) {
        auto n = b.notifications[i];
        if (n.first != UINT32_MAX) {
            uint64_t pos = n.first / b.align, next = (before / total) * total + pos;
            if (next <= before)
                next += total;
            if (next <= after) {
                if (audio)
                    b.notify_pending[i] = 1;
                else
                    signal(n.second);
            }
        }
    }
    b.frames = after;
    if (!b.looping && after == total) {
        b.playing = false;
        stopped(b, audio);
    }
}
void clock_update(Buffer &b, bool defer_events) {
    uint64_t t = now(), dt = t - b.last_ns;
    b.last_ns = t;
    if (!b.playing || output_active || output_paused)
        return;
    uint64_t scaled = (dt % 1000000000) * b.frequency + b.remainder;
    uint64_t count = dt / 1000000000 * b.frequency + scaled / 1000000000;
    b.remainder = scaled % 1000000000;
    advance(b, b.frames + count, defer_events);
    b.render_frame = double(b.frames);
}
void update(Buffer &b) {
    for (size_t i = 0; i < b.notify_pending.size(); ++i)
        if (b.notify_pending[i]) {
            signal(b.notifications[i].second);
            b.notify_pending[i] = 0;
        }
    if (b.stop_pending) {
        stopped(b);
        b.stop_pending = false;
    }
    clock_update(b, false);
}
void reset_render(Buffer &b) {
    b.span_head = b.span_count = 0;
    b.render_frame = double(b.frames);
}
template <class F> void each_buffer(F fn) {
    ++visit;
    for (auto &o : objects)
        if (o.second.buffer && o.second.buffer->visit != visit) {
            o.second.buffer->visit = visit;
            fn(*o.second.buffer);
        }
}
void consume(uint64_t presented) {
    each_buffer([&](Buffer &b) {
        while (b.span_count) {
            auto &s = b.spans[b.span_head];
            if (presented <= s.begin)
                break;
            double source = s.source + double(std::min(presented, s.end) - s.begin) * s.step;
            if (b.playing)
                advance(b, uint64_t(source + 1e-7), true);
            if (presented < s.end)
                break;
            b.span_head = (b.span_head + 1) % b.spans.size();
            --b.span_count;
        }
    });
}
float scalar(const std::array<uint32_t, 16> &v, unsigned i) {
    float f;
    std::memcpy(&f, &v[i], 4);
    return f;
}
bool valid3d(const std::array<uint32_t, 16> &v, bool listener) {
    for (unsigned i = 1; i < 16; ++i)
        if ((listener || i <= 6 || (i >= 9 && i <= 11) || i == 13 || i == 14) &&
            !std::isfinite(scalar(v, i)))
            return false;
    if (listener)
        return scalar(v, 13) > 0 && scalar(v, 14) >= 0 && scalar(v, 14) <= 10 &&
               scalar(v, 15) >= 0 && scalar(v, 15) <= 10;
    return v[7] <= 360 && v[8] <= 360 && int32_t(v[12]) >= -10000 && int32_t(v[12]) <= 0 &&
           scalar(v, 13) > 0 && scalar(v, 14) >= scalar(v, 13) && v[15] <= 2;
}
float db(int32_t hundredths) {
    return hundredths <= -10000 ? 0.f : std::pow(10.f, float(hundredths) / 2000.f);
}
void gains(const Buffer &b, const std::array<uint32_t, 16> &listener, float &l, float &r) {
    float gain = db(b.volume), pan = float(b.pan) / 10000.f;
    if ((b.flags & 0x10) && b.spatial[15] != 2) { // NORMAL / HEADRELATIVE / DISABLE.
        double delta[3];
        for (unsigned i = 0; i < 3; ++i)
            delta[i] =
                double(scalar(b.spatial, 1 + i)) - (b.spatial[15] == 1 ? 0. : scalar(listener, 1 + i));
        double distance = std::hypot(delta[0],delta[1],delta[2]);
        double minimum = scalar(b.spatial, 13), maximum = scalar(b.spatial, 14);
        // DS distances are in world units; distance factor converts BOTH the
        // minimum distance and displacement to metres and cancels in the ratio.
        if ((b.flags & 0x20000) && distance >= maximum)
            gain = 0;
        double effective =
            minimum + (std::clamp(distance, minimum, maximum) - minimum) * scalar(listener, 14);
        gain *= minimum / std::max(minimum, effective);
        // Listener right = up cross front (DirectSound's left-handed space).
        double right[3] = {1, 0, 0};
        if (b.spatial[15] == 0) {
            double f[3], u[3];
            for (unsigned i = 0; i < 3; ++i) {
                f[i] = scalar(listener, 7 + i);
                u[i] = scalar(listener, 10 + i);
            }
            right[0] = u[1] * f[2] - u[2] * f[1];
            right[1] = u[2] * f[0] - u[0] * f[2];
            right[2] = u[0] * f[1] - u[1] * f[0];
        }
        double length = std::hypot(right[0],right[1],right[2]);
        pan = distance > 0 && length > 0
                  ? (delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2]) /
                        (distance * length)
                  : 0;
    }
    pan = std::clamp(pan, -1.f, 1.f);
    l = gain * db(int32_t(-10000 * std::max(0.f, pan)));
    r = gain * db(int32_t(10000 * std::min(0.f, pan)));
}
float sample(const Buffer &b, uint64_t frame, unsigned channel) {
    const unsigned channels = b.format[2], bits = b.format[14];
    size_t index = size_t(frame) * b.align + (channels == 1 ? 0 : channel) * bits / 8;
    const uint8_t *p = b.samples->pcm.data() + index;
    if (bits == 8)
        return (int(*p) - 128) / 128.f;
    int16_t value;
    std::memcpy(&value, p, 2);
    return value / 32768.f;
}
void movie_clock(BinkRecord &m) {
    uint64_t t = now();
    if (!output_active && !output_paused && !m.paused)
        m.seconds += double(t - m.last_ns) / 1e9;
    m.last_ns = t;
}
bool setformat(Buffer &b, uint32_t address) {
    std::array<uint8_t, 18> f;
    if (!read(address, f.data(), 18)) {
        fail("WAVEFORMATEX input unmapped (18 bytes required)");
        return false;
    }
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
        !rate || rate > 200000 || bytes != rate * align || extra) {
        char message[224];
        std::snprintf(message, sizeof(message),
                      "unsupported PCM format: tag=%u channels=%u bits=%u rate=%u byteRate=%u "
                      "align=%u cbSize=%u; require tag=1, mono/stereo 8/16-bit, rate 1..200000, "
                      "exact byte/block rate, cbSize=0",
                      tag, ch, bits, rate, bytes, align, extra);
        fail(message);
        return false;
    }
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
                                        {0x6825a449, 0x4d827524, 0xe3500f92, 0x1eabb36a},
                                        {0x279afa86, 0x11ce4981, 0x200021a5, 0x60e50baf},
                                        {0xb0210783, 0x11d089cd, 0xa00008af, 0x16cd25c9}};
    for (unsigned i = 0; i < 7; ++i)
        if (!std::memcmp(g, guids[i], 16))
            return i + 1;
    return 0;
}
} // namespace
extern "C" void ac1_audio_output_active(int active) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    each_buffer([&](Buffer &b) {
        clock_update(b, true);
        reset_render(b);
    });
    for (auto &m : movies)
        movie_clock(m.second);
    output_active = active != 0;
}
extern "C" void ac1_audio_output_paused(int paused) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    each_buffer([](Buffer &b) {
        // Lifecycle calls have no guest baton. Leave pending notifications for
        // service; never call update() or other guest callbacks here.
        clock_update(b, true);
    });
    for (auto &m : movies)
        movie_clock(m.second);
    output_paused = paused != 0;
}
extern "C" void ac1_audio_consume(uint64_t presented) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    consume(presented);
}
extern "C" void ac1_audio_render(float *out, uint32_t count, uint32_t rate, uint64_t first,
                                 uint64_t presented) {
    std::fill_n(out, size_t(count) * 2, 0.f);
    std::unique_lock<std::recursive_mutex> lock(mutex, std::try_to_lock);
    if (!lock || !output_active || output_paused || !rate)
        return;
    consume(presented);
    float master = 1;
    const std::array<uint32_t, 16> *listener = nullptr;
    each_buffer([&](Buffer &b) {
        if (b.flags & 1) {
            master = db(b.volume);
            listener = &b.listener;
        }
    });
    each_buffer([&](Buffer &b) {
        if (!b.playing)
            return;
        uint64_t total = b.samples->bytes / b.align;
        if (!b.looping && b.render_frame >= total)
            return;
        if (b.span_count == b.spans.size()) {
            mixer_fault = "consumption history exhausted (128 outstanding parameter changes)";
            return;
        }
        double start = b.render_frame, step = double(b.frequency) / rate;
        float left, right;
        gains(b, listener ? *listener : b.listener, left, right);
        uint32_t rendered = 0;
        for (; rendered < count; ++rendered) {
            double position = start + rendered * step;
            if (!b.looping && position >= total)
                break;
            if (b.flags & 1)
                continue; // Primary cursors still follow device consumption.
            uint64_t frame = uint64_t(position), a = frame % total;
            uint64_t next = b.looping ? (a + 1) % total : std::min(a + 1, total - 1);
            float fraction = float(position - frame);
            float l = sample(b, a, 0), r = sample(b, a, 1);
            out[rendered * 2] += (l + (sample(b, next, 0) - l) * fraction) * left;
            out[rendered * 2 + 1] += (r + (sample(b, next, 1) - r) * fraction) * right;
        }
        b.render_frame = start + rendered * step;
        if (rendered) {
            if (b.span_count) {
                auto &last = b.spans[(b.span_head + b.span_count - 1) % b.spans.size()];
                if (last.end == first && last.step == step &&
                    std::abs(last.source + double(last.end - last.begin) * step - start) < 1e-6) {
                    last.end += rendered;
                    return;
                }
            }
            b.spans[(b.span_head + b.span_count) % b.spans.size()] = {first, first + rendered,
                                                                      start, step};
            ++b.span_count;
        }
    });
    for (auto &entry : movies) {
        auto &m = entry.second;
        if (m.paused)
            continue;
        for (auto &track : m.tracks) {
            auto &v = track.second;
            uint64_t capacity = v.pcm.size() / v.channels;
            for (uint32_t i = 0; i < count; ++i) {
                double pos = (m.seconds + double(i) / rate) * v.rate;
                uint64_t a = uint64_t(pos + 1e-7);
                if (a >= v.submitted || v.submitted - a > capacity)
                    continue; // underrun / late track
                uint64_t next = std::min(a + 1, v.submitted - 1);
                float fraction = float(std::max(0., pos - double(a)));
                for (unsigned ch = 0; ch < 2; ++ch) {
                    unsigned c = v.channels == 1 ? 0 : ch;
                    float x = v.pcm[(a % capacity) * v.channels + c] / 32768.f;
                    float y = v.pcm[(next % capacity) * v.channels + c] / 32768.f;
                    out[i * 2 + ch] += x + (y - x) * fraction;
                }
            }
        }
        m.seconds += double(count) / rate;
    }
    for (size_t i = 0; i < size_t(count) * 2; ++i)
        out[i] = std::clamp(out[i] * master, -1.f, 1.f);
}
extern "C" int ac1_audio_bink_submit(uint32_t record, uint32_t track, const int16_t *pcm,
                                     size_t frames, uint32_t rate, uint32_t channels,
                                     uint64_t pts) {
#ifdef __ANDROID__
    ac1_audio_device_service();
#endif
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!record || !pcm || !frames || rate < 100 || rate > 200000 ||
        (channels != 1 && channels != 2) || frames > size_t(rate) * 4) {
        fail("Bink PCM requires record, nonempty s16 mono/stereo, 100..200000 Hz, chunk <=4 "
             "seconds");
        return 0;
    }
    if (!movies.count(record) && movies.size() >= 16) {
        fail("Bink mixer record limit (16)");
        return 0;
    }
#ifndef __ANDROID__
    if (!output_active && movies.empty())
        std::fprintf(
            stderr,
            "AC1 portable Bink audio: no device adapter; explicit clock-driven silent fallback\n");
#endif
    auto &m = movies[record];
    if (!m.last_ns)
        m.last_ns = now();
    movie_clock(m);
    if (!m.tracks.count(track) && m.tracks.size() >= 8) {
        fail("Bink mixer track limit (8 per record)");
        return 0;
    }
    auto &v = m.tracks[track];
    if (!v.rate) {
        v.rate = rate;
        v.channels = channels;
        v.pcm.resize(size_t(rate) * channels * 4);
    }
    if (v.rate != rate || v.channels != channels || pts != v.submitted * 1000000 / rate) {
        fail("Bink PCM format changed or noncontiguous PTS (STOP required before rewind)");
        return 0;
    }
    uint64_t retired = uint64_t(m.seconds * rate), end = v.submitted + frames;
    // Activity pause does not suspend the guest video clock. Retire PCM that
    // continues to be decoded in the background, rather than growing a stale
    // queue or killing playback after four seconds. Explicit BinkPause still
    // retains its queue because the decoder stops submitting while paused.
    if (output_paused) {
        m.seconds = std::max(m.seconds, double(end) / rate);
        retired = std::max(retired, end);
    }
    if (end > retired && end - retired > uint64_t(rate) * 4) {
        fail("Bink mixer queue exceeds four seconds; output is not consuming PCM");
        return 0;
    }
    for (size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < channels; ++c)
            v.pcm[((v.submitted + i) % (uint64_t(rate) * 4)) * channels + c] =
                pcm[i * channels + c];
    v.submitted = end;
    return 1;
}
extern "C" void ac1_audio_bink_control(uint32_t record, int event) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = movies.find(record);
    if (event != AC1_BINK_AUDIO_STOP && event != AC1_BINK_AUDIO_PAUSE &&
        event != AC1_BINK_AUDIO_RESUME) {
        mixer_fault = "unknown Bink audio control event";
        return;
    }
    if (it == movies.end())
        return;
    movie_clock(it->second);
    if (event == AC1_BINK_AUDIO_STOP)
        movies.erase(it);
    else
        it->second.paused = event == AC1_BINK_AUDIO_PAUSE;
}
extern "C" int ac1_ds8_configure(const ac1_ds8_callbacks *c, void *p) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!c || c->struct_size < sizeof(*c) || !c->guest_alloc || !c->guest_free ||
        !c->guest_resolve || !objects.empty())
        return 0;
    cb = *c;
    ctx = p;
    error[0] = 0;
    speaker = 4;
    mixer_fault = nullptr;
    return 1;
}
extern "C" int ac1_ds8_create8(uint32_t guid, uint32_t out, uint32_t outer, uint32_t *hr) {
#ifdef __ANDROID__
    ac1_audio_device_service();
#endif
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
    std::fprintf(stderr, "AC1 DirectSound: portable PCM mixer (%s)\n",
                 output_active ? "device consumption cursors" : "no device; clock fallback");
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
    if (k == 5 && s == 22)
        return 4; // AcquireResources(this, flags, effect count, result codes).
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
#ifdef __ANDROID__
    ac1_audio_device_service();
#endif
    std::lock_guard<std::recursive_mutex> lock(mutex);
    error[0] = 0;
    if (mixer_fault)
        return fail(mixer_fault);
    // Aliases share a Buffer; update's elapsed time is zero on subsequent aliases.
    for (auto &o : objects)
        if (o.second.buffer)
            update(*o.second.buffer);
    return error[0] ? -1 : 1;
}
extern "C" int ac1_ds8_dispatch(uint32_t t, const uint32_t *a, uint32_t n, uint32_t *hr) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    error[0] = 0;
    if (mixer_fault)
        return fail(mixer_fault);
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
            if (d[4] && !setformat(*b, d[4]))
                return -1;
            if ((d[1] & ~0x0007c3ffu) || (d[1] & (0x4u | 0x200u))) {
                char message[160];
                std::snprintf(message, sizeof(message),
                              "CreateSoundBuffer flags=0x%08x: unknown capability, hardware-only "
                              "or CTRLFX unsupported",
                              d[1]);
                return fail(message);
            }
            if ((d[1] & 0x10) && !(d[1] & 1) && b->format[2] != 1)
                return fail("CreateSoundBuffer CTRL3D requires mono PCM");
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
            b->samples->pcm.resize(bytes, b->format[14] == 8 ? 128 : 0);
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
            b->resources_acquired = false;
            b->frames = b->remainder = 0;
            b->notifications.clear();
            b->notify_pending.clear();
            b->stop_pending = false;
            reset_render(*b);
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
    if (k == 5 && s == 22) {
        if (a[1] || a[2])
            return fail("AcquireResources supports only flags=0 and zero effects");
        if (!(o.buffer->flags & 0x40000u)) {
            *hr = 0x88780032; // DSERR_INVALIDCALL: not a LOCDEFER buffer (native oracle).
            return 1;
        }
        // Software PCM storage was acquired when the buffer was created. No FX
        // resources or result-code entries exist for this no-effects request.
        o.buffer->resources_acquired = true;
        return 1;
    }
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
        b.notify_pending.assign(b.notifications.size(), 0);
        return 1;
    }
    if (k == 4 || k == 6) {
        uint32_t *current = k == 4 ? b.listener.data() : b.spatial.data();
        uint32_t *pending = k == 4 ? b.pending_listener.data() : b.pending.data();
        unsigned size = 64; // Both DS3DLISTENER and DS3DBUFFER are pack-4, 64 bytes.
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
            std::array<uint32_t, 16> data;
            if (!read(a[1], data.data(), size) || data[0] != size)
                return fail("3D parameter size/range invalid");
            if (a[2] > 1 || !valid3d(data, k == 4))
                return fail("3D parameters require finite values, positive ordered min/max "
                            "distance, mode 0..2, rolloff/Doppler 0..10, apply 0..1");
            std::memcpy(pending, data.data(), size);
            if (!a[2])
                std::memcpy(current, data.data(), size);
            return 1;
        }
        // Getters and setters use the documented pack-4 DS3D structures.
        static const unsigned loff[] = {13, 15, 7, 1, 14, 4};        // listener getters 4..9
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
            std::array<uint32_t, 16> candidate;
            std::memcpy(candidate.data(), pending, size);
            std::memcpy(candidate.data() + off, a + 1, words * 4);
            if (!valid3d(candidate, k == 4))
                return fail("3D setter requires finite values, positive ordered min/max distance, "
                            "mode 0..2 and rolloff/Doppler 0..10");
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
                                  : (b.playing ? 1u : 0u) | (b.playing && b.looping ? 4u : 0u) |
                                    ((b.flags & 0x40000u) && b.resources_acquired ? 0x10u : 0u));
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
            return fail("Lock flags: only FROMWRITECURSOR=1 and ENTIREBUFFER=2 supported");
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
        const uint32_t flags = a[3];
        const bool deferred = (b.flags & 0x40000u) != 0;
        if (a[1] || (flags & ~0x3fu) || (!deferred && (a[2] || (flags & ~1u))) ||
            (flags & 6u) == 6u || (flags & 24u) == 24u) {
            std::fprintf(
                stderr,
                "AC1 DirectSound Play rejected: reserved=%u priority=%u flags=0x%08x LOCDEFER=%u "
                "(allowed flags 0x3f, exclusive allocation/termination hints)\n",
                a[1], a[2], flags, unsigned(deferred));
            fail("Play: reserved must be zero; priority/hints require LOCDEFER; allocation and "
                 "termination pairs are exclusive; flags outside 0x3f unsupported");
            *hr = invalid;
            return 1;
        }
        if (flags & 2u) { // Hardware-only allocation is unavailable on this software backend.
            fail("Play LOCHARDWARE unsupported: only software mixing resources available");
            *hr = 0x88780032; // Matches native Windows without hardware mixing resources.
            return 1;
        }
        // Software voices already own PCM storage; no hardware voice must be
        // evicted for the valid deferred-buffer priority/termination hints.
        if (!b.playing)
            for (auto pos : b.notifications)
                if (pos.first == cursor)
                    signal(pos.second);
        if (!b.playing) {
            b.frames %= bytes / b.align;
            reset_render(b);
        }
        b.playing = true;
        b.resources_acquired = true;
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
        reset_render(b);
        return 1;
    }
    if (s == 14) {
        if (!(b.flags & 1))
            *hr = 0x88780064;
        else if (!setformat(b, a[1]))
            return -1;
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
        b.resources_acquired = false;
        if (b.playing) {
            b.playing = false;
            stopped(b);
        }
        reset_render(b);
        return error[0] ? -1 : 1;
    }
    if (s == 19) {
        if (!b.locked || std::memcmp(a + 1, b.lock, 16)) {
            *hr = invalid;
            return 1;
        }
        for (unsigned i : {0u, 2u})
            if (b.lock[i + 1] &&
                !read(b.lock[i], b.samples->pcm.data() + b.lock[i] - b.samples->address,
                      b.lock[i + 1]))
                return fail("Unlock PCM range unmapped during snapshot");
        b.locked = false;
        return 1;
    }
    if (s == 20)
        return 1;
    return fail("unsupported buffer method");
}
extern "C" void ac1_ds8_shutdown(void) {
#ifdef __ANDROID__
    ac1_audio_device_shutdown();
#endif
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto &o : objects) {
        cb.guest_free(ctx, o.second.table);
        cb.guest_free(ctx, o.first);
    }
    objects.clear();
    movies.clear();
    output_active = false;
    mixer_fault = nullptr;
    cb = {};
    ctx = nullptr;
}
extern "C" const char *ac1_ds8_last_error(void) { return error; }
