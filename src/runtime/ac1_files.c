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
#include <dirent.h>
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
    AC1_ERROR_HANDLE_EOF = 38u,
    AC1_ERROR_IO_PENDING = 997u,
    AC1_ERROR_IO_INCOMPLETE = 996u,
    AC1_ERROR_TOO_MANY_OPEN_FILES = 4u,
    AC1_FILE_ASYNC_SLOTS = 128u,
    AC1_ERROR_NEGATIVE_SEEK = 131u,
    AC1_ERROR_SEEK_ON_DEVICE = 132u,
    AC1_ERROR_INVALID_NAME = 123u,
    AC1_ERROR_IO_DEVICE = 1117u,
    AC1_GENERIC_READ = 0x80000000u,
    AC1_OPEN_EXISTING = 3u,
    AC1_FILE_FLAG_RANDOM_ACCESS = 0x10000000u,
    AC1_GEAR_STREAM_FLAGS = 0x70000001u
};

#define AC1_STATUS_END_OF_FILE UINT32_C(0xC0000011)

#ifdef _WIN32
typedef HANDLE ac1_host_file;
#define AC1_HOST_FILE_INVALID INVALID_HANDLE_VALUE
#else
typedef int ac1_host_file;
#define AC1_HOST_FILE_INVALID (-1)
#endif

typedef struct ac1_file_async_operation {
    int active;
    int pending;
    uint32_t guest_handle;
    uint32_t guest_overlapped_address;
    uint32_t requested_bytes;
    void *buffer;
    uint32_t completion_error;
    uint32_t snapshot_valid;
#ifdef _WIN32
    HANDLE duplicated_event;
    OVERLAPPED native_overlapped;
#else
    uint32_t bytes_transferred;
    uint32_t internal_status;
    uint32_t internal_high;
#endif
} ac1_file_async_operation;

struct ac1_files {
    char root[AC1_FILE_PATH_CAPACITY];
    char guest_root[AC1_FILE_PATH_CAPACITY];
    ac1_host_file slots[AC1_FILE_HANDLE_SLOTS];
    ac1_file_async_operation async_operations[AC1_FILE_ASYNC_SLOTS];
#ifndef _WIN32
    int root_fd;
#endif
};

static void clear_async_operation(ac1_file_async_operation *operation)
{
    if (!operation) return;
#ifdef _WIN32
    if (operation->duplicated_event)
        CloseHandle(operation->duplicated_event);
#endif
    memset(operation, 0, sizeof(*operation));
}

static void cancel_file_operations(ac1_files *files, uint32_t guest_handle,
                                   ac1_host_file host_file)
{
    if (!files) return;
    for (size_t i = 0; i < AC1_FILE_ASYNC_SLOTS; ++i) {
        ac1_file_async_operation *operation = &files->async_operations[i];
        if (!operation->active || operation->guest_handle != guest_handle)
            continue;
#ifdef _WIN32
        (void)CancelIoEx(host_file, &operation->native_overlapped);
        DWORD ignored_bytes = 0;
        (void)GetOverlappedResult(host_file, &operation->native_overlapped,
                                  &ignored_bytes, TRUE);
#else
        (void)host_file;
#endif
        clear_async_operation(operation);
    }
}

static int normalize_guest_absolute_root(const char *source,
                                         char *destination,
                                         size_t capacity);

static unsigned char ascii_path_fold(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? (unsigned char)(value + ('a' - 'A')) : value;
}

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
    case ELOOP: return AC1_ERROR_ACCESS_DENIED;
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

