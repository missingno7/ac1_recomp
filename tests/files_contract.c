#define _POSIX_C_SOURCE 200809L
#include "ac1_files.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "files_contract: %s (line %d)\n", message, __LINE__); \
        goto cleanup; \
    } \
} while (0)

static int join_path(char *output, size_t capacity, const char *root,
                     const char *relative);

static int make_scratch_root(char *path, size_t capacity)
{
#ifdef _WIN32
    char temp_path[MAX_PATH];
    char temp_file[MAX_PATH];
    const char *configured_base = getenv("AC1_FILES_TEST_ROOT");
    DWORD length;
    if (configured_base && configured_base[0]) {
        length = (DWORD)strlen(configured_base);
        if (length >= sizeof(temp_path)) return 0;
        memcpy(temp_path, configured_base, (size_t)length + 1u);
        if (length && temp_path[length - 1u] != '\\' &&
            temp_path[length - 1u] != '/') {
            if (length + 1u >= sizeof(temp_path)) return 0;
            temp_path[length++] = '\\';
            temp_path[length] = '\0';
        }
    } else {
        length = GetTempPathA((DWORD)sizeof(temp_path), temp_path);
    }
    if (!length || length >= sizeof(temp_path) ||
        !GetTempFileNameA(temp_path, "ac1", 0, temp_file) ||
        !DeleteFileA(temp_file) || !CreateDirectoryA(temp_file, NULL))
        return 0;
    if (strlen(temp_file) >= capacity) {
        RemoveDirectoryA(temp_file);
        return 0;
    }
    memcpy(path, temp_file, strlen(temp_file) + 1u);
    return 1;
#else
    const char *base = getenv("AC1_FILES_TEST_ROOT");
    if (!base || !base[0]) base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    int count = snprintf(path, capacity, "%s/ac1_files_contract_XXXXXX", base);
    return count > 0 && (size_t)count < capacity && mkdtemp(path) != NULL;
#endif
}

#ifdef _WIN32
static int verify_native_trailing_dot_normalization(const char *root)
{
    char path[32768];
    int length = snprintf(path, sizeof(path), "%s\\Videos.\\LoadingScreen.bik",
                          root);
    if (length <= 0 || (size_t)length >= sizeof(path)) return 0;
    HANDLE file = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Win32 trailing-dot probe failed: %s (error=%lu)\n",
                path, (unsigned long)GetLastError());
        return 0;
    }
    CloseHandle(file);
    return 1;
}
#endif

static int join_path(char *output, size_t capacity, const char *root,
                     const char *relative)
{
    size_t root_length = strlen(root);
    const char *separator = root_length &&
        (root[root_length - 1u] == '/' || root[root_length - 1u] == '\\')
        ? "" :
#ifdef _WIN32
        "\\";
#else
        "/";
#endif
    int written = snprintf(output, capacity, "%s%s%s", root, separator,
                           relative);
    return written > 0 && (size_t)written < capacity;
}

