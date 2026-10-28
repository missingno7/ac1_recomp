#include "ac1_audio_win32.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct ac1_native_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} ac1_native_guid;
_Static_assert(sizeof(ac1_native_guid) == 16u, "Windows GUID must be 16 bytes");

typedef BOOL (CALLBACK *ac1_ds_enum_callback)(ac1_native_guid *guid,
                                               LPCSTR description,
                                               LPCSTR module,
                                               LPVOID context);
typedef HRESULT (WINAPI *ac1_direct_sound_enumerate_a_fn)(
    ac1_ds_enum_callback callback, LPVOID context);

typedef struct ac1_enum_context {
    ac1_audio_device_list *list;
    int failed;
} ac1_enum_context;

static int copy_native_string(char *destination, size_t capacity,
                              const char *source)
{
    if (!destination || !capacity) return 0;
    if (!source) source = "";

    size_t length = strlen(source);
    if (length >= capacity) {
        memcpy(destination, source, capacity - 1u);
        destination[capacity - 1u] = '\0';
        return 0;
    }

    memcpy(destination, source, length + 1u);
    return 1;
}

static BOOL CALLBACK collect_device(ac1_native_guid *guid,
                                    LPCSTR description,
                                    LPCSTR module,
                                    LPVOID opaque)
{
    ac1_enum_context *context = (ac1_enum_context *)opaque;
    ac1_audio_device_list *list = context ? context->list : NULL;
    if (!list) return FALSE;

    if (list->count >= AC1_AUDIO_ENUM_MAX_DEVICES) {
        list->overflow_flags |= AC1_AUDIO_ENUM_OVERFLOW_DEVICE_COUNT;
        return FALSE;
    }

    if (list->count == list->capacity) {
        size_t next_capacity = list->capacity ? list->capacity * 2u : 8u;
        if (next_capacity > AC1_AUDIO_ENUM_MAX_DEVICES)
            next_capacity = AC1_AUDIO_ENUM_MAX_DEVICES;
        if (next_capacity > SIZE_MAX / sizeof(*list->devices)) {
            list->overflow_flags |= AC1_AUDIO_ENUM_OUT_OF_MEMORY;
            context->failed = 1;
            return FALSE;
        }
        ac1_audio_device_record *next = (ac1_audio_device_record *)realloc(
            list->devices, next_capacity * sizeof(*list->devices));
        if (!next) {
            list->overflow_flags |= AC1_AUDIO_ENUM_OUT_OF_MEMORY;
            context->failed = 1;
            return FALSE;
        }
        list->devices = next;
        list->capacity = next_capacity;
    }

    ac1_audio_device_record *record = &list->devices[list->count];
    memset(record, 0, sizeof(*record));
    if (guid) {
        memcpy(record->guid, guid, sizeof(record->guid));
        record->has_guid = 1;
    }
    if (!copy_native_string(record->description, sizeof(record->description),
                            description))
        list->overflow_flags |= AC1_AUDIO_ENUM_OVERFLOW_DESCRIPTION;
    if (!copy_native_string(record->module, sizeof(record->module), module))
        list->overflow_flags |= AC1_AUDIO_ENUM_OVERFLOW_MODULE;

    ++list->count;
    if (list->overflow_flags) return FALSE;
    return TRUE;
}

static int32_t hresult_from_last_error(DWORD error)
{
    if (!error) error = ERROR_GEN_FAILURE;
    return (int32_t)HRESULT_FROM_WIN32(error);
}

int ac1_audio_enumerate_a(ac1_audio_device_list *list)
{
    if (!list) return AC1_AUDIO_ENUM_ERROR;
    memset(list, 0, sizeof(*list));

    wchar_t path[32768];
    SetLastError(ERROR_SUCCESS);
    UINT length = GetSystemDirectoryW(path, (UINT)(sizeof(path) / sizeof(path[0])));
    static const wchar_t suffix[] = L"\\dsound.dll";
    const UINT path_capacity = (UINT)(sizeof(path) / sizeof(path[0]));
    if (!length) {
        list->native_hresult = hresult_from_last_error(GetLastError());
        return AC1_AUDIO_ENUM_ERROR;
    }
    if (length >= path_capacity ||
        length + (UINT)(sizeof(suffix) / sizeof(suffix[0])) > path_capacity) {
        list->native_hresult = hresult_from_last_error(ERROR_INSUFFICIENT_BUFFER);
        return AC1_AUDIO_ENUM_ERROR;
    }
    memcpy(path + length, suffix, sizeof(suffix));

    HMODULE module = LoadLibraryW(path);
    if (!module) {
        list->native_hresult = hresult_from_last_error(GetLastError());
        return AC1_AUDIO_ENUM_ERROR;
    }

    FARPROC raw_function = GetProcAddress(module, "DirectSoundEnumerateA");
    if (!raw_function) {
        list->native_hresult = hresult_from_last_error(GetLastError());
        FreeLibrary(module);
        return AC1_AUDIO_ENUM_ERROR;
    }

    ac1_direct_sound_enumerate_a_fn enumerate = NULL;
    if (sizeof(enumerate) != sizeof(raw_function)) {
        list->native_hresult = (int32_t)E_FAIL;
        FreeLibrary(module);
        return AC1_AUDIO_ENUM_ERROR;
    }
    memcpy(&enumerate, &raw_function, sizeof(enumerate));

    ac1_enum_context context = { list, 0 };
    list->native_hresult = (int32_t)enumerate(collect_device, &context);
    FreeLibrary(module);
    return context.failed ? AC1_AUDIO_ENUM_ERROR : AC1_AUDIO_ENUM_HANDLED;
}

#else

int ac1_audio_enumerate_a(ac1_audio_device_list *list)
{
    if (list) memset(list, 0, sizeof(*list));
    return list ? AC1_AUDIO_ENUM_UNSUPPORTED : AC1_AUDIO_ENUM_ERROR;
}

#endif

void ac1_audio_device_list_destroy(ac1_audio_device_list *list)
{
    if (!list) return;
    free(list->devices);
    memset(list, 0, sizeof(*list));
}