ac1_files *ac1_files_create_with_guest_root(const char *host_game_root,
                                            const char *guest_windows_root)
{
    ac1_files *files = (ac1_files *)calloc(1u, sizeof(*files));
    if (!files) return NULL;
#ifndef _WIN32
    files->root_fd = -1;
#endif
    if (!normalize_root(files->root, sizeof(files->root), host_game_root) ||
        (guest_windows_root && guest_windows_root[0] &&
         !normalize_guest_absolute_root(guest_windows_root, files->guest_root,
                                        sizeof(files->guest_root)))) {
        free(files);
        return NULL;
    }
    for (size_t i = 0; i < AC1_FILE_HANDLE_SLOTS; ++i)
        files->slots[i] = AC1_HOST_FILE_INVALID;
#ifndef _WIN32
    files->root_fd = open(files->root,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (files->root_fd < 0) {
        free(files);
        return NULL;
    }
    struct stat root_info;
    if (fstat(files->root_fd, &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        close(files->root_fd);
        free(files);
        return NULL;
    }
#endif
    return files;
}

ac1_files *ac1_files_create(const char *game_root)
{
#ifdef _WIN32
    /* Preserve the legacy convenience API for relative host paths. Derive
     * its logical Windows root from the same canonicalized path used for
     * opening assets, rather than requiring the caller's input to be absolute. */
    ac1_files *files = ac1_files_create_with_guest_root(game_root, NULL);
    if (!files) return NULL;
    if (!normalize_guest_absolute_root(files->root, files->guest_root,
                                       sizeof(files->guest_root))) {
        ac1_files_destroy(files);
        return NULL;
    }
    return files;
#else
    return ac1_files_create_with_guest_root(game_root, NULL);
#endif
}

void ac1_files_destroy(ac1_files *files)
{
    if (!files) return;
    for (size_t i = 0; i < AC1_FILE_HANDLE_SLOTS; ++i) {
        if (files->slots[i] == AC1_HOST_FILE_INVALID) continue;
        cancel_file_operations(files,
            AC1_FILE_HANDLE_BASE + (uint32_t)(i * 4u), files->slots[i]);
#ifdef _WIN32
        CloseHandle(files->slots[i]);
#else
        close(files->slots[i]);
#endif
        files->slots[i] = AC1_HOST_FILE_INVALID;
    }
#ifndef _WIN32
    if (files->root_fd >= 0) close(files->root_fd);
    files->root_fd = -1;
#endif
    free(files);
}

void ac1_files_cancel_pending(ac1_files *files)
{
    if (!files) return;
    for (size_t i = 0; i < AC1_FILE_HANDLE_SLOTS; ++i) {
        if (files->slots[i] == AC1_HOST_FILE_INVALID) continue;
        cancel_file_operations(files,
            AC1_FILE_HANDLE_BASE + (uint32_t)(i * 4u), files->slots[i]);
    }
}

int ac1_files_has_pending(const ac1_files *files)
{
    if (!files) return 0;
    for (size_t i = 0; i < AC1_FILE_ASYNC_SLOTS; ++i)
        if (files->async_operations[i].active &&
            files->async_operations[i].pending)
            return 1;
    return 0;
}

static int component_ascii_equal(const char *component, size_t length,
                                 const char *name)
{
    size_t name_length = strlen(name);
    if (length != name_length) return 0;
    for (size_t i = 0; i < length; ++i) {
        if (ascii_path_fold((unsigned char)component[i]) !=
            ascii_path_fold((unsigned char)name[i]))
            return 0;
    }
    return 1;
}

static int is_reserved_dos_device_component(const char *component,
                                            size_t length)
{
    size_t base_length = 0;
    while (base_length < length && component[base_length] != '.')
        ++base_length;
    while (base_length && component[base_length - 1u] == ' ')
        --base_length;
    if (component_ascii_equal(component, base_length, "CON") ||
        component_ascii_equal(component, base_length, "PRN") ||
        component_ascii_equal(component, base_length, "AUX") ||
        component_ascii_equal(component, base_length, "NUL") ||
        component_ascii_equal(component, base_length, "CONIN$") ||
        component_ascii_equal(component, base_length, "CONOUT$") ||
        component_ascii_equal(component, base_length, "CLOCK$"))
        return 1;
    return base_length == 4u &&
        ((ascii_path_fold((unsigned char)component[0]) == 'c' &&
          ascii_path_fold((unsigned char)component[1]) == 'o' &&
          ascii_path_fold((unsigned char)component[2]) == 'm') ||
         (ascii_path_fold((unsigned char)component[0]) == 'l' &&
          ascii_path_fold((unsigned char)component[1]) == 'p' &&
          ascii_path_fold((unsigned char)component[2]) == 't')) &&
        component[3] >= '1' && component[3] <= '9';
}

static int normalize_guest_components(const char *path, char *normalized,
                                     size_t capacity)
{
    if (!path || !path[0] || !normalized || !capacity)
        return 0;
    size_t input_length = strlen(path);
    if (input_length >= capacity) return 0;
    size_t output_length = 0;
    size_t component_start = 0;
    for (size_t i = 0; i <= input_length; ++i) {
        unsigned char ch = (unsigned char)path[i];
        int separator = ch == '/' || ch == '\\';
        if (!separator && ch != '\0') {
            if (ch < 0x20u || ch == ':' || ch == '"' || ch == '<' ||
                ch == '>' || ch == '|' || ch == '*' || ch == '?')
                return 0;
            normalized[output_length++] = (char)ch;
            continue;
        }
        size_t component_length = output_length - component_start;
        if (!component_length ||
            normalized[output_length - 1u] == '.' ||
            normalized[output_length - 1u] == ' ' ||
            is_reserved_dos_device_component(normalized + component_start,
                                             component_length) ||
            (component_length == 1u && normalized[component_start] == '.') ||
            (component_length == 2u && normalized[component_start] == '.' &&
             normalized[component_start + 1u] == '.'))
            return 0;
        if (separator) {
            normalized[output_length++] = '/';
            component_start = output_length;
        }
    }
    normalized[output_length] = '\0';
    return 1;
}

static int normalize_guest_path(const char *path, char *normalized,
                                size_t capacity)
{
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\') return 0;
    return normalize_guest_components(path, normalized, capacity);
}

static int is_windows_absolute_path(const char *path)
{
    if (!path || !path[0] || !path[1] || !path[2]) return 0;
    unsigned char drive = (unsigned char)path[0];
    return ((drive >= 'A' && drive <= 'Z') ||
            (drive >= 'a' && drive <= 'z')) &&
           path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static int normalize_guest_absolute_root(const char *source,
                                         char *destination,
                                         size_t capacity)
{
    if (!is_windows_absolute_path(source) || !destination || capacity < 4u)
        return 0;
    char components[AC1_FILE_PATH_CAPACITY];
    if (!normalize_guest_components(source + 3u, components,
                                    sizeof(components)))
        return 0;
    size_t components_length = strlen(components);
    if (components_length + 4u > capacity) return 0;
    destination[0] = (char)ascii_path_fold((unsigned char)source[0]);
    destination[1] = ':';
    destination[2] = '/';
    memcpy(destination + 3u, components, components_length + 1u);
    return 1;
}

static int guest_path_to_relative(const ac1_files *files, const char *guest_path,
                                  char *relative_path, size_t capacity)
{
    if (is_windows_absolute_path(guest_path)) {
        if (!files->guest_root[0] ||
            !normalize_guest_absolute_root(guest_path, relative_path,
                                           capacity))
            return 0;
        size_t root_length = strlen(files->guest_root);
        size_t path_length = strlen(relative_path);
        if (path_length <= root_length || relative_path[root_length] != '/')
            return 0;
        for (size_t i = 0; i < root_length; ++i) {
            if (ascii_path_fold((unsigned char)relative_path[i]) !=
                ascii_path_fold((unsigned char)files->guest_root[i]))
                return 0;
        }
        size_t suffix_offset = root_length + 1u;
        size_t suffix_length = path_length - suffix_offset;
        if (!suffix_length) return 0;
        memmove(relative_path, relative_path + suffix_offset,
                suffix_length + 1u);
        return normalize_guest_path(relative_path, relative_path, capacity);
    }
    return normalize_guest_path(guest_path, relative_path, capacity);
}

#ifdef _WIN32
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
    for (size_t i = 0; path[i]; ++i)
        if (path[i] == '/') path[i] = '\\';
    return 1;
}

static int windows_parent_directories_are_safe(char *path,
                                               size_t relative_offset,
                                               uint32_t *error_out)
{
    size_t length = strlen(path);
    for (size_t i = relative_offset; i < length; ++i) {
        if (path[i] != '\\') continue;
        path[i] = '\0';
        HANDLE directory = CreateFileA(path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            NULL);
        path[i] = '\\';
        if (directory == INVALID_HANDLE_VALUE) {
            *error_out = (uint32_t)GetLastError();
            return 0;
        }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(directory, &info)) {
            *error_out = (uint32_t)GetLastError();
            CloseHandle(directory);
            return 0;
        }
        CloseHandle(directory);
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            *error_out = AC1_ERROR_ACCESS_DENIED;
            return 0;
        }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            *error_out = AC1_ERROR_PATH_NOT_FOUND;
            return 0;
        }
    }
    return 1;
}
#endif