static int write_fixture(const char *root, const char *relative,
                         const uint8_t *bytes, size_t length)
{
    char path[32768];
    if (!join_path(path, sizeof(path), root, relative)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int wrote = fwrite(bytes, 1u, length, file) == length;
    int closed = fclose(file) == 0;
    return wrote && closed;
}

static int create_video_directory(const char *root)
{
    char path[32768];
    if (!join_path(path, sizeof(path), root, "Videos")) return 0;
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) != 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static int remove_fixture(const char *root, const char *relative)
{
    char path[32768];
    if (!join_path(path, sizeof(path), root, relative)) return 0;
#ifdef _WIN32
    return DeleteFileA(path) != 0;
#else
    return unlink(path) == 0;
#endif
}

static int remove_video_directory(const char *root)
{
    char path[32768];
    if (!join_path(path, sizeof(path), root, "Videos")) return 0;
#ifdef _WIN32
    return RemoveDirectoryA(path) != 0;
#else
    return rmdir(path) == 0;
#endif
}

static int remove_scratch_root(const char *path)
{
#ifdef _WIN32
    return RemoveDirectoryA(path) != 0;
#else
    return rmdir(path) == 0;
#endif
}

static int open_asset(ac1_files *files, const char *path, uint32_t flags,
                      uint32_t *handle, uint32_t *error)
{
    return ac1_files_open(files, path, 0x80000000u, 3u, 0, 3u, flags,
                          0u, handle, error);
}

static int check_relative_root(const char *root)
{
    char previous[32768];
#ifdef _WIN32
    DWORD length = GetCurrentDirectoryA((DWORD)sizeof(previous), previous);
    if (!length || length >= sizeof(previous) || !SetCurrentDirectoryA(root))
        return 0;
#else
    if (!getcwd(previous, sizeof(previous)) || chdir(root) != 0) return 0;
#endif
    ac1_files *files = ac1_files_create(".");
#ifdef _WIN32
    int restored = SetCurrentDirectoryA(previous) != 0;
#else
    int restored = chdir(previous) == 0;
#endif
    if (!files || !restored) {
        ac1_files_destroy(files);
        return 0;
    }
    uint32_t handle = UINT32_MAX, error = UINT32_MAX;
    int ok = open_asset(files, "DataPC.forge", 0x10000000u,
                        &handle, &error) == AC1_FILES_HANDLED &&
             handle != UINT32_MAX && error == 0u &&
             ac1_files_close(files, handle, &error) == AC1_FILES_HANDLED &&
             error == 0u;
    ac1_files_destroy(files);
    return ok;
}

static void *allocate_aligned_io_buffer(size_t size)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *buffer = NULL;
    if (posix_memalign(&buffer, 4096u, size) != 0) return NULL;
    return buffer;
#endif
}

static void free_aligned_io_buffer(void *buffer)
{
#ifdef _WIN32
    if (buffer) VirtualFree(buffer, 0u, MEM_RELEASE);
#else
    free(buffer);
#endif
}

static int check_overlapped_read(ac1_files *files,
                                 const uint8_t *expected_bytes,
                                 size_t size)
{
    void *buffer = allocate_aligned_io_buffer(size);
    if (!buffer) return 0;
#ifdef _WIN32
    HANDLE native_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!native_event) {
        free_aligned_io_buffer(buffer);
        return 0;
    }
    void *host_event = native_event;
#else
    void *host_event = NULL;
#endif
    uint32_t handle = UINT32_MAX;
    uint32_t error = UINT32_MAX;
    ac1_files_overlapped_snapshot snapshot;
    int ok = open_asset(files, "AsyncData.bin", 0x70000001u,
                        &handle, &error) == AC1_FILES_HANDLED &&
             handle != UINT32_MAX && error == 0u;
    if (!ok) {
        fprintf(stderr, "overlapped fixture open failed: handle=%08x error=%u\n",
                handle, error);
        goto done;
    }

    uint32_t guest_overlapped = 0x12345678u;
    if (ac1_files_read_overlapped(files, handle, buffer, (uint32_t)size,
            guest_overlapped, 0u, 0u, host_event, &error, &snapshot) !=
                AC1_FILES_HANDLED ||
        (error != 0u && error != 997u)) {
        fprintf(stderr, "overlapped ReadFile start failed: error=%u\n", error);
        ok = 0;
        goto close_file;
    }
    if (!snapshot.valid) {
        fprintf(stderr, "ReadFile did not expose native OVERLAPPED fields\n");
        ok = 0;
        goto close_file;
    }
    uint32_t transferred = UINT32_MAX;
    for (unsigned attempt = 0; attempt < 10000u; ++attempt) {
        if (ac1_files_get_overlapped_result(files, handle,
                guest_overlapped, 0u, &transferred, &error, &snapshot) !=
                AC1_FILES_HANDLED) {
            fprintf(stderr, "GetOverlappedResult was not handled\n");
            ok = 0;
            goto close_file;
        }
        if (error != 996u) break;
#ifdef _WIN32
        Sleep(1u);
#endif
    }
    if (error != 0u || transferred != size ||
        memcmp(buffer, expected_bytes, size) != 0 || !snapshot.valid ||
        snapshot.internal != 0u || snapshot.internal_high != size) {
        fprintf(stderr, "overlapped completion mismatch: bytes=%u error=%u\n",
                transferred, error);
        ok = 0;
        goto close_file;
    }
