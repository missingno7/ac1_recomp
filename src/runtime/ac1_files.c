#define _POSIX_C_SOURCE 200809L
#include "ac1_files.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
_Static_assert(sizeof(off_t) >= sizeof(int64_t),
               "AC1 file service requires 64-bit host file offsets");
#endif

enum {
    AC1_FILE_HANDLE_BASE = 0xE4000000u,
    AC1_FILE_HANDLE_SLOTS = 256u,
    AC1_FILE_PATH_CAPACITY = 32768u,
    AC1_ERROR_FILE_NOT_FOUND = 2u,
    AC1_ERROR_PATH_NOT_FOUND = 3u,
    AC1_ERROR_ACCESS_DENIED = 5u,
    AC1_ERROR_INVALID_HANDLE = 6u,
    AC1_ERROR_NOT_ENOUGH_MEMORY = 8u,
    AC1_ERROR_INVALID_PARAMETER = 87u,
    AC1_ERROR_TOO_MANY_OPEN_FILES = 4u,
    AC1_ERROR_NEGATIVE_SEEK = 131u,
    AC1_ERROR_SEEK_ON_DEVICE = 132u,
    AC1_ERROR_IO_DEVICE = 1117u,
    AC1_GENERIC_READ = 0x80000000u,
    AC1_OPEN_EXISTING = 3u,
    AC1_FILE_FLAG_RANDOM_ACCESS = 0x10000000u
};

#ifdef _WIN32
typedef HANDLE ac1_host_file;
#define AC1_HOST_FILE_INVALID INVALID_HANDLE_VALUE
#else
typedef int ac1_host_file;
#define AC1_HOST_FILE_INVALID (-1)
#endif

struct ac1_files {
    char root[AC1_FILE_PATH_CAPACITY];
    ac1_host_file slots[AC1_FILE_HANDLE_SLOTS];
};

#ifndef _WIN32
static uint32_t error_from_errno(int error)
{
    switch (error) {
    case ENOENT: return AC1_ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return AC1_ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM: return AC1_ERROR_ACCESS_DENIED;
    case EBADF: return AC1_ERROR_INVALID_HANDLE;
    case EPIPE: return 109u; /* ERROR_BROKEN_PIPE */
    case ENOSPC: return 112u; /* ERROR_DISK_FULL */
    case ESPIPE: return AC1_ERROR_SEEK_ON_DEVICE;
    case EMFILE:
    case ENFILE: return AC1_ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM: return AC1_ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return AC1_ERROR_INVALID_PARAMETER;
    default: return AC1_ERROR_IO_DEVICE;
    }
}
#endif

static int normalize_root(char *destination, size_t capacity,
                          const char *source)
{
    if (!destination || !capacity || !source || !source[0]) return 0;
#ifdef _WIN32
    DWORD length = GetFullPathNameA(source, (DWORD)capacity, destination, NULL);
    if (!length || length >= capacity) return 0;
    for (size_t i = 0; destination[i]; ++i)
        if (destination[i] == '/') destination[i] = '\\';
    while (length > 3u && destination[length - 1u] == '\\')
        destination[--length] = '\0';
    return 1;
#else
    char resolved[AC1_FILE_PATH_CAPACITY];
    if (!realpath(source, resolved)) return 0;
    size_t length = strlen(resolved);
    if (length >= capacity) return 0;
    memcpy(destination, resolved, length + 1u);
    return 1;
#endif
}

ac1_files *ac1_files_create(const char *game_root)
{
    ac1_files *files = (ac1_files *)calloc(1u, sizeof(*files));
    if (!files) return NULL;
    if (!normalize_root(files->root, sizeof(files->root), game_root)) {
        free(files);
        return NULL;
    }
    for (size_t i = 0; i < AC1_FILE_HANDLE_SLOTS; ++i)
        files->slots[i] = AC1_HOST_FILE_INVALID;
    return files;
}

void ac1_files_destroy(ac1_files *files)
{
    if (!files) return;
    for (size_t i = 0; i < AC1_FILE_HANDLE_SLOTS; ++i) {
        if (files->slots[i] == AC1_HOST_FILE_INVALID) continue;
#ifdef _WIN32
        CloseHandle(files->slots[i]);
#else
        close(files->slots[i]);
#endif
        files->slots[i] = AC1_HOST_FILE_INVALID;
    }
    free(files);
}