#ifndef _WIN32
static int ascii_path_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (ascii_path_fold((unsigned char)*left++) !=
            ascii_path_fold((unsigned char)*right++))
            return 0;
    }
    return *left == *right;
}

static int symlink_at(int directory, const char *name)
{
    struct stat info;
    return fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISLNK(info.st_mode);
}

static int openat_case_insensitive(int directory, const char *name, int flags,
                                   uint32_t missing_error,
                                   uint32_t *error_out)
{
    int fd = openat(directory, name, flags);
    if (fd >= 0) return fd;
    int exact_error = errno;
    if (symlink_at(directory, name)) {
        *error_out = AC1_ERROR_ACCESS_DENIED;
        return -1;
    }
    if (exact_error != ENOENT) {
        *error_out = exact_error == ENOTDIR ? AC1_ERROR_PATH_NOT_FOUND
            : error_from_errno(exact_error);
        return -1;
    }

    int scan_fd = openat(directory, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scan_fd < 0) {
        *error_out = error_from_errno(errno);
        return -1;
    }
    DIR *entries = fdopendir(scan_fd);
    if (!entries) {
        int saved_error = errno;
        close(scan_fd);
        *error_out = error_from_errno(saved_error);
        return -1;
    }

    char match[AC1_FILE_PATH_CAPACITY];
    match[0] = '\0';
    int match_count = 0;
    struct dirent *entry;
    while ((entry = readdir(entries)) != NULL) {
        if (!ascii_path_equal(entry->d_name, name)) continue;
        size_t entry_length = strlen(entry->d_name);
        if (entry_length >= sizeof(match)) {
            match_count = 2;
            break;
        }
        if (strcmp(entry->d_name, name) == 0) {
            memcpy(match, entry->d_name, entry_length + 1u);
            match_count = 1;
            break; /* An exact spelling wins, as on Win32 lookup. */
        }
        if (!match_count) {
            memcpy(match, entry->d_name, entry_length + 1u);
            match_count = 1;
        } else {
            match_count = 2;
            break;
        }
    }
    closedir(entries);
    if (match_count > 1) {
        *error_out = AC1_ERROR_INVALID_NAME;
        return -1;
    }
    if (!match_count) {
        *error_out = missing_error;
        return -1;
    }

    fd = openat(directory, match, flags);
    if (fd >= 0) return fd;
    int saved_error = errno;
    if (symlink_at(directory, match))
        *error_out = AC1_ERROR_ACCESS_DENIED;
    else if (saved_error == ENOENT)
        *error_out = missing_error;
    else if (saved_error == ENOTDIR)
        *error_out = AC1_ERROR_PATH_NOT_FOUND;
    else
        *error_out = error_from_errno(saved_error);
    return -1;
}

