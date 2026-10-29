/* Native AC1 Bink prototype. Guest addresses are integers, never host pointers. */
#ifndef AC1_BINK_NATIVE_H
#define AC1_BINK_NATIVE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ac1_bink_provider ac1_bink_provider;
enum {
    AC1_BINK_RECORD_BYTES = 0xe0,
    AC1_BINK_FRAME_BUFFERS_BYTES = 0x78,
    AC1_BINK_OPEN_TRACKS = 0x4000,
    AC1_BINK_SURFACE32A = 5,
    AC1_BINK_AUDIO_PAUSE = 1,
    AC1_BINK_AUDIO_RESUME = 2,
    AC1_BINK_AUDIO_STOP = 3
};
/* All calls, including callbacks, run synchronously on the guest baton thread.
 * Callbacks must not reenter this provider or throw. write_guest must validate
 * the WHOLE range before writing, and return 1 on success, 0 on failure.
 * monotonic_us must use a monotonic host clock, independent of game time.
 * submit_pcm must copy/enqueue the whole chunk before returning 1; 0 is a
 * fatal sink error, not backpressure. Each track has an independent sample
 * clock. pts_us starts at zero on open/loop; the sink mixes/resamples tracks.
 * PCM is interleaved signed native-endian 16-bit, already volume-scaled.
 * audio_control controls ALL queued tracks for this record. A NULL submit_pcm
 * explicitly chooses silent playback; no guest audio callback is executed.
 * The runtime owns record allocation, path translation and destination mapping.
 */
typedef struct ac1_bink_callbacks {
    void *opaque;
    int (*write_guest)(void *, uint32_t address, const void *, size_t bytes);
    uint64_t (*monotonic_us)(void *);
    int (*submit_pcm)(void *, uint32_t record, uint32_t track_id,
                      const int16_t *, size_t sample_frames,
                      uint32_t rate, uint32_t channels, uint64_t pts_us);
    void (*audio_control)(void *, uint32_t record, int event);
} ac1_bink_callbacks;

ac1_bink_provider *ac1_bink_create(const ac1_bink_callbacks *);
void ac1_bink_destroy(ac1_bink_provider *);
/* All int operations return 1 on success, -1 on error except the predicates:
 * wait/should_skip return 0/1, or -1 on error. error is borrowed until the next
 * call on this provider. Failures never manufacture a finished BINK record.
 */
const char *ac1_bink_error(const ac1_bink_provider *);
/* Selection affects subsequent opens, like BinkSetSoundTrack. IDs, not stream
 * ordinals. Zero tracks explicitly disables sound. Maximum eight (DLL limit).
 * Missing selected IDs in a silent movie are harmless, as in AC1 LoadingScreen.
 */
int ac1_bink_set_sound_tracks(ac1_bink_provider *, uint32_t count, const uint32_t *ids);
/* Guest allocator addresses are retained for diagnostics only. The supervisor
 * must allocate/free guest records through those guest functions if required;
 * FFmpeg's host allocations must never call an x86 function pointer. */
int ac1_bink_set_memory(ac1_bink_provider *, uint32_t alloc_address, uint32_t free_address);
int ac1_bink_open(ac1_bink_provider *, uint32_t record, const char *host_path, uint32_t flags);
/* Read-only input from the runtime's rooted asset service. The caller keeps
 * opaque alive and closes its file AFTER close/destroy (also after open failure).
 * read returns bytes, 0 at EOF, -1 on error; seek uses SEEK_SET/CUR/END and
 * returns the absolute position or -1. No guest file handle reaches FFmpeg. */
typedef struct ac1_bink_input {
    void *opaque;
    uint64_t size;
    int (*read)(void *, uint8_t *, int bytes);
    int64_t (*seek)(void *, int64_t offset, int whence);
} ac1_bink_input;
int ac1_bink_open_input(ac1_bink_provider *, uint32_t record, const ac1_bink_input *, uint32_t flags);
int ac1_bink_close(ac1_bink_provider *, uint32_t record);
int ac1_bink_do_frame(ac1_bink_provider *, uint32_t record);
int ac1_bink_next_frame(ac1_bink_provider *, uint32_t record);
int ac1_bink_wait(ac1_bink_provider *, uint32_t record);
int ac1_bink_should_skip(ac1_bink_provider *, uint32_t record);
int ac1_bink_pause(ac1_bink_provider *, uint32_t record, int paused);
int ac1_bink_set_volume(ac1_bink_provider *, uint32_t record, uint32_t track_id, int volume);
/* destination is a validated host mapping, size is its actual accessible span.
 * Supports AC1's positive-pitch BGRA path: flags 5 or 0x80000005 (COPYALL).
 * Height is destination surface height, not a clipping height. No partial
 * writes on invalid geometry; row padding and pixels outside the rect survive.
 * Repeated DoFrame is idempotent. Copy remains valid after NextFrame until
 * the next successful DoFrame, matching Bink's retained display buffer.
 */
int ac1_bink_copy_to_buffer(ac1_bink_provider *, uint32_t record,
                           void *destination, size_t size, int32_t pitch,
                           uint32_t height, uint32_t x, uint32_t y, uint32_t flags);
/* Writes the DLL's 32-bit 0x78-byte descriptor to guest memory. Plane
 * allocation flags are populated but addresses/pitches remain zero; AC1 uses
 * only buffer count/dimensions/index to create BGRA textures, then CopyToBuffer.
 * This API does not claim to implement BinkRegisterFrameBuffers/YUV uploads. */
int ac1_bink_get_frame_buffers_info(ac1_bink_provider *, uint32_t record, uint32_t info_address);
#ifdef _WIN32
/* Optional host DirectSound sink, serviced synchronously; no worker thread.
 * Returns NULL with an HRESULT diagnostic if the device cannot be opened. */
void *ac1_bink_audio_create(char *error, size_t capacity);
void ac1_bink_audio_destroy(void *);
int ac1_bink_audio_service(void *);
int ac1_bink_audio_submit(void *, uint32_t, uint32_t, const int16_t *, size_t,
                          uint32_t, uint32_t, uint64_t);
void ac1_bink_audio_control(void *, uint32_t, int);
#endif
#ifdef __cplusplus
}
#endif
#endif