static int is_single_relative_filename(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (path[0] == '.' || path[0] == '/' || path[0] == '\\')
        return 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        if (*p < 0x20u || *p == '/' || *p == '\\' || *p == ':' || *p == '"' ||
            *p == '<' || *p == '>' || *p == '|' || *p == '*' || *p == '?')
            return 0;
    }
    return 1;
}

static int make_asset_path(const ac1_files *files, const char *filename,
                           char *path, size_t capacity)
{
    size_t root_length = strlen(files->root);
    size_t filename_length = strlen(filename);
#ifdef _WIN32
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    int has_separator = root_length &&
        (files->root[root_length - 1u] == '/' ||
         files->root[root_length - 1u] == '\\');
    size_t required = root_length + (has_separator ? 0u : 1u) + filename_length + 1u;
    if (required > capacity) return 0;
    memcpy(path, files->root, root_length);
    size_t offset = root_length;
    if (!has_separator) path[offset++] = separator;
    memcpy(path + offset, filename, filename_length + 1u);
    return 1;
}

static int allocate_slot(ac1_files *files, size_t *index_out)
{
    for (size_t i = 0; i < AC1_FILE_HANDLE_SLOTS; ++i) {
        if (files->slots[i] == AC1_HOST_FILE_INVALID) {
            *index_out = i;
            return 1;
        }
    }
    return 0;
}

int ac1_files_open(ac1_files *files, const char *guest_path,
                   uint32_t desired_access, uint32_t share_mode,
                   int security_attributes_present,
                   uint32_t creation_disposition, uint32_t flags_and_attributes,
                   uint32_t template_handle, uint32_t *guest_handle_out,
                   uint32_t *error_out)
{
    if (!files || !guest_handle_out || !error_out) return 0;
    *guest_handle_out = UINT32_MAX; /* INVALID_HANDLE_VALUE */
    *error_out = 0u;

    /* This is intentionally the observed archive existence probe, not a
     * general CreateFile implementation. In particular no writes or creation
     * dispositions can reach the original installation. */
    if (desired_access != AC1_GENERIC_READ) {
        const uint32_t write_access = 0x40000000u | 0x00010000u |
            0x00040000u | 0x00080000u | 0x00000002u | 0x00000004u |
            0x00000010u | 0x00000100u | 0x00000400u;
        if (desired_access & write_access) {
            *error_out = AC1_ERROR_ACCESS_DENIED;
            return AC1_FILES_HANDLED;
        }
        return AC1_FILES_UNSUPPORTED;
    }
    if (creation_disposition != AC1_OPEN_EXISTING) {
        if (creation_disposition == 1u || creation_disposition == 2u ||
            creation_disposition == 4u || creation_disposition == 5u) {
            *error_out = AC1_ERROR_ACCESS_DENIED;
            return AC1_FILES_HANDLED;
        }
        return AC1_FILES_UNSUPPORTED;
    }
    if (share_mode & ~7u || security_attributes_present ||
        flags_and_attributes != AC1_FILE_FLAG_RANDOM_ACCESS || template_handle)
        return AC1_FILES_UNSUPPORTED;

    if (!guest_path || !guest_path[0]) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    if (!is_single_relative_filename(guest_path)) {
        /* The current startup slice only needs an archive basename. Preserve
         * visibility of other relative/absolute path forms at the runtime
         * boundary instead of disguising them as missing or denied files. */
        return AC1_FILES_UNSUPPORTED;
    }

    size_t slot_index;
    if (!allocate_slot(files, &slot_index)) {
        *error_out = AC1_ERROR_TOO_MANY_OPEN_FILES;
        return 1;
    }

    char path[AC1_FILE_PATH_CAPACITY];
    if (!make_asset_path(files, guest_path, path, sizeof(path))) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return 1;
    }

#ifdef _WIN32
    HANDLE host = CreateFileA(path, GENERIC_READ, share_mode, NULL,
                              OPEN_EXISTING,
                              FILE_FLAG_RANDOM_ACCESS |
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                              NULL);
    if (host == INVALID_HANDLE_VALUE) {
        *error_out = (uint32_t)GetLastError();
        return 1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(host, &info)) {
        *error_out = (uint32_t)GetLastError();
        CloseHandle(host);
        return 1;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DIRECTORY)) != 0u) {
        CloseHandle(host);
        *error_out = AC1_ERROR_ACCESS_DENIED;
        return 1;
    }