static int open_asset_posix(ac1_files *files, char *relative_path,
                            uint32_t *error_out)
{
    int directory = openat(files->root_fd, ".",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        *error_out = error_from_errno(errno);
        return -1;
    }

    char *component = relative_path;
    for (;;) {
        char *separator = strchr(component, '/');
        if (separator) *separator = '\0';
        if (separator) {
            int next = openat_case_insensitive(directory, component,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                AC1_ERROR_PATH_NOT_FOUND, error_out);
            if (next < 0) {
                close(directory);
                return -1;
            }
            close(directory);
            directory = next;
            component = separator + 1u;
            continue;
        }

        int file = openat_case_insensitive(directory, component,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW,
            AC1_ERROR_FILE_NOT_FOUND, error_out);
        close(directory);
        if (file < 0) return -1;
        struct stat info;
        if (fstat(file, &info) != 0) {
            *error_out = error_from_errno(errno);
            close(file);
            return -1;
        }
        if (!S_ISREG(info.st_mode)) {
            *error_out = AC1_ERROR_ACCESS_DENIED;
            close(file);
            return -1;
        }
        return file;
    }
}
#endif

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

    /* The service only opens observed read-only game assets. In particular no
     * writes or creation dispositions can reach the original installation. */
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
        (flags_and_attributes != AC1_FILE_FLAG_RANDOM_ACCESS &&
         flags_and_attributes != AC1_GEAR_STREAM_FLAGS) || template_handle)
        return AC1_FILES_UNSUPPORTED;

    if (!guest_path || !guest_path[0]) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    char relative_path[AC1_FILE_PATH_CAPACITY];
    if (!guest_path_to_relative(files, guest_path, relative_path,
                                sizeof(relative_path))) {
        /* Absolute paths must match the configured guest root exactly at a
         * component boundary; traversal is never normalized away. */
        return AC1_FILES_UNSUPPORTED;
    }

    size_t slot_index;
    if (!allocate_slot(files, &slot_index)) {
        *error_out = AC1_ERROR_TOO_MANY_OPEN_FILES;
        return 1;
    }