#ifdef _WIN32
    if (WaitForSingleObject(native_event, 0u) != WAIT_OBJECT_0) {
        fprintf(stderr, "native OVERLAPPED event was not signaled\n");
        ok = 0;
        goto close_file;
    }
    ResetEvent(native_event);
#endif

    uint32_t first_internal = snapshot.internal;
    uint32_t first_internal_high = snapshot.internal_high;
    if (ac1_files_get_overlapped_result(files, handle, guest_overlapped, 0u,
            &transferred, &error, &snapshot) != AC1_FILES_HANDLED ||
        error != 0u || transferred != size || !snapshot.valid ||
        snapshot.internal != first_internal ||
        snapshot.internal_high != first_internal_high) {
        fprintf(stderr, "repeated GetOverlappedResult consumed completion\n");
        ok = 0;
        goto close_file;
    }
    if (ac1_files_read_overlapped(files, handle, buffer, (uint32_t)size,
            guest_overlapped, 0u, 0u, host_event, &error, &snapshot) !=
            AC1_FILES_HANDLED || (error != 0u && error != 997u)) {
        fprintf(stderr, "same-key ReadFile did not reuse terminal operation\n");
        ok = 0;
        goto close_file;
    }
    for (unsigned attempt = 0; attempt < 10000u; ++attempt) {
        if (ac1_files_get_overlapped_result(files, handle,
                guest_overlapped, 0u, &transferred, &error, &snapshot) !=
                AC1_FILES_HANDLED) {
            ok = 0;
            goto close_file;
        }
        if (error != 996u) break;
#ifdef _WIN32
        Sleep(1u);
#endif
    }
    if (error != 0u || transferred != size || !snapshot.valid ||
        snapshot.internal_high != size) {
        fprintf(stderr, "same-key reused operation did not complete\n");
        ok = 0;
        goto close_file;
    }
#ifdef _WIN32
    ResetEvent(native_event);
#endif

    /* Beyond EOF is ERROR_HANDLE_EOF for this observed overlapped ReadFile
     * path, unlike a synchronous ReadFile zero-byte EOF success. */
    if (ac1_files_read_overlapped(files, handle, buffer, (uint32_t)size,
            guest_overlapped + 4u, (uint32_t)size, 0u, host_event, &error,
            &snapshot) !=
        AC1_FILES_HANDLED) {
        ok = 0;
        goto close_file;
    }
    if (error != 38u) {
        if (error != 0u && error != 997u) {
            fprintf(stderr, "overlapped EOF start mismatch: error=%u\n", error);
            ok = 0;
            goto close_file;
        }
        transferred = UINT32_MAX;
        for (unsigned attempt = 0; attempt < 10000u; ++attempt) {
            if (ac1_files_get_overlapped_result(files, handle,
                    guest_overlapped + 4u, 0u, &transferred, &error,
                    &snapshot) !=
                AC1_FILES_HANDLED) {
                ok = 0;
                goto close_file;
            }
            if (error != 996u) break;
#ifdef _WIN32
            Sleep(1u);
#endif
        }
        if (error != 38u) {
            fprintf(stderr, "overlapped EOF completion mismatch: error=%u bytes=%u\n",
                    error, transferred);
            ok = 0;
            goto close_file;
        }
    }

    if (ac1_files_read_overlapped(files, handle, buffer, (uint32_t)size,
            guest_overlapped + 8u, 0u, 0u, host_event, &error, &snapshot) !=
                AC1_FILES_HANDLED ||
        (error != 0u && error != 997u)) {
        ok = 0;
        goto close_file;
    }
    ac1_files_cancel_pending(files);
    if (ac1_files_get_overlapped_result(files, handle,
            guest_overlapped + 8u, 0u, &transferred, &error,
            &snapshot) !=
            AC1_FILES_HANDLED || error != 87u) {
        ok = 0;
        goto close_file;
    }

close_file:
    if (ac1_files_close(files, handle, &error) != AC1_FILES_HANDLED || error)
        ok = 0;
done:
#ifdef _WIN32
    CloseHandle(native_event);
#endif
    free_aligned_io_buffer(buffer);
    return ok;
}

