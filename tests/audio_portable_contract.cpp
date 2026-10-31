/* No device, assets or translated code. Drive real COM tokens plus a fake
 * output with independent render/presentation heads. Windows-buildable. */
#include "../src/platform/ac1_ds8_bridge.h"
#include "../src/platform/ac1_audio_mixer.h"
#include "../src/platform/ac1_bink_native.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>
namespace {
std::array<uint8_t, 16 * 1024 * 1024> ram{};
uint32_t next_address = 4096;
uint64_t clock_ns = 1000000000;
thread_local bool audio_thread;
std::vector<uint32_t> events;
void *resolve(void *, uint32_t a, uint32_t n, uint32_t) {
    assert(!audio_thread);
    return a && uint64_t(a) + n <= ram.size() ? ram.data() + a : nullptr;
}
uint32_t alloc(void *, uint32_t bytes, uint32_t alignment) {
    assert(!audio_thread);
    next_address = (next_address + alignment - 1) & ~(alignment - 1);
    uint32_t a = next_address;
    next_address += bytes;
    assert(next_address < ram.size());
    return a;
}
void free_guest(void *, uint32_t) {
    assert(!audio_thread);
}
int signal(void *, uint32_t e) {
    assert(!audio_thread);
    events.push_back(e);
    return 1;
}
uint64_t now(void *) {
    assert(!audio_thread);
    return clock_ns;
}
uint32_t get(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, ram.data() + a, 4);
    return v;
}
uint32_t data(std::initializer_list<uint32_t> v) {
    uint32_t a = alloc(nullptr, uint32_t(v.size() * 4), 4);
    std::memcpy(ram.data() + a, v.begin(), v.size() * 4);
    return a;
}
uint32_t call(uint32_t object, unsigned slot, std::initializer_list<uint32_t> args = {}) {
    std::vector<uint32_t> a{object};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t hr = ~0u, t = get(get(object) + slot * 4);
    if (ac1_ds8_argument_count(t) != int(a.size()))
        std::fprintf(stderr, "arity slot=%u token=%08x expected=%d actual=%zu\n", slot, t,
                     ac1_ds8_argument_count(t), a.size());
    assert(ac1_ds8_argument_count(t) == int(a.size()));
    int rc = ac1_ds8_dispatch(t, a.data(), uint32_t(a.size()), &hr);
    if (rc != 1)
        std::fprintf(stderr, "slot %u: %s\n", slot, ac1_ds8_last_error());
    assert(rc == 1);
    return hr;
}
uint32_t out, io, dev;
uint32_t qi(uint32_t object, std::initializer_list<uint32_t> guid) {
    assert(!call(object, 0, {data(guid), out}));
    return get(out);
}
uint32_t buffer(uint32_t rate, uint32_t channels, uint32_t bits, uint32_t count,
                uint32_t flags = 0xe0) {
    uint32_t align = channels * bits / 8;
    uint32_t fmt = data({1 | (channels << 16), rate, rate * align, align | (bits << 16), 0});
    uint32_t desc = data({36, flags, count * align, 0, fmt, 0, 0, 0, 0});
    assert(!call(dev, 3, {desc, out, 0}));
    return get(out);
}
void upload(uint32_t b, const void *pcm, uint32_t bytes, bool unlock = true) {
    assert(!call(b, 11, {0, bytes, io, io + 4, io + 8, io + 12, 0}));
    std::memcpy(ram.data() + get(io), pcm, bytes);
    if (unlock)
        assert(!call(b, 19, {get(io), get(io + 4), get(io + 8), get(io + 12)}));
}
void close_buffer(uint32_t b) {
    call(b, 18);
    assert(call(b, 2) == 0);
}
void near(float actual, float expected) {
    if (std::abs(actual - expected) > 2e-5f)
        std::fprintf(stderr, "actual=%f expected=%f\n", actual, expected);
    assert(std::abs(actual - expected) < 2e-5f);
}
uint64_t head;
std::vector<float> render(uint32_t count, uint32_t rate = 1000, uint64_t consumed = UINT64_MAX) {
    std::vector<float> pcm(count * 2);
    audio_thread = true;
    ac1_audio_render(pcm.data(), count, rate, head, consumed == UINT64_MAX ? head : consumed);
    audio_thread = false;
    head += count;
    return pcm;
}
void consume(uint64_t frame) {
    audio_thread = true;
    ac1_audio_consume(frame);
    audio_thread = false;
}
uint32_t cursor(uint32_t b) {
    assert(!call(b, 4, {out, 0}));
    return get(out);
}
void start(uint32_t b, bool loop = true) {
    assert(!call(b, 12, {0, 0, loop ? 1u : 0u}));
}
uint32_t bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
void consumption() {
    uint32_t b = buffer(1000, 1, 16, 8);
    int16_t pcm[] = {0, 8192, 16384, 24576, 0, -8192, -16384, -24576};
    upload(b, pcm, sizeof pcm);
    uint32_t notify = qi(b, {0xb0210783, 0x11d089cd, 0xa00008af, 0x16cd25c9});
    assert(!call(notify, 3, {2, data({4, 101, UINT32_MAX, 102})}));
    start(b, false);
    auto mixed = render(4, 1000, 0);
    near(mixed[0], 0);
    near(mixed[2], .25f);
    near(mixed[4], .5f);
    near(mixed[6], .75f);
    clock_ns += 1000000000;
    assert(cursor(b) == 0 && events.empty()); // Generated is not presented; clock cannot move it.
    consume(head - 2);
    assert(events.empty());
    // The guest service delivers the event the audio thread deferred, once.
    assert(ac1_ds8_service() == 1 && events.size() == 1 && events.back() == 101);
    assert(cursor(b) == 4 && events.size() == 1 && events.back() == 101);
    render(4, 1000, head);
    consume(head);
    assert(!call(b, 9, {out}) && !(get(out) & 1));
    assert(events.size() == 2 && events.back() == 102 && cursor(b) == 0);
    start(b);
    render(20);
    consume(head);
    assert(cursor(b) == 8); // More than two wraps.
    assert(!call(b, 13, {2}));
    assert(cursor(b) == 2);
    render(1);
    consume(head);
    assert(cursor(b) == 4);
    // Frequency changes split the consumption history without retroactively
    // changing the rate of audio already queued in the output device.
    assert(!call(b, 13, {0}));
    uint64_t origin = head;
    render(2, 1000, origin);
    assert(!call(b, 17, {2000}));
    render(2, 1000, origin);
    consume(origin + 2);
    assert(cursor(b) == 4);
    consume(origin + 3);
    assert(cursor(b) == 8);
    consume(origin + 4);
    assert(cursor(b) == 12);
    uint32_t desc = data({36, 1, 0, 0, 0, 0, 0, 0, 0});
    assert(!call(dev, 3, {desc, out, 0}));
    uint32_t primary = get(out);
    start(primary);
    render(10);
    consume(head);
    assert(cursor(primary) == 1764); // 44100 source frames/s, stereo s16, 10ms.
    close_buffer(primary);
    assert(call(notify, 2) == 0);
    close_buffer(b);
}
void mixing() {
    uint32_t b = buffer(1000, 1, 16, 4);
    int16_t pcm[] = {0, 16384, 0, -16384};
    upload(b, pcm, sizeof pcm);
    start(b);
    assert(!call(b, 17, {500}));
    auto v = render(4);
    near(v[0], 0);
    near(v[2], .25f);
    near(v[4], .5f);
    near(v[6], .25f);
    consume(head);
    assert(cursor(b) == 4);
    assert(!call(b, 13, {0}));
    assert(!call(b, 15, {uint32_t(-2000)}));
    assert(!call(b, 16, {2000}));
    v = render(4);
    near(v[4], .005f);
    near(v[5], .05f);
    // Primary gain applies to DS and Bink at the final summation stage.
    uint32_t desc = data({36, 1, 0, 0, 0, 0, 0, 0, 0});
    assert(!call(dev, 3, {desc, out, 0}));
    uint32_t primary = get(out);
    assert(!call(primary, 15, {uint32_t(-2000)}));
    assert(!call(b, 13, {0}));
    v = render(4);
    near(v[4], .0005f);
    near(v[5], .005f);
    close_buffer(primary);
    close_buffer(b);
    uint32_t a = buffer(1000, 1, 8, 4), stereo = buffer(1000, 2, 16, 4);
    uint8_t p8[] = {192, 192, 192, 192};
    int16_t p16[] = {8192, -8192, 8192, -8192, 8192, -8192, 8192, -8192};
    upload(a, p8, sizeof p8);
    upload(stereo, p16, sizeof p16);
    start(a);
    start(stereo);
    v = render(2);
    near(v[0], .75f);
    near(v[1], .25f);
    assert(!call(a, 16, {10000}));
    v = render(1);
    near(v[0], .25f);
    near(v[1], .25f);
    assert(!call(a, 15, {uint32_t(-10000)}));
    v = render(1);
    near(v[0], .25f);
    near(v[1], -.25f);
    close_buffer(stereo);
    // Unlocked PCM remains audible while a guest writer holds Lock. Neither
    // the writer's in-progress data nor direct post-Unlock writes leak through.
    uint8_t fresh[] = {128, 128, 128, 128};
    assert(!call(a, 15, {0}));
    assert(!call(a, 16, {0}));
    upload(a, fresh, sizeof fresh, false);
    v = render(1);
    near(v[0], .5f);
    assert(!call(a, 19, {get(io), get(io + 4), get(io + 8), get(io + 12)}));
    std::memset(ram.data() + get(io), 255, 4);
    v = render(1);
    near(v[0], 0);
    // Duplicate storage is published once for all voices; release order is safe.
    assert(!call(dev, 5, {a, out}));
    uint32_t duplicate = get(out);
    uint8_t new_pcm[] = {160, 160, 160, 160};
    upload(duplicate, new_pcm, 4);
    start(duplicate);
    v = render(1);
    near(v[0], .5f);
    close_buffer(a);
    v = render(1);
    near(v[0], .25f);
    close_buffer(duplicate);
    uint32_t s8 = buffer(2000, 2, 8, 2);
    uint8_t stereo8[] = {192, 64, 192, 64};
    upload(s8, stereo8, sizeof stereo8);
    start(s8);
    v = render(1);
    near(v[0], .5f);
    near(v[1], -.5f);
    close_buffer(s8);
}
void voice_registry() {
    uint32_t b = buffer(1000, 1, 16, 8);
    int16_t pcm[8];
    std::fill(std::begin(pcm), std::end(pcm), 16384);
    upload(b, pcm, sizeof pcm);
    start(b);
    near(render(1)[0], .5f);
    assert(!call(b, 18));
    near(render(1)[0], 0.f); // Stop removes the voice from callback iteration.
    start(b);
    near(render(1)[0], .5f); // Play registers it again.
    assert(!call(dev, 5, {b, out}));
    uint32_t duplicate = get(out);
    start(duplicate);
    near(render(1)[0], 1.f); // Duplicate voices own separate mixer cursors.
    close_buffer(b);
    near(render(1)[0], .5f); // Releasing one alias removes only that voice.
    assert(!call(duplicate, 18));
    near(render(1)[0], 0.f);
    start(duplicate);
    near(render(1)[0], .5f);
    close_buffer(duplicate);
}
void spatial() {
    uint32_t desc = data({36, 0x11, 0, 0, 0, 0, 0, 0, 0});
    assert(!call(dev, 3, {desc, out, 0}));
    uint32_t primary = get(out);
    uint32_t listener = qi(primary, {0x279afa84, 0x11ce4981, 0x200021a5, 0x60e50baf});
    uint32_t b = buffer(1000, 1, 16, 4, 0x90);
    int16_t pcm[] = {16384, 16384, 16384, 16384};
    upload(b, pcm, sizeof pcm);
    uint32_t spatial = qi(b, {0x279afa86, 0x11ce4981, 0x200021a5, 0x60e50baf});
    start(b);
    assert(!call(spatial, 17, {bits(1), 0}));       // min distance
    assert(!call(spatial, 16, {bits(4), 0}));       // max distance
    assert(!call(spatial, 19, {0, 0, bits(2), 0})); // position 2m in front
    auto v = render(1);
    near(v[0], .25f);
    near(v[1], .25f);
    assert(!call(listener, 15, {bits(2), 0}));
    v = render(1);
    near(v[0], 1.f / 6);
    near(v[1], 1.f / 6);
    assert(!call(listener, 15, {bits(1), 0}));
    assert(!call(listener, 14, {0, 0, bits(1), 0}));
    v = render(1);
    near(v[0], .5f); // listener position
    assert(!call(listener, 14, {0, 0, 0, 0}));
    assert(!call(listener, 11, {bits(2), 0}));
    v = render(1);
    near(v[0], .25f); // world-unit factor cancels
    assert(!call(spatial, 19, {0, 0, bits(100), 0}));
    v = render(1);
    near(v[0], .125f); // clamped at max
    assert(!call(listener, 15, {0, 0}));
    v = render(1);
    near(v[0], .5f); // zero rolloff
    assert(!call(listener, 15, {bits(1), 0}));
    assert(!call(spatial, 19, {bits(2), 0, 0, 1}));
    v = render(1);
    near(v[0], .125f); // deferred
    assert(!call(listener, 17));
    v = render(1);
    near(v[0], 0);
    near(v[1], .25f);
    assert(!call(listener, 13, {0, 0, bits(-1), 0, bits(1), 0, 0}));
    v = render(1);
    near(v[0], .25f);
    near(v[1], 0); // rotated listener
    assert(!call(spatial, 18, {1, 0}));
    v = render(1);
    near(v[0], 0);
    near(v[1], .25f); // head-relative
    assert(!call(spatial, 18, {2, 0}));
    v = render(1);
    near(v[0], .5f);
    near(v[1], .5f); // disabled
    assert(call(spatial, 2) == 0);
    close_buffer(b);
    assert(call(listener, 2) == 0);
    close_buffer(primary);
    b = buffer(1000, 1, 16, 4, 0x20090);
    upload(b, pcm, sizeof pcm);
    start(b);
    spatial = qi(b, {0x279afa86, 0x11ce4981, 0x200021a5, 0x60e50baf});
    assert(!call(spatial, 16, {bits(2), 0}));
    assert(!call(spatial, 19, {0, 0, bits(2), 0}));
    v = render(1);
    near(v[0], 0);
    near(v[1], 0);
    assert(call(spatial, 2) == 0);
    close_buffer(b);
}
void bink() {
    int16_t pcm[] = {0, 16384, 0, -16384};
    assert(ac1_audio_bink_submit(1, 10, pcm, 4, 500, 1, 0));
    auto v = render(4);
    near(v[0], 0);
    near(v[2], .25f);
    near(v[4], .5f);
    near(v[6], .25f);
    ac1_audio_bink_control(1, AC1_BINK_AUDIO_PAUSE);
    v = render(4);
    near(v[0], 0);
    ac1_audio_bink_control(1, AC1_BINK_AUDIO_RESUME);
    v = render(2);
    near(v[0], 0);
    near(v[2], -.25f);
    assert(ac1_audio_bink_submit(1, 10, pcm, 4, 500, 1, 8000));
    v = render(4);
    near(v[0], -.5f);
    near(v[2], -.25f);
    near(v[4], 0);
    near(v[6], .25f);
    ac1_audio_bink_control(1, AC1_BINK_AUDIO_STOP);
    v = render(1);
    near(v[0], 0);
    int16_t mono[] = {8192, 8192, 8192, 8192},
            stereo[] = {8192, -8192, 8192, -8192, 8192, -8192, 8192, -8192};
    assert(ac1_audio_bink_submit(2, 0, mono, 4, 1000, 1, 0));
    assert(ac1_audio_bink_submit(2, 1, stereo, 4, 1000, 2, 0));
    uint32_t b = buffer(1000, 1, 16, 4);
    upload(b, mono, sizeof mono);
    start(b);
    v = render(1);
    near(v[0], .75f);
    near(v[1], .25f);
    uint32_t desc = data({36, 1, 0, 0, 0, 0, 0, 0, 0});
    assert(!call(dev, 3, {desc, out, 0}));
    uint32_t primary = get(out);
    assert(!call(primary, 15, {uint32_t(-2000)}));
    v = render(1);
    near(v[0], .075f);
    near(v[1], .025f);
    close_buffer(primary);
    assert(!ac1_audio_bink_submit(2, 0, mono, 4, 1000, 1, 0));
    assert(std::strstr(ac1_ds8_last_error(), "PTS"));
    assert(!ac1_audio_bink_submit(2, 1, mono, 4, 1000, 3, 4000));
    assert(std::strstr(ac1_ds8_last_error(), "mono/stereo"));
    ac1_audio_bink_control(2, AC1_BINK_AUDIO_STOP);
    close_buffer(b);
    // Queued native Bink freezes with Activity pause, then resumes.
    assert(ac1_audio_bink_submit(3, 0, mono, 4, 1000, 1, 0));
    ac1_audio_output_paused(1);
    v = render(8);
    near(v[0], 0);
    ac1_audio_output_paused(0);
    v = render(1);
    near(v[0], .25f);
    ac1_audio_bink_control(3, AC1_BINK_AUDIO_STOP);
    // The guest video clock can continue while the Activity is backgrounded.
    // More than four seconds of new PCM must not overflow the paused sink.
    std::array<int16_t, 1000> second{};
    second.fill(8192);
    ac1_audio_output_paused(1);
    for (unsigned i = 0; i < 8; ++i)
        assert(ac1_audio_bink_submit(4, 0, second.data(), 1000, 1000, 1, uint64_t(i) * 1000000));
    ac1_audio_output_paused(0);
    v = render(1);
    near(v[0], 0); // stale background audio retired
    assert(ac1_audio_bink_submit(4, 0, second.data(), 1000, 1000, 1, 8000000));
    v = render(1);
    near(v[0], .25f);
    ac1_audio_bink_control(4, AC1_BINK_AUDIO_STOP);
}
void concurrency_and_fallback() {
    uint32_t b = buffer(1000, 1, 16, 64);
    std::array<int16_t, 64> pcm{};
    pcm.fill(8192);
    upload(b, pcm.data(), sizeof pcm);
    start(b);
    std::atomic<bool> done{false};
    std::thread output([&] {
        while (!done.load())
            render(8);
    });
    for (unsigned i = 0; i < 200; ++i) {
        upload(b, pcm.data(), sizeof pcm);
        assert(!call(b, 13, {(i % 64) * 2}));
        assert(!call(b, 17, {1000 + i}));
    }
    done.store(true);
    output.join();
    ac1_audio_output_active(0);
    assert(!call(b, 17, {1000}));
    assert(!call(b, 13, {0}));
    clock_ns += 10000000;
    assert(cursor(b) == 20); // Device unavailable clock fallback.
    clock_ns += 10000000;
    ac1_audio_output_paused(1);
    clock_ns += 1000000000;
    assert(cursor(b) == 40);
    ac1_audio_output_paused(0);
    clock_ns += 10000000;
    assert(cursor(b) == 60);
    clock_ns += 10000000;
    ac1_audio_output_active(1);
    head = 0;
    assert(cursor(b) == 80); // Clock progress up to device acquisition is preserved.
    render(4, 1000, 0);
    assert(cursor(b) == 80);
    consume(head);
    assert(cursor(b) == 88);
    render(8, 1000, head); // Simulate a route loss with unconsumed queued output.
    ac1_audio_output_active(0);
    ac1_audio_output_active(1);
    head = 0;
    assert(cursor(b) == 88);
    render(1, 1000, 0);
    consume(head);
    assert(cursor(b) == 90); // New device timeline resumes at the consumed source cursor.
    close_buffer(b);
}
void unsupported() {
    uint32_t fmt = data({3 | (2 << 16), 48000, 384000, 8 | (32 << 16), 0});
    uint32_t desc = data({36, 0x80, 32, 0, fmt, 0, 0, 0, 0});
    uint32_t args[] = {dev, desc, out, 0}, hr;
    assert(ac1_ds8_dispatch(get(get(dev) + 3 * 4), args, 4, &hr) < 0);
    assert(std::strstr(ac1_ds8_last_error(), "tag=3") &&
           std::strstr(ac1_ds8_last_error(), "bits=32"));
    std::array<int16_t, 4000> pcm{};
    assert(ac1_audio_bink_submit(42, 0, pcm.data(), 4000, 1000, 1, 0));
    assert(!ac1_audio_bink_submit(42, 0, pcm.data(), 1000, 1000, 1, 4000000));
    assert(std::strstr(ac1_ds8_last_error(), "four seconds"));
    ac1_audio_bink_control(42, AC1_BINK_AUDIO_STOP);
}
void diagnostics() {
    ac1_audio_stats before{}, after{};
    ac1_audio_get_stats(&before);
    uint32_t b = buffer(1000, 1, 16, 4);
    const int16_t pcm[] = {8192, 8192, 8192, 8192};
    upload(b, pcm, sizeof pcm);
    start(b);
    assert(ac1_audio_bink_submit(99, 0, pcm, 4, 1000, 1, 0));
    auto mixed = render(8, 1000, head);
    for (unsigned i = 0; i < 8; ++i)
        near(mixed[i * 2], i < 4 ? .5f : .25f);
    ac1_audio_get_stats(&after);
    assert(after.render_calls == before.render_calls + 1);
    assert(after.requested_frames == before.requested_frames + 8);
    assert(after.lock_misses == before.lock_misses);
    assert(after.ds_frames == before.ds_frames + 8);
    assert(after.bink_submitted_frames == before.bink_submitted_frames + 4);
    assert(after.bink_frames == before.bink_frames + 4);
    assert(after.bink_underrun_frames == before.bink_underrun_frames + 4);
    assert(after.nonzero_frames == before.nonzero_frames + 8);
    // Taking a snapshot must not consume queued output or advance the play cursor.
    uint32_t position = cursor(b);
    ac1_audio_get_stats(&after);
    assert(cursor(b) == position);
    ac1_audio_bink_control(99, AC1_BINK_AUDIO_STOP);
    close_buffer(b);
}
void benchmark() {
    std::vector<uint32_t> all;
    all.reserve(300);
    for (unsigned i = 0; i < 300; ++i) {
        const unsigned channels = (i % 3) ? 2 : 1;
        const unsigned bits_per_sample = (i % 4 < 2) ? 16 : 8;
        const unsigned rate = (i % 3 == 0) ? 22050 : (i % 3 == 1) ? 44100 : 48000;
        const unsigned frames = i < 32 ? 48000 : 2048;
        uint32_t b = buffer(rate, channels, bits_per_sample, frames,
                            i % 3 == 0 ? 0x90 : 0xe0);
        all.push_back(b);
        if (i < 32) {
            if (bits_per_sample == 16) {
                std::vector<int16_t> pcm(frames * channels, int16_t(8192 + (i % 4) * 1024));
                upload(b, pcm.data(), uint32_t(pcm.size() * sizeof(int16_t)));
            } else {
                std::vector<uint8_t> pcm(frames * channels, uint8_t(160 + (i % 4) * 8));
                upload(b, pcm.data(), uint32_t(pcm.size()));
            }
            start(b, i != 0);
        }
    }
    std::vector<int16_t> movie(192000, 4096);
    std::array<float, 480 * 2> output{};
    for (unsigned frames : {96u, 480u}) {
        ac1_audio_bink_control(700, AC1_BINK_AUDIO_STOP);
        assert(ac1_audio_bink_submit(700, 0, movie.data(), movie.size(), 48000, 1, 0));
        constexpr unsigned iterations = 1200;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned n = 0; n < iterations; ++n) {
            audio_thread = true;
            ac1_audio_render(output.data(), frames, 48000, head, head);
            audio_thread = false;
            head += frames;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count();
        const double ns_callback = double(elapsed) / iterations;
        std::printf("BENCH audio frames=%u voices=32 objects=300 bink=1 callbacks=%u "
                    "ns/callback=%.0f ns/output-frame=%.2f\n",
                    frames, iterations, ns_callback, ns_callback / frames);
    }
    ac1_audio_bink_control(700, AC1_BINK_AUDIO_STOP);
    for (uint32_t b : all) close_buffer(b);
}
} // namespace
int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    ac1_ds8_callbacks cb{};
    cb.struct_size = sizeof cb;
    cb.guest_resolve = resolve;
    cb.guest_alloc = alloc;
    cb.guest_free = free_guest;
    cb.monotonic_ns = now;
    cb.signal_event = signal;
    assert(ac1_ds8_configure(&cb, nullptr));
    out = alloc(nullptr, 4, 4);
    io = alloc(nullptr, 16, 4);
    uint32_t hr;
    assert(ac1_ds8_create8(0, out, 0, &hr) == 1 && !hr);
    dev = get(out);
    ac1_audio_output_active(1);
    consumption();
    mixing();
    voice_registry();
    spatial();
    bink();
    unsupported();
    concurrency_and_fallback();
    diagnostics();
    benchmark();
    ac1_ds8_shutdown();
    std::puts("PASS audio "
              "consumption/notifications/PCM/gain/pan/rate/3D/Bink/snapshots/concurrency/fallback");
}