#else
    int host = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (host < 0) {
        *error_out = error_from_errno(errno);
        return 1;
    }
    struct stat info;
    if (fstat(host, &info) != 0) {
        *error_out = error_from_errno(errno);
        close(host);
        return 1;
    }
    if (!S_ISREG(info.st_mode)) {
        close(host);
        *error_out = AC1_ERROR_ACCESS_DENIED;
        return 1;
    }
#endif

    files->slots[slot_index] = host;
    *guest_handle_out = AC1_FILE_HANDLE_BASE + (uint32_t)(slot_index * 4u);
    *error_out = 0u;
    return AC1_FILES_HANDLED;
}

int ac1_files_close(ac1_files *files, uint32_t guest_handle,
                    uint32_t *error_out)
{
    if (!files || !error_out) return 0;
    if (guest_handle < AC1_FILE_HANDLE_BASE ||
        ((guest_handle - AC1_FILE_HANDLE_BASE) & 3u) != 0u)
        return 0;
    uint32_t slot_index = (guest_handle - AC1_FILE_HANDLE_BASE) / 4u;
    if (slot_index >= AC1_FILE_HANDLE_SLOTS) return 0;
    ac1_host_file host = files->slots[slot_index];
    if (host == AC1_HOST_FILE_INVALID) {
        *error_out = AC1_ERROR_INVALID_HANDLE;
        return 1;
    }
    files->slots[slot_index] = AC1_HOST_FILE_INVALID;
#ifdef _WIN32
    if (!CloseHandle(host)) {
        *error_out = (uint32_t)GetLastError();
        return 1;
    }
#else
    if (close(host) != 0) {
        *error_out = error_from_errno(errno);
        return 1;
    }
#endif
    *error_out = 0u;
    return 1;
}

static int lookup_file_slot(ac1_files *files, uint32_t guest_handle,
                            size_t *slot_index_out, uint32_t *error_out)
{
    if (guest_handle < AC1_FILE_HANDLE_BASE ||
        ((guest_handle - AC1_FILE_HANDLE_BASE) & 3u) != 0u)
        return AC1_FILES_NOT_HANDLED;
    uint32_t slot_index = (guest_handle - AC1_FILE_HANDLE_BASE) / 4u;
    if (slot_index >= AC1_FILE_HANDLE_SLOTS) return AC1_FILES_NOT_HANDLED;
    if (files->slots[slot_index] == AC1_HOST_FILE_INVALID) {
        *error_out = AC1_ERROR_INVALID_HANDLE;
        return AC1_FILES_HANDLED;
    }
    *slot_index_out = slot_index;
    return AC1_FILES_HANDLED;
}

int ac1_files_read(ac1_files *files, uint32_t guest_handle, void *buffer,
                   uint32_t requested_bytes, uint32_t *bytes_read_out,
                   uint32_t *error_out)
{
    if (!files || !error_out) return AC1_FILES_NOT_HANDLED;
    if (bytes_read_out) *bytes_read_out = 0u;
    *error_out = 0u;
    size_t slot_index = 0;
    int lookup = lookup_file_slot(files, guest_handle, &slot_index, error_out);
    if (lookup != AC1_FILES_HANDLED || *error_out) return lookup;
    if (!bytes_read_out || (requested_bytes && !buffer)) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }

#ifdef _WIN32
    DWORD bytes_read = 0;
    if (!ReadFile(files->slots[slot_index], buffer, requested_bytes,
                  &bytes_read, NULL)) {
        *error_out = (uint32_t)GetLastError();
        return AC1_FILES_HANDLED;
    }
    *bytes_read_out = (uint32_t)bytes_read;
#else
    if (!requested_bytes) return AC1_FILES_HANDLED;
    ssize_t bytes_read;
    do {
        bytes_read = read(files->slots[slot_index], buffer,
                          (size_t)requested_bytes);
    } while (bytes_read < 0 && errno == EINTR);
    if (bytes_read < 0) {
        *error_out = error_from_errno(errno);
        return AC1_FILES_HANDLED;
    }
    *bytes_read_out = (uint32_t)bytes_read;
