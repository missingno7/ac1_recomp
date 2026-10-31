/* Portable DS8 and native Bink share a host-owned float stereo mixer. */
#ifndef AC1_AUDIO_MIXER_H
#define AC1_AUDIO_MIXER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Device adapters (and fake outputs) start a new frame timeline here. This
 * discards unconsumed output on device replacement, preserving the play cursor.
 * All mixer entry points are synchronized; none of these invoke guest services.
 * render is nonblocking: on contention it returns 0 with silence, without
 * moving voices; otherwise 1. first_frame is the absolute output frame;
 * consumed counts PRESENTED frames, not frames generated. Calls must be ordered
 * by a single output consumer. */
void ac1_audio_output_active(int active);
void ac1_audio_output_paused(int paused);
void ac1_audio_consume(uint64_t consumed);
int ac1_audio_render(float *stereo, uint32_t frames, uint32_t rate, uint64_t first_frame,
                     uint64_t consumed);
/* Cumulative host-owned diagnostics. Voice frames count contributions before
 * summation, not distinct output frames. Late unlocks wrote PCM the mixer had
 * already read for a playing buffer. Snapshot on a control thread only. */
typedef struct ac1_audio_stats {
    uint64_t render_calls, requested_frames, lock_misses;
    uint64_t ds_frames, bink_frames, bink_underrun_frames, bink_submitted_frames;
    uint64_t nonzero_frames, late_unlocks;
} ac1_audio_stats;
void ac1_audio_get_stats(ac1_audio_stats *stats);
/* Copies the whole native-endian s16 chunk; PTS starts at zero and must be
 * contiguous per (record, track). PCM has already had Bink volume applied.
 * Returns zero with ac1_ds8_last_error() on unsupported format/queue overflow.
 * Controls use AC1_BINK_AUDIO_*; STOP removes every track for the record. */
int ac1_audio_bink_submit(uint32_t record, uint32_t track, const int16_t *pcm, size_t frames,
                          uint32_t rate, uint32_t channels, uint64_t pts_us);
void ac1_audio_bink_control(uint32_t record, int event);
#ifdef __ANDROID__
/* Only control threads call these. Error callbacks merely publish an error;
 * service closes/reopens disconnected streams outside AAudio callback threads. */
void ac1_audio_device_service(void);
void ac1_audio_device_pause(int paused);
void ac1_audio_device_shutdown(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