#ifdef _WIN32
    char path[AC1_FILE_PATH_CAPACITY];
    if (!make_asset_path(files, relative_path, path, sizeof(path))) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    size_t root_length = strlen(files->root);
    int root_has_separator = root_length &&
        (files->root[root_length - 1u] == '/' ||
         files->root[root_length - 1u] == '\\');
    size_t relative_offset = root_length + (root_has_separator ? 0u : 1u);
    if (!windows_parent_directories_are_safe(path, relative_offset,
                                             error_out))
        return AC1_FILES_HANDLED;
    HANDLE host = CreateFileA(path, GENERIC_READ, share_mode, NULL,
                              OPEN_EXISTING,
                              flags_and_attributes |
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
    int host = open_asset_posix(files, relative_path, error_out);
    if (host < 0) {
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
    cancel_file_operations(files, guest_handle, host);
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

int ac1_files_get_size(ac1_files *files, uint32_t guest_handle,
                       uint64_t *size_out, uint32_t *error_out)
{
    if (!files || !error_out) return AC1_FILES_NOT_HANDLED;
    if (size_out) *size_out = 0u;
    *error_out = 0u;
    size_t slot_index = 0;
    int lookup = lookup_file_slot(files, guest_handle, &slot_index, error_out);
    if (lookup != AC1_FILES_HANDLED || *error_out) return lookup;
    if (!size_out) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
#ifdef _WIN32
    LARGE_INTEGER size;
    if (!GetFileSizeEx(files->slots[slot_index], &size)) {
        *error_out = (uint32_t)GetLastError();
        return AC1_FILES_HANDLED;
    }
    if (size.QuadPart < 0) {
        *error_out = AC1_ERROR_IO_DEVICE;
        return AC1_FILES_HANDLED;
    }
    *size_out = (uint64_t)size.QuadPart;
#else
    struct stat info;
    if (fstat(files->slots[slot_index], &info) != 0) {
        *error_out = error_from_errno(errno);
        return AC1_FILES_HANDLED;
    }
    if (info.st_size < 0) {
        *error_out = AC1_ERROR_IO_DEVICE;
        return AC1_FILES_HANDLED;
    }
    *size_out = (uint64_t)info.st_size;
#endif
    *error_out = 0u;
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

static ac1_file_async_operation *find_async_operation(
    ac1_files *files, uint32_t guest_handle, uint32_t guest_overlapped_address)
{
    for (size_t i = 0; i < AC1_FILE_ASYNC_SLOTS; ++i) {
        ac1_file_async_operation *operation = &files->async_operations[i];
        if (operation->active && operation->guest_handle == guest_handle &&
            operation->guest_overlapped_address == guest_overlapped_address)
            return operation;
    }
    return NULL;
}

static ac1_file_async_operation *allocate_async_operation(ac1_files *files)
{
    for (size_t i = 0; i < AC1_FILE_ASYNC_SLOTS; ++i) {
        ac1_file_async_operation *operation = &files->async_operations[i];
        if (!operation->active) return operation;
    }
    return NULL;
}

static int refresh_pending_async_operation(ac1_files *files,
                                           size_t file_slot_index,
                                           ac1_file_async_operation *operation)
{
    if (!operation->pending) return 1;
#ifdef _WIN32
    DWORD bytes_transferred = 0;
    if (GetOverlappedResult(files->slots[file_slot_index],
                            &operation->native_overlapped,
                            &bytes_transferred, FALSE)) {
        operation->pending = 0;
        operation->completion_error = 0u;
        return 1;
    }
    DWORD native_error = GetLastError();
    if (native_error == ERROR_IO_INCOMPLETE) return 0;
    operation->pending = 0;
    operation->completion_error = (uint32_t)native_error;
    return 1;
#else
    (void)files;
    (void)file_slot_index;
    operation->pending = 0;
    return 1;
#endif
}

static void copy_overlapped_snapshot(
    const ac1_file_async_operation *operation,
    ac1_files_overlapped_snapshot *snapshot_out)
{
    if (!snapshot_out) return;
    if (!operation->snapshot_valid) return;
#ifdef _WIN32
    snapshot_out->internal = (uint32_t)operation->native_overlapped.Internal;
    snapshot_out->internal_high =
        (uint32_t)operation->native_overlapped.InternalHigh;
#else
    snapshot_out->internal = operation->internal_status;
    snapshot_out->internal_high = operation->internal_high;
#endif
    snapshot_out->valid = 1u;
}

int ac1_files_read_overlapped(ac1_files *files, uint32_t guest_handle,
                              void *buffer, uint32_t requested_bytes,
                              uint32_t guest_overlapped_address,
                              uint32_t offset_low, uint32_t offset_high,
                              void *host_event,
                              uint32_t *error_out,
                              ac1_files_overlapped_snapshot *snapshot_out)
{
    if (!files || !error_out) return AC1_FILES_NOT_HANDLED;
#ifndef _WIN32
    (void)host_event;
#endif
    if (snapshot_out) memset(snapshot_out, 0, sizeof(*snapshot_out));
    *error_out = 0u;
    size_t slot_index = 0;
    int lookup = lookup_file_slot(files, guest_handle, &slot_index, error_out);
    if (lookup != AC1_FILES_HANDLED || *error_out) return lookup;
    if (!guest_overlapped_address || (requested_bytes && !buffer)) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    ac1_file_async_operation *operation = find_async_operation(
        files, guest_handle, guest_overlapped_address);
    if (operation && !refresh_pending_async_operation(files, slot_index,
                                                       operation)) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    if (operation) clear_async_operation(operation);
    else operation = allocate_async_operation(files);
    if (!operation) {
        *error_out = AC1_ERROR_TOO_MANY_OPEN_FILES;
        return AC1_FILES_HANDLED;
    }
    memset(operation, 0, sizeof(*operation));
    operation->active = 1;
    operation->guest_handle = guest_handle;
    operation->guest_overlapped_address = guest_overlapped_address;
    operation->requested_bytes = requested_bytes;
    operation->buffer = buffer;

#ifdef _WIN32
    operation->native_overlapped.Offset = offset_low;
    operation->native_overlapped.OffsetHigh = offset_high;
    if (host_event && !DuplicateHandle(GetCurrentProcess(),
            (HANDLE)host_event, GetCurrentProcess(),
            &operation->duplicated_event, 0u, FALSE, DUPLICATE_SAME_ACCESS)) {
        uint32_t native_error = (uint32_t)GetLastError();
        clear_async_operation(operation);
        *error_out = native_error;
        return AC1_FILES_HANDLED;
    }
    operation->native_overlapped.hEvent = operation->duplicated_event;
    if (!ReadFile(files->slots[slot_index], buffer, requested_bytes, NULL,
                  &operation->native_overlapped)) {
        DWORD native_error = GetLastError();
        operation->pending = native_error == ERROR_IO_PENDING;
        operation->completion_error = (uint32_t)native_error;
        *error_out = (uint32_t)native_error;
    }
    else operation->completion_error = 0u;
    operation->snapshot_valid = 1u;
#else
    uint64_t offset = ((uint64_t)offset_high << 32) | offset_low;
    if (offset > (uint64_t)INT64_MAX) {
        clear_async_operation(operation);
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    ssize_t count;
    do {
        count = pread(files->slots[slot_index], buffer,
                      (size_t)requested_bytes, (off_t)offset);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        int saved_error = errno;
        operation->completion_error = error_from_errno(saved_error);
        *error_out = operation->completion_error;
        /* The host has no NTSTATUS to expose for this native I/O failure. */
        operation->snapshot_valid = 0u;
        return AC1_FILES_HANDLED;
    }
    if (requested_bytes && count == 0) {
        operation->completion_error = AC1_ERROR_HANDLE_EOF;
        operation->internal_status = AC1_STATUS_END_OF_FILE;
        operation->snapshot_valid = 1u;
        *error_out = AC1_ERROR_HANDLE_EOF;
    } else {
        operation->bytes_transferred = (uint32_t)count;
        operation->internal_status = 0u;
        operation->internal_high = (uint32_t)count;
        operation->completion_error = 0u;
        operation->snapshot_valid = 1u;
    }
#endif
    copy_overlapped_snapshot(operation, snapshot_out);
    return AC1_FILES_HANDLED;
}

int ac1_files_get_overlapped_result(ac1_files *files, uint32_t guest_handle,
                                    uint32_t guest_overlapped_address,
                                    uint32_t wait,
                                    uint32_t *bytes_transferred_out,
                                    uint32_t *error_out,
                                    ac1_files_overlapped_snapshot *snapshot_out)
{
    if (!files || !error_out) return AC1_FILES_NOT_HANDLED;
    if (snapshot_out) memset(snapshot_out, 0, sizeof(*snapshot_out));
    *error_out = 0u;
    if (bytes_transferred_out) *bytes_transferred_out = 0u;
    size_t slot_index = 0;
    int lookup = lookup_file_slot(files, guest_handle, &slot_index, error_out);
    if (lookup != AC1_FILES_HANDLED || *error_out) return lookup;
    if (!guest_overlapped_address || !bytes_transferred_out) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
    if (wait != 0u) return AC1_FILES_UNSUPPORTED;
    ac1_file_async_operation *operation = find_async_operation(
        files, guest_handle, guest_overlapped_address);
    if (!operation) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return AC1_FILES_HANDLED;
    }
#ifdef _WIN32
    if (operation->pending) {
        DWORD bytes_transferred = 0;
        if (!GetOverlappedResult(files->slots[slot_index],
                                 &operation->native_overlapped,
                                 &bytes_transferred, FALSE)) {
            DWORD native_error = GetLastError();
            operation->completion_error = (uint32_t)native_error;
            if (native_error != ERROR_IO_INCOMPLETE)
                operation->pending = 0;
            *error_out = (uint32_t)native_error;
            copy_overlapped_snapshot(operation, snapshot_out);
            return AC1_FILES_HANDLED;
        }
        operation->pending = 0;
        operation->completion_error = 0u;
    }
    *bytes_transferred_out =
        (uint32_t)operation->native_overlapped.InternalHigh;
#else
    *bytes_transferred_out = operation->bytes_transferred;
#endif
    copy_overlapped_snapshot(operation, snapshot_out);
    *error_out = operation->completion_error;
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