#endif
    /* A zero-byte transfer at EOF is success. The caller decides whether a
     * short read satisfies its higher-level exact-read request. */
    *error_out = 0u;
    return AC1_FILES_HANDLED;
}

#ifndef _WIN32
static int64_t combine_seek_distance(uint32_t low, int has_high,
                                     uint32_t high)
{
    if (!has_high) return (int64_t)(int32_t)low;
    return (int64_t)(int32_t)high * INT64_C(4294967296) +
           (int64_t)(uint64_t)low;
}
#endif

int ac1_files_seek(ac1_files *files, uint32_t guest_handle,
                   uint32_t distance_low, int has_distance_high,
                   uint32_t distance_high, uint32_t move_method,
                   uint32_t *position_low_out, uint32_t *position_high_out,
                   uint32_t *error_out)
{
    if (!files || !error_out) return AC1_FILES_NOT_HANDLED;
    if (position_low_out) *position_low_out = UINT32_MAX;
    *error_out = 0u;
    size_t slot_index = 0;
    int lookup = lookup_file_slot(files, guest_handle, &slot_index, error_out);
    if (lookup != AC1_FILES_HANDLED || *error_out) return lookup;
    if (!position_low_out || (has_distance_high && !position_high_out) ||
        move_method > 2u) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
#ifdef _WIN32
    LONG high = (LONG)distance_high;
    SetLastError(NO_ERROR);
    DWORD low = SetFilePointer(files->slots[slot_index], (LONG)distance_low,
                               has_distance_high ? &high : NULL,
                               move_method == 0u ? FILE_BEGIN
                                   : move_method == 1u ? FILE_CURRENT : FILE_END);
    DWORD native_error = GetLastError();
    if (low == INVALID_SET_FILE_POINTER && native_error != NO_ERROR) {
        *error_out = (uint32_t)native_error;
        return AC1_FILES_HANDLED;
    }
    uint64_t position;
    if (has_distance_high) {
        *position_high_out = (uint32_t)high;
        position = ((uint64_t)(uint32_t)high << 32) | low;
    } else {
        /* SetFilePointer without lpDistanceToMoveHigh has a 32-bit file
         * position range and INVALID_SET_FILE_POINTER/GetLastError semantics. */
        position = low;
    }
#else
    int64_t distance = combine_seek_distance(distance_low,
                                              has_distance_high,
                                              distance_high);
    static const int origins[3] = {SEEK_SET, SEEK_CUR, SEEK_END};
    int64_t base = 0;
    if (move_method == 1u) {
        off_t current = lseek(files->slots[slot_index], (off_t)0, SEEK_CUR);
        if (current < 0) {
            *error_out = errno == EINVAL ? AC1_ERROR_NEGATIVE_SEEK
                : error_from_errno(errno);
            return AC1_FILES_HANDLED;
        }
        base = (int64_t)current;
    } else if (move_method == 2u) {
        struct stat info;
        if (fstat(files->slots[slot_index], &info) != 0) {
            *error_out = error_from_errno(errno);
            return AC1_FILES_HANDLED;
        }
        base = (int64_t)info.st_size;
    }
    if ((distance > 0 && base > INT64_MAX - distance) ||
        (distance < 0 && base < INT64_MIN - distance)) {
        *error_out = 534u; /* ERROR_ARITHMETIC_OVERFLOW */
        return AC1_FILES_HANDLED;
    }
    int64_t target = base + distance;
    if (target < 0) {
        *error_out = AC1_ERROR_NEGATIVE_SEEK;
        return AC1_FILES_HANDLED;
    }
    if (!has_distance_high && target > UINT32_MAX) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    off_t result = lseek(files->slots[slot_index], (off_t)distance,
                         origins[move_method]);
    if (result < 0) {
        *error_out = errno == EINVAL ? AC1_ERROR_INVALID_PARAMETER
            : error_from_errno(errno);
        return AC1_FILES_HANDLED;
    }
    uint64_t position = (uint64_t)result;
    if (has_distance_high)
        *position_high_out = (uint32_t)(position >> 32);
#endif
    *position_low_out = (uint32_t)position;
    *error_out = 0u;
    return AC1_FILES_HANDLED;
}
