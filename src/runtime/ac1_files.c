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
    case EMFILE:
    case ENFILE: return AC1_ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM: return AC1_ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return AC1_ERROR_INVALID_PARAMETER;
    default: return AC1_ERROR_ACCESS_DENIED;
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