int main(void)
{
    static const uint8_t archive[] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F
    };
    static const uint8_t video[] = { 0x42, 0x49, 0x4B, 0x00, 0x7F };
    uint8_t async_fixture[4096];
    for (size_t i = 0; i < sizeof(async_fixture); ++i)
        async_fixture[i] = (uint8_t)(i * 13u + 7u);
    char root[32768] = { 0 };
    char trailing_root[32768];
    ac1_files *files = NULL;
    ac1_files *trailing_root_files = NULL;
    int passed = 0;
    uint32_t handle = UINT32_MAX;
    uint32_t error = UINT32_MAX;
    uint64_t size = UINT64_MAX;
    uint32_t count = UINT32_MAX;
    uint8_t buffer[16] = { 0 };
    uint32_t low = UINT32_MAX;
    ac1_files_overlapped_snapshot snapshot;

    CHECK(make_scratch_root(root, sizeof(root)), "create temporary root");
    CHECK(create_video_directory(root), "create Videos fixture directory");
    CHECK(write_fixture(root, "DataPC.forge", archive, sizeof(archive)),
          "write archive fixture");
    CHECK(write_fixture(root, "Videos/LoadingScreen.bik", video,
                        sizeof(video)), "write nested video fixture");
    CHECK(write_fixture(root, "AsyncData.bin", async_fixture,
                        sizeof(async_fixture)), "write async fixture");
#ifdef _WIN32
    CHECK(verify_native_trailing_dot_normalization(root),
          "native Win32 opens a trailing-dot component alias");
#endif
    CHECK(check_relative_root(root), "relative host-root compatibility");

#ifdef _WIN32
    const char *trailing_separator = "\\";
#else
    const char *trailing_separator = "/";
