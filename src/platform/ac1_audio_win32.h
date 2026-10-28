#ifndef AC1_AUDIO_WIN32_H
#define AC1_AUDIO_WIN32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DirectSoundEnumerateA is collected on the host first. Guest callbacks are
 * deliberately outside this API and must be invoked by the runtime after the
 * host call has returned. */
#define AC1_AUDIO_ENUM_MAX_DEVICES 64u
#define AC1_AUDIO_ENUM_DESCRIPTION_CAPACITY 256u
#define AC1_AUDIO_ENUM_MODULE_CAPACITY 260u

enum {
    AC1_AUDIO_ENUM_UNSUPPORTED = 0,
    AC1_AUDIO_ENUM_HANDLED = 1,
    AC1_AUDIO_ENUM_ERROR = -1
};

enum {
    AC1_AUDIO_ENUM_OVERFLOW_DEVICE_COUNT = 1u,
    AC1_AUDIO_ENUM_OVERFLOW_DESCRIPTION = 2u,
    AC1_AUDIO_ENUM_OVERFLOW_MODULE = 4u,
    AC1_AUDIO_ENUM_OUT_OF_MEMORY = 8u
};

typedef struct ac1_audio_device_record {
    uint8_t has_guid;
    uint8_t guid[16];
    char description[AC1_AUDIO_ENUM_DESCRIPTION_CAPACITY];
    char module[AC1_AUDIO_ENUM_MODULE_CAPACITY];
} ac1_audio_device_record;

typedef struct ac1_audio_device_list {
    ac1_audio_device_record *devices;
    size_t count;
    size_t capacity;
    uint32_t overflow_flags;
    /* Exact HRESULT from DirectSoundEnumerateA when status is HANDLED. For
     * ERROR, setup failures are represented as HRESULT_FROM_WIN32 values. */
    int32_t native_hresult;
} ac1_audio_device_list;

/* Returns HANDLED after calling the native API, UNSUPPORTED on platforms
 * without a real DirectSound provider, and ERROR for host-side setup or
 * collection failures. On HANDLED, native_hresult is the provider's exact
 * result; overflow_flags must be zero before consuming the records. */
int ac1_audio_enumerate_a(ac1_audio_device_list *list);

/* Releases the owned record array and clears the list. Safe on zeroed lists. */
void ac1_audio_device_list_destroy(ac1_audio_device_list *list);

#ifdef __cplusplus
}
#endif

#endif
