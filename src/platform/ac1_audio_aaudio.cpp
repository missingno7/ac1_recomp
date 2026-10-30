/* AAudio output for API 29+. No guest pointers, callbacks or allocations on
 * the real-time thread. Stream ownership is serialized on control threads. */
#include "ac1_audio_mixer.h"
#include <aaudio/AAudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <time.h>

namespace {
std::mutex control;
AAudioStream *stream;
std::atomic<int32_t> stream_error{AAUDIO_OK};
std::atomic<uint64_t> presented{0};
std::atomic<bool> used_read_counter{false};
uint64_t rendered; // Callback thread only, reset before requestStart.
uint32_t device_rate;
bool paused, wanted;
bool reported_read_counter;
std::chrono::steady_clock::time_point retry_at{};

aaudio_data_callback_result_t pull(AAudioStream *s, void *, void *data, int32_t frames) {
    uint64_t consumed = presented.load(std::memory_order_relaxed);
    int64_t position, timestamp;
    if (AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &position, &timestamp) == AAUDIO_OK) {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        int64_t ns = int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
        // AAudio timestamps describe a frame at the hardware presentation
        // boundary. Project to now, never beyond frames we actually supplied.
        int64_t at = position + (ns - timestamp) * int64_t(device_rate) / 1000000000;
        consumed = std::max(consumed, uint64_t(std::clamp<int64_t>(at, 0, int64_t(rendered))));
    } else {
        // Timestamp is normally unavailable during startup. FramesRead counts
        // downstream consumption, rather than advancing a free-running clock.
        int64_t read = AAudioStream_getFramesRead(s);
        if (read > 0) {
            consumed = std::max(consumed, std::min(rendered, uint64_t(read)));
            used_read_counter.store(true, std::memory_order_relaxed);
        }
    }
    presented.store(consumed, std::memory_order_release);
    ac1_audio_render(static_cast<float *>(data), uint32_t(frames), device_rate, rendered, consumed);
    rendered += uint32_t(frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
void error_callback(AAudioStream *, void *, aaudio_result_t error) {
    stream_error.store(error, std::memory_order_release);
}
void close_stream() {
    if (!stream)
        return;
    // Mixer callbacks use try_lock; close cannot wait on a held bridge lock.
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream); // Joins callbacks before state/PCM teardown.
    stream = nullptr;
    ac1_audio_consume(presented.load(std::memory_order_acquire));
    ac1_audio_output_active(0);
}
void service() {
    auto now = std::chrono::steady_clock::now();
    int32_t failure = stream_error.exchange(AAUDIO_OK, std::memory_order_acq_rel);
    if (failure != AAUDIO_OK) {
        std::fprintf(stderr, "AC1 AAudio stream error: %s (%d); reopening\n",
                     AAudio_convertResultToText(failure), failure);
        close_stream();
        retry_at = now;
    }
    if (used_read_counter.exchange(false) && !reported_read_counter) {
        std::fprintf(stderr, "AC1 AAudio: hardware timestamp unavailable; using downstream "
                             "FramesRead consumption\n");
        reported_read_counter = true;
    }
    if (stream) {
        ac1_audio_consume(presented.load(std::memory_order_acquire));
        return;
    }
    if (!wanted || paused || now < retry_at)
        return;
    retry_at = now + std::chrono::seconds(1);
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t rc = AAudio_createStreamBuilder(&builder);
    if (rc == AAUDIO_OK) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setDataCallback(builder, pull, nullptr);
        AAudioStreamBuilder_setErrorCallback(builder, error_callback, nullptr);
        rc = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
    }
    if (rc == AAUDIO_OK &&
        (AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_FLOAT ||
         AAudioStream_getChannelCount(stream) != 2 || AAudioStream_getSampleRate(stream) <= 0))
        rc = AAUDIO_ERROR_INVALID_FORMAT;
    if (rc == AAUDIO_OK) {
        device_rate = uint32_t(AAudioStream_getSampleRate(stream));
        rendered = 0;
        presented.store(0);
        stream_error.store(AAUDIO_OK);
        reported_read_counter = false;
        int32_t burst = AAudioStream_getFramesPerBurst(stream);
        if (burst > 0) {
            int32_t size = AAudioStream_setBufferSizeInFrames(stream, burst * 2);
            if (size < 0)
                std::fprintf(
                    stderr,
                    "AC1 AAudio buffer size request failed: %s (%d); retaining device default\n",
                    AAudio_convertResultToText(size), size);
        }
        ac1_audio_output_active(1);
        rc = AAudioStream_requestStart(stream);
    }
    if (rc != AAUDIO_OK) {
        std::fprintf(stderr, "AC1 AAudio open/start failed: %s (%d); clock fallback, retry in 1s\n",
                     AAudio_convertResultToText(rc), rc);
        close_stream();
    } else {
        std::fprintf(
            stderr, "AC1 AAudio: float stereo %u Hz, burst=%d, buffer=%d, low-latency=%s\n",
            device_rate, AAudioStream_getFramesPerBurst(stream),
            AAudioStream_getBufferSizeInFrames(stream),
            AAudioStream_getPerformanceMode(stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "yes"
                                                                                           : "no");
    }
}
} // namespace
extern "C" void ac1_audio_device_service() {
    std::lock_guard<std::mutex> lock(control);
    wanted = true;
    service();
}
extern "C" void ac1_audio_device_pause(int pause) {
    std::lock_guard<std::mutex> lock(control);
    paused = pause != 0;
    if (paused) {
        // Close/flush instead of replaying stale queued audio on resume.
        ac1_audio_output_paused(1);
        close_stream();
    } else {
        ac1_audio_output_paused(0);
        retry_at = {};
        service();
    }
}
extern "C" void ac1_audio_device_shutdown() {
    std::lock_guard<std::mutex> lock(control);
    wanted = false;
    close_stream();
    retry_at = {};
}