#endif
    int trailing_length = snprintf(trailing_root, sizeof(trailing_root), "%s%s",
                                   root, trailing_separator);
    CHECK(trailing_length > 0 && (size_t)trailing_length < sizeof(trailing_root),
          "construct trailing-separator host root");
    trailing_root_files = ac1_files_create_with_guest_root(
        trailing_root, "Z:\\Virtual Game\\Root");
    CHECK(trailing_root_files != NULL &&
          open_asset(trailing_root_files, "DataPC.forge", 0x10000000u,
                     &handle, &error) == AC1_FILES_HANDLED &&
          handle != UINT32_MAX && error == 0u,
          "host root with a trailing separator");
    CHECK(ac1_files_close(trailing_root_files, handle, &error) ==
              AC1_FILES_HANDLED && error == 0u,
          "close trailing-root fixture handle");
    ac1_files_destroy(trailing_root_files);
    trailing_root_files = NULL;

    files = ac1_files_create_with_guest_root(root, "Z:\\Virtual Game\\Root");
    CHECK(files != NULL, "create service with logical Windows root");
    CHECK(check_overlapped_read(files, async_fixture, sizeof(async_fixture)),
          "overlapped read completion/content/EOF");
    CHECK(open_asset(files, "z:/virtual game/root/DataPC.forge",
                     0x70000001u, &handle, &error) == AC1_FILES_HANDLED &&
          handle != UINT32_MAX && error == 0u,
          "case-insensitive absolute guest root mapping");
    CHECK(ac1_files_get_size(files, handle, &size, &error) == AC1_FILES_HANDLED &&
          error == 0u && size == sizeof(archive), "exact file size");
    CHECK(ac1_files_close(files, handle, &error) == AC1_FILES_HANDLED &&
          error == 0u, "close absolute-mapped handle");
    CHECK(open_asset(files, "DataPC.forge", 0x10000000u,
                     &handle, &error) == AC1_FILES_HANDLED &&
          handle != UINT32_MAX && error == 0u,
          "open buffered read handle");
    CHECK(ac1_files_read(files, handle, buffer, 6u, &count, &error) ==
              AC1_FILES_HANDLED && error == 0u && count == 6u &&
          memcmp(buffer, archive, 6u) == 0, "read fixture bytes");
    CHECK(ac1_files_seek(files, handle, 13u, 0, 0u, 0u, &low, NULL,
                         &error) == AC1_FILES_HANDLED && error == 0u && low == 13u,
          "seek to tail");
    CHECK(ac1_files_read(files, handle, buffer, sizeof(buffer), &count, &error) ==
              AC1_FILES_HANDLED && error == 0u && count == 3u &&
          memcmp(buffer, archive + 13u, 3u) == 0, "short read at EOF");
    CHECK(ac1_files_read(files, handle, buffer, sizeof(buffer), &count, &error) ==
              AC1_FILES_HANDLED && error == 0u && count == 0u,
          "EOF is a successful zero-byte read");

    CHECK(open_asset(files, "videos\\loadingscreen.bik", 0x70000001u,
                     &handle, &error) == AC1_FILES_HANDLED &&
          handle != UINT32_MAX && error == 0u,
          "nested case-insensitive lookup");
    CHECK(ac1_files_close(files, handle, &error) == AC1_FILES_HANDLED &&
          error == 0u, "close nested fixture");
    CHECK(open_asset(files, "Z:\\Virtual Game\\Rooted\\DataPC.forge",
                     0x70000001u, &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject absolute prefix lookalike");
    CHECK(open_asset(files,
                     "Z:\\Virtual Game\\Root\\..\\Root\\DataPC.forge",
                     0x70000001u, &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject absolute traversal");
    CHECK(open_asset(files,
                     "Z:\\Virtual Game\\Root\\.. \\DataPC.forge",
                     0x70000001u, &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject absolute traversal with trailing space");
    CHECK(open_asset(files, "..\\DataPC.forge", 0x10000000u,
                     &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject relative traversal");
    CHECK(open_asset(files, ".. \\DataPC.forge", 0x10000000u,
                     &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject traversal with trailing space");
    CHECK(open_asset(files, "Videos.\\LoadingScreen.bik", 0x70000001u,
                     &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject component with trailing dot");
    CHECK(open_asset(files, "Videos \\LoadingScreen.bik", 0x70000001u,
                     &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject component with trailing space");
    CHECK(open_asset(files, "NUL.txt", 0x10000000u,
                     &handle, &error) == AC1_FILES_UNSUPPORTED,
          "reject reserved DOS device component");
    CHECK(ac1_files_read(files, 0xF1000000u, NULL, 1u, &count, &error) ==
              AC1_FILES_NOT_HANDLED,
          "foreign synthetic handle remains unhandled");
    CHECK(ac1_files_read_overlapped(files, 0xF1000000u, NULL, 1u, 0u,
              0u, 0u, NULL, &error, &snapshot) == AC1_FILES_NOT_HANDLED,
          "foreign async handle takes precedence over bad arguments");
    CHECK(ac1_files_get_overlapped_result(files, 0xF1000000u, 0u, 99u,
              NULL, &error, &snapshot) == AC1_FILES_NOT_HANDLED,
          "foreign result handle takes precedence over bad arguments");
    CHECK(ac1_files_close(files, handle, &error) == AC1_FILES_NOT_HANDLED,
          "foreign token close remains unhandled");

    CHECK(open_asset(files, "DataPC.forge", 0x10000000u,
                     &handle, &error) == AC1_FILES_HANDLED &&
          handle != UINT32_MAX && error == 0u,
          "open relative archive for invalid-handle checks");
    CHECK(ac1_files_close(files, handle, &error) == AC1_FILES_HANDLED &&
          error == 0u, "close archive fixture");
    CHECK(ac1_files_get_size(files, handle, &size, &error) == AC1_FILES_HANDLED &&
          error == 6u, "closed owned handle reports invalid handle");
    CHECK(ac1_files_seek(files, handle, 0u, 0, 0u, 99u, &low, NULL,
                         &error) == AC1_FILES_HANDLED && error == 6u,
          "owned-handle validation precedes seek-method validation");

    passed = 1;

cleanup:
    ac1_files_destroy(trailing_root_files);
    ac1_files_destroy(files);
    if (root[0]) {
        remove_fixture(root, "DataPC.forge");
        remove_fixture(root, "AsyncData.bin");
        remove_fixture(root, "Videos/LoadingScreen.bik");
        remove_video_directory(root);
        remove_scratch_root(root);
    }
    if (passed) {
        puts("PASS: file roots/containment, sync and overlapped read/EOF, "
             "size/seek, and handle validation");
        return 0;
    }
    return 1;
}
