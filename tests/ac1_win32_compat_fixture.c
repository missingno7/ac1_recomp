/* Focused oracle for the actual guest adapter. No generated game code.
 * Scheduler callbacks are synchronous fixture doubles; filesystem/marshalling
 * and every API under test come from ac1_win32_compat.inc itself. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ac1_files.h"
#include "pcrecomp_heap.h"
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <windows.h>

#define AC1_STD_INPUT_HANDLE 0xe1000001u
#define AC1_STD_OUTPUT_HANDLE 0xe1000002u
#define AC1_STD_ERROR_HANDLE 0xe1000003u
#define AC1_KERNEL32_HANDLE 0x76000000u
#define AC1_THREAD_HANDLE_BASE 0xe3000000u
#define AC1_MAX_THREADS 32
#define AC1_WAIT_OBJECT_0 0
#define AC1_STILL_ACTIVE 259

typedef struct {
    uint32_t eax, esp, eip;
} CPU;
typedef struct {
    uint32_t token;
    char dll[96], name[160];
    uint16_t ordinal;
    int by_ordinal;
} import_record;
typedef struct {
    uint32_t handle;
    int kind, manual_reset;
    HANDLE host_event;
} sync_object;
typedef struct {
    uint32_t id, handle, exit_code;
    int closed, finished, blocked, wait_kind;
    uint32_t wait_handle, wait_count, wait_handles[64], wait_result;
    CPU cpu;
} ac1_guest_thread;
typedef struct {
    unsigned count;
    ac1_guest_thread threads[32];
} ac1_scheduler;
static ac1_scheduler scheduler;
static ac1_scheduler *active_scheduler = &scheduler;
static ac1_guest_thread *active_guest_thread = &scheduler.threads[0];
static pcrecomp_heap *process_heap;
static ac1_files *guest_files;
static uint32_t last_error, image_base = 0x100000, image_size_loaded = 0x10000;
static struct {
    uint32_t load_base;
} bink_image;
static int bink_image_mapped;
static char bink_module_path[32768], exe_path_copy[32768], guest_current_directory[32768],
    userdata[32768];
static unsigned char memory[4 * 1024 * 1024];
static uint32_t next_heap = 0x200000, allocs[4096];
static unsigned alloc_count;
static jmp_buf trap;
static char trap_reason[512];
static unsigned callback_count;
static sync_object fixture_event = {0xe2000000u, 1, 1, NULL};
static void *guest_memory_try_ptr(uint32_t p, size_t n)
{
    return p && (uint64_t)p + n <= sizeof(memory) ? memory + p : NULL;
}
static uint32_t rd32(uint32_t p)
{
    uint32_t n;
    memcpy(&n, memory + p, 4);
    return n;
}
static void wr32(uint32_t p, uint32_t n)
{
    memcpy(memory + p, &n, 4);
}
static void wr16(uint32_t p, uint16_t n)
{
    memcpy(memory + p, &n, 2);
}
static uint16_t image_u16(const uint8_t *p)
{
    uint16_t n;
    memcpy(&n, p, 2);
    return n;
}
static uint32_t image_u32(const uint8_t *p)
{
    uint32_t n;
    memcpy(&n, p, 4);
    return n;
}
static void return_stdcall(CPU *cpu, uint32_t bytes)
{
    cpu->eip = rd32(cpu->esp);
    cpu->esp += 4 + bytes;
}
static void stop_guest(const char *why, uint32_t token)
{
    (void)token;
    snprintf(trap_reason, sizeof(trap_reason), "%s", why);
    longjmp(trap, 1);
}
static int equal_ascii_fold(const char *a, const char *b)
{
    return !_stricmp(a, b);
}
static int copy_guest_string(uint32_t p, char *out, size_t cap)
{
    for (size_t j = 0; j < cap; ++j) {
        char *c = guest_memory_try_ptr(p + (uint32_t)j, 1);
        if (!c)
            return 0;
        out[j] = *c;
        if (!*c)
            return 1;
    }
    return 0;
}
uint32_t pcrecomp_heap_alloc(pcrecomp_heap *h, uint32_t n, int zero)
{
    (void)h;
    uint32_t p = next_heap;
    next_heap += (n + 15) & ~15u;
    if (next_heap > sizeof(memory) || alloc_count == 4096)
        return 0;
    if (zero)
        memset(memory + p, 0, n);
    allocs[alloc_count++] = p;
    return p;
}
int pcrecomp_heap_free(pcrecomp_heap *h, uint32_t p)
{
    (void)h;
    for (unsigned j = 0; j < alloc_count; ++j)
        if (allocs[j] == p) {
            allocs[j] = 0;
            return 1;
        }
    return 0;
}
static int get_user_data_root(char *out, size_t cap)
{
    return snprintf(out, cap, "%s", userdata) > 0;
}
static int profile_path_is_in_userdata(const char *p)
{
    size_t n = strlen(userdata);
    return !_strnicmp(p, userdata, n) && (p[n] == '/' || p[n] == '\\');
}
static int profile_path_is_absolute(const char *p)
{
    return strlen(p) > 2 && p[1] == ':';
}
static int ensure_directory_tree(const char *p)
{
    return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
static ac1_guest_thread *thread_for_handle(uint32_t h)
{
    for (unsigned j = 0; j < scheduler.count; ++j)
        if (scheduler.threads[j].handle == h && !scheduler.threads[j].closed)
            return &scheduler.threads[j];
    return NULL;
}
static sync_object *sync_object_for_handle(uint32_t h)
{
    return h == fixture_event.handle ? &fixture_event : NULL;
}
static void scheduler_lock(ac1_scheduler *s)
{
    (void)s;
}
static void scheduler_unlock(ac1_scheduler *s)
{
    (void)s;
}
static void scheduler_notify(ac1_scheduler *s)
{
    (void)s;
}
static int invoke_guest_stdcall_callback(CPU *c, uint32_t proc, const uint32_t *args, uint32_t n,
                                         uint32_t *r)
{
    (void)c;
    if (proc != 0x123456 || n != 1 || strcmp((char *)memory + args[0], "00000409"))
        return 0;
    ++callback_count;
    *r = 0;
    return 1;
}
static int handle_thread_import(CPU *c, const import_record *i)
{
    ac1_guest_thread *t = thread_for_handle(rd32(c->esp + 4));
    if (!strcmp(i->name, "GetThreadId")) {
        c->eax = t ? t->id : 0;
        if (!t)
            last_error = 6;
        return_stdcall(c, 4);
        return 1;
    }
    if (!strcmp(i->name, "GetExitCodeThread")) {
        c->eax = t != NULL;
        if (t)
            wr32(rd32(c->esp + 8), t->finished ? t->exit_code : 259);
        else
            last_error = 6;
        return_stdcall(c, 8);
        return 1;
    }
    return 0;
}
static int handle_sync_import(CPU *c, const import_record *i)
{
    if (!strcmp(i->name, "CloseHandle")) {
        ac1_guest_thread *t = thread_for_handle(rd32(c->esp + 4));
        c->eax = t != NULL;
        if (t)
            t->closed = 1;
        else
            last_error = 6;
        return_stdcall(c, 4);
        return 1;
    }
    return 0;
}
static int handle_heap_import(CPU *c, const import_record *i)
{
    (void)c;
    (void)i;
    return 0;
}
static int handle_import(CPU *c, const import_record *i)
{
    (void)c;
    (void)i;
    return 0;
}
#include "ac1_win32_compat.inc"

static unsigned checks, calls;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL line %u: %s (guest error %u native %lu)\n", __LINE__, #e,        \
                    last_error, GetLastError());                                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint32_t put(const char *s)
{
    static uint32_t next = 0x20000;
    uint32_t p = next;
    size_t n = strlen(s) + 1;
    memcpy(memory + p, s, n);
    next += (uint32_t)(n + 7) & ~7u;
    return p;
}
static uint32_t call_dll(const char *dll, const char *name, unsigned n, ...)
{
    CPU c = {0, 0x100, 0xabcdef01};
    import_record i = {0};
    i.token = 0xf1000000;
    strcpy(i.dll, dll);
    strcpy(i.name, name);
    if (name[0] == '#') {
        i.by_ordinal = 1;
        i.ordinal = (uint16_t)atoi(name + 1);
    }
    wr32(c.esp, 0x34567890);
    va_list ap;
    va_start(ap, n);
    for (unsigned j = 1; j <= n; ++j)
        wr32(c.esp + j * 4, va_arg(ap, uint32_t));
    va_end(ap);
    CHECK(handle_win32_compat_import(&c, &i));
    CHECK(c.esp == 0x104 + n * 4);
    CHECK(c.eip == 0x34567890);
    ++calls;
    return c.eax;
}
#define K(name, n, ...) call_dll("KERNEL32.dll", name, n, ##__VA_ARGS__)
#define U(name, n, ...) call_dll("USER32.dll", name, n, ##__VA_ARGS__)
#define D(dll, name, n, ...) call_dll(dll, name, n, ##__VA_ARGS__)
#define SENTINEL 0x1234abcd
static void errors(void)
{
    last_error = SENTINEL;
    SetLastError(SENTINEL);
}
static void test_time(void)
{
    static const SYSTEMTIME cases[] = {
        {1601, 1, 0, 1, 0, 0, 0, 0},        {1900, 2, 0, 28, 23, 59, 59, 999},
        {1970, 1, 0, 1, 0, 0, 0, 0},        {2000, 2, 0, 29, 10, 20, 30, 456},
        {2024, 12, 0, 31, 23, 59, 59, 999}, {30827, 12, 0, 31, 23, 59, 59, 999},
        {2023, 2, 0, 29, 0, 0, 0, 0},       {1600, 1, 0, 1, 0, 0, 0, 0},
        {2024, 13, 0, 1, 0, 0, 0, 0},       {2024, 1, 0, 1, 24, 0, 0, 0}};
    for (unsigned j = 0; j < sizeof(cases) / sizeof(cases[0]); ++j) {
        memcpy(memory + 0x1000, &cases[j], 16);
        FILETIME ft;
        errors();
        int native = SystemTimeToFileTime(&cases[j], &ft);
        DWORD err = GetLastError();
        CHECK(K("SystemTimeToFileTime", 2, 0x1000u, 0x1100u) == (uint32_t)native);
        CHECK(last_error == err);
        if (native) {
            CHECK(!memcmp(memory + 0x1100, &ft, 8));
            SYSTEMTIME st;
            CHECK(FileTimeToSystemTime(&ft, &st));
            CHECK(K("FileTimeToSystemTime", 2, 0x1100u, 0x1200u));
            CHECK(!memcmp(&st, memory + 0x1200, 16));
        }
    }
    uint64_t invalid = UINT64_MAX;
    memcpy(memory + 0x1100, &invalid, 8);
    errors();
    SYSTEMTIME st;
    CHECK(!FileTimeToSystemTime((FILETIME *)&invalid, &st));
    DWORD err = GetLastError();
    CHECK(!K("FileTimeToSystemTime", 2, 0x1100u, 0x1200u));
    CHECK(last_error == err);
    SYSTEMTIME input = {2024, 6, 0, 15, 13, 4, 5, 100};
    FILETIME f, local, back;
    CHECK(SystemTimeToFileTime(&input, &f));
    memcpy(memory + 0x1100, &f, 8);
    CHECK(FileTimeToLocalFileTime(&f, &local));
    CHECK(K("FileTimeToLocalFileTime", 2, 0x1100u, 0x1200u));
    CHECK(!memcmp(&local, memory + 0x1200, 8));
    CHECK(LocalFileTimeToFileTime(&local, &back));
    CHECK(K("LocalFileTimeToFileTime", 2, 0x1200u, 0x1300u));
    CHECK(!memcmp(&back, memory + 0x1300, 8));
    TIME_ZONE_INFORMATION tz;
    DWORD zone = GetTimeZoneInformation(&tz);
    CHECK(K("GetTimeZoneInformation", 1, 0x1400u) == zone);
    CHECK(!memcmp(&tz, memory + 0x1400, 172));
    K("GetLocalTime", 1, 0x1000u);
    GetLocalTime(&st);
    CHECK(image_u16(memory + 0x1000) == st.wYear);
    K("GetSystemTime", 1, 0x1000u);
    GetSystemTime(&st);
    CHECK(image_u16(memory + 0x1000) == st.wYear);
    memcpy(memory + 0x1000, &input, 16);
    char text[256];
    int count = GetDateFormatA(0x409, DATE_SHORTDATE, &input, NULL, text, 256);
    CHECK(K("GetDateFormatA", 6, 0x409u, DATE_SHORTDATE, 0x1000u, 0u, 0x1500u, 256u) ==
          (uint32_t)count);
    CHECK(!strcmp(text, (char *)memory + 0x1500));
    count = GetTimeFormatA(0x409, 0, &input, NULL, text, 256);
    CHECK(K("GetTimeFormatA", 6, 0x409u, 0u, 0x1000u, 0u, 0x1500u, 256u) == (uint32_t)count);
    CHECK(!strcmp(text, (char *)memory + 0x1500));
    CHECK(K("GetProcessTimes", 5, UINT32_MAX, 0x1000u, 0x1100u, 0x1200u, 0x1300u));
    CHECK(wc_read64(0x1000) > 0);
}
static void test_locale(void)
{
    CHECK(K("GetUserDefaultLCID", 0) == 0x409);
    CHECK(K("GetOEMCP", 0) == GetOEMCP());
    CHECK(K("GetConsoleCP", 0) == GetConsoleCP());
    CHECK(K("GetConsoleOutputCP", 0) == GetConsoleOutputCP());
    CHECK(K("IsValidLocale", 2, 0x409u, LCID_INSTALLED) ==
          (uint32_t)IsValidLocale(0x409, LCID_INSTALLED));
    CHECK(K("EnumSystemLocalesA", 2, 0x123456u, LCID_INSTALLED));
    CHECK(callback_count == 1);
    uint32_t a = put("aBc"), b = put("AbC");
    for (unsigned flags = 0; flags <= 1; ++flags) {
        errors();
        int n = CompareStringA(0x409, flags, (char *)memory + a, -1, (char *)memory + b, -1);
        DWORD err = GetLastError();
        CHECK(K("CompareStringA", 6, 0x409u, flags, a, UINT32_MAX, b, UINT32_MAX) == (uint32_t)n);
        if (!n)
            CHECK(last_error == err);
    }
    WCHAR wa[] = {L'A', 0x0100, L'b', 0}, wb[] = {L'a', 0x0100, L'B', 0};
    memcpy(memory + 0x1000, wa, sizeof(wa));
    memcpy(memory + 0x1100, wb, sizeof(wb));
    CHECK(K("CompareStringW", 6, 0x409u, 1u, 0x1000u, UINT32_MAX, 0x1100u, UINT32_MAX) ==
          (uint32_t)CompareStringW(0x409, 1, wa, -1, wb, -1));
    char mapped[32];
    int len = LCMapStringA(0x409, LCMAP_UPPERCASE, "aBc", -1, mapped, 32);
    CHECK(K("LCMapStringA", 6, 0x409u, LCMAP_UPPERCASE, a, UINT32_MAX, 0x1200u, 32u) ==
          (uint32_t)len);
    CHECK(!memcmp(mapped, memory + 0x1200, len));
    WORD types[4];
    CHECK(GetStringTypeA(0x409, CT_CTYPE1, "aBc", -1, types));
    CHECK(K("GetStringTypeA", 5, 0x409u, CT_CTYPE1, a, UINT32_MAX, 0x1200u));
    CHECK(!memcmp(types, memory + 0x1200, sizeof(types)));
    unsigned queries[] = {LOCALE_SDECIMAL, LOCALE_SSHORTDATE, LOCALE_IDEFAULTANSICODEPAGE,
                          LOCALE_IDATE | LOCALE_RETURN_NUMBER};
    for (unsigned j = 0; j < 4; ++j)
        for (unsigned wide = 0; wide < 2; ++wide) {
            unsigned char data[256] = {0};
            errors();
            int n = wide ? GetLocaleInfoW(0x409, queries[j], (WCHAR *)data, 128)
                         : GetLocaleInfoA(0x409, queries[j], (char *)data, 128);
            DWORD err = GetLastError();
            CHECK(K(wide ? "GetLocaleInfoW" : "GetLocaleInfoA", 4, 0x409u, queries[j], 0x1200u,
                    128u) == (uint32_t)n);
            CHECK(!memcmp(data, memory + 0x1200, n * (wide ? 2 : 1)));
            if (!n)
                CHECK(last_error == err);
            CHECK(K(wide ? "GetLocaleInfoW" : "GetLocaleInfoA", 4, 0x409u, queries[j], 0u, 0u) ==
                  (uint32_t)n);
            if (n > 1) {
                errors();
                CHECK(!K(wide ? "GetLocaleInfoW" : "GetLocaleInfoA", 4, 0x409u, queries[j], 0x1200u,
                         1u));
                CHECK(last_error == 122);
            }
        }
}

static void test_files(const char *root)
{
    char path[32768], copy[32768], moved[32768], asset[32768], temp[32768];
    snprintf(path, sizeof(path), "%s/test.dat", userdata);
    snprintf(copy, sizeof(copy), "%s/copy.dat", userdata);
    snprintf(moved, sizeof(moved), "%s/moved.dat", userdata);
    snprintf(asset, sizeof(asset), "%s/asset.dat", root);
    HANDLE original =
        CreateFileA(asset, GENERIC_WRITE, 7, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(original != INVALID_HANDLE_VALUE);
    DWORD wrote;
    CHECK(WriteFile(original, "original", 8, &wrote, NULL));
    CloseHandle(original);
    uint32_t pathp = put(path), copyp = put(copy), movedp = put(moved), assetp = put(asset);
    errors();
    HANDLE native = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 7, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(native != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(native, "abcdef", 6, &wrote, NULL));
    uint32_t file = K("CreateFileA", 7, pathp, GENERIC_READ | GENERIC_WRITE, 7u, 0u, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, 0u);
    CHECK(file != UINT32_MAX);
    CHECK(K("GetFileSize", 2, file, 0x1000u) == GetFileSize(native, NULL));
    CHECK(rd32(0x1000) == 0);
    errors();
    FILETIME times[3];
    CHECK(GetFileTime(native, &times[0], &times[1], &times[2]));
    CHECK(K("GetFileTime", 4, file, 0x1000u, 0x1100u, 0x1200u));
    CHECK(last_error == SENTINEL);
    CHECK(!memcmp(&times[0], memory + 0x1000, 8));
    CHECK(!memcmp(&times[1], memory + 0x1100, 8));
    CHECK(!memcmp(&times[2], memory + 0x1200, 8));
    SYSTEMTIME fixed = {2020, 1, 0, 2, 3, 4, 5, 0};
    FILETIME ft;
    CHECK(SystemTimeToFileTime(&fixed, &ft));
    memcpy(memory + 0x1300, &ft, 8);
    CHECK(K("SetFileTime", 4, file, 0u, 0u, 0x1300u));
    CHECK(GetFileTime(native, NULL, NULL, &ft));
    CHECK(!memcmp(&ft, memory + 0x1300, 8));
    uint32_t txt = put("XYZ");
    CHECK(K("WriteFile", 5, file, txt, 3u, 0x1400u, 0u));
    CHECK(rd32(0x1400) == 3);
    CHECK(K("SetEndOfFile", 1, file));
    CHECK(GetFileSize(native, NULL) == 3);
    CHECK(K("FlushFileBuffers", 1, file));
    CHECK(LockFile(native, 0, 0, 2, 0));
    CHECK(K("LockFile", 5, file, 0u, 0u, 2u, 0u));
    CHECK(!LockFile(native, 0, 0, 2, 0));
    CHECK(GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(!K("LockFile", 5, file, 0u, 0u, 2u, 0u));
    CHECK(last_error == ERROR_LOCK_VIOLATION);
    CHECK(UnlockFile(native, 0, 0, 2, 0));
    CHECK(K("UnlockFile", 5, file, 0u, 0u, 2u, 0u));
    CHECK(!K("UnlockFile", 5, file, 0u, 0u, 2u, 0u));
    CHECK(last_error == ERROR_NOT_LOCKED);
    memset(memory + 0x1500, 0, 20);
    CHECK(K("LockFileEx", 6, file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0u, 1u, 0u,
            0x1500u));
    CHECK(K("UnlockFile", 5, file, 0u, 0u, 1u, 0u));
    uint32_t err;
    CHECK(ac1_files_close(guest_files, file, &err));
    CloseHandle(native);
    errors();
    DWORD attrs = GetFileAttributesA(path);
    CHECK(K("GetFileAttributesA", 1, pathp) == attrs);
    CHECK(last_error == SENTINEL);
    CHECK(K("SetFileAttributesA", 2, pathp, FILE_ATTRIBUTE_READONLY));
    CHECK(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY);
    CHECK(K("SetFileAttributesA", 2, pathp, FILE_ATTRIBUTE_NORMAL));
    CHECK(K("CopyFileA", 3, pathp, copyp, 1u));
    CHECK(!K("CopyFileA", 3, pathp, copyp, 1u));
    CHECK(last_error == ERROR_FILE_EXISTS);
    CHECK(K("MoveFileA", 2, copyp, movedp));
    CHECK(GetFileAttributesA(copy) == INVALID_FILE_ATTRIBUTES);
    CHECK(K("DeleteFileA", 1, movedp));
    CHECK(K("DeleteFileA", 1, pathp));
    snprintf(path, sizeof(path), "%s/child", userdata);
    pathp = put(path);
    CHECK(K("CreateDirectoryA", 2, pathp, 0u));
    CHECK(!K("CreateDirectoryA", 2, pathp, 0u));
    CHECK(last_error == 183);
    CHECK(K("RemoveDirectoryA", 1, pathp));
    CHECK(K("GetFullPathNameA", 4, put("asset.dat"), 0u, 0u, 0u) == strlen(asset) + 1);
    uint32_t got = K("GetFullPathNameA", 4, put("asset.dat"), 1024u, 0x1800u, 0x1400u);
    char expected[32768];
    char *part = NULL;
    DWORD len = GetFullPathNameA(asset, sizeof(expected), expected, &part);
    CHECK(got == len);
    CHECK(!strcmp(expected, (char *)memory + 0x1800));
    CHECK(rd32(0x1400) == 0x1800u + (uint32_t)(part - expected));
    uint32_t needed = K("GetTempPathA", 2, 0u, 0u);
    CHECK(needed > 1);
    CHECK(K("GetTempPathA", 2, 1024u, 0x1800u) == needed - 1);
    strcpy(temp, (char *)memory + 0x1800);
    CHECK(strstr(temp, userdata) == temp);
    CHECK(GetFileAttributesA(temp) & FILE_ATTRIBUTE_DIRECTORY);
    CHECK(K("GetDriveTypeA", 1, 0u) == DRIVE_FIXED);
    /* Every install mutation fails, including destination of copy/move. */
    CHECK(!K("SetFileAttributesA", 2, assetp, FILE_ATTRIBUTE_NORMAL));
    CHECK(last_error == 5);
    CHECK(!K("DeleteFileA", 1, assetp));
    CHECK(last_error == 5);
    CHECK(!K("MoveFileA", 2, assetp, put(moved)));
    CHECK(last_error == 5);
    CHECK(!K("CopyFileA", 3, assetp, assetp, 0u));
    CHECK(last_error == 5);
    CHECK(!K("CreateDirectoryA", 2, put("newdir"), 0u));
    CHECK(last_error == 5);
    CHECK(!K("RemoveDirectoryA", 1, put("newdir")));
    CHECK(last_error == 5);
    CHECK(!K("SetEndOfFile", 1, 0xdeadbeefu));
    CHECK(last_error == 6);
    CHECK(ac1_files_open(guest_files, "asset.dat", GENERIC_READ, 7, 0, OPEN_EXISTING, 0x10000000, 0,
                         &file, &err) == 1 &&
          !err);
    CHECK(!K("SetEndOfFile", 1, file));
    CHECK(last_error == 5);
    CHECK(!K("SetFileTime", 4, file, 0u, 0u, 0x1300u));
    CHECK(last_error == 5);
    CHECK(!K("WriteFile", 5, file, txt, 3u, 0x1400u, 0u));
    CHECK(last_error == 5);
    CHECK(ac1_files_close(guest_files, file, &err));
    char unsafe[32768];
    snprintf(unsafe, sizeof(unsafe), "%s/../asset.dat", userdata);
    CHECK(!K("DeleteFileA", 1, put(unsafe)));
    CHECK(last_error == 5);
    snprintf(unsafe, sizeof(unsafe), "%s/hard.dat", userdata);
    CHECK(CreateHardLinkA(unsafe, asset, NULL));
    CHECK(!K("SetFileAttributesA", 2, put(unsafe), FILE_ATTRIBUTE_NORMAL));
    CHECK(last_error == 5);
    CHECK(DeleteFileA(unsafe));
    snprintf(unsafe, sizeof(unsafe), "%s/link.dat", userdata);
    if (CreateSymbolicLinkA(unsafe, asset, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        CHECK(!K("SetFileAttributesA", 2, put(unsafe), FILE_ATTRIBUTE_NORMAL));
        CHECK(last_error == 5);
        CHECK(DeleteFileA(unsafe));
        puts("PASS: userdata symlink rejection");
    } else {
        CHECK(GetLastError() == ERROR_PRIVILEGE_NOT_HELD ||
              GetLastError() == ERROR_INVALID_PARAMETER);
        puts("SKIP: symlink creation unavailable; hard-link rejection verified");
    }
    CHECK(GetFileAttributesA(asset) != INVALID_FILE_ATTRIBUTES);
    char ancestor[32768], resolved[32768];
    strcpy(ancestor, userdata);
    char *last = strrchr(ancestor, '/');
    CHECK(last != NULL);
    *last = 0;
    CHECK(!ac1_files_resolve(guest_files, ancestor, asset, 1, 0, resolved, sizeof(resolved), &err));
    CHECK(err == 5);
    char trailing[32768];
    snprintf(trailing, sizeof(trailing), "%s/", userdata);
    CHECK(
        ac1_files_resolve(guest_files, trailing, userdata, 1, 0, resolved, sizeof(resolved), &err));
    /* Wildcards: compare unordered results, layout and truncation errors. */
    const char *names[] = {"Alpha.txt", "beta.TXT", "plain", "z.bin"};
    for (unsigned j = 0; j < 4; ++j) {
        snprintf(path, sizeof(path), "%s/%s", userdata, names[j]);
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        fputs("test", f);
        fclose(f);
    }
    const char *patterns[] = {"*", "*.*", "*.txt", "a????.txt", "missing*", "?lain"};
    for (unsigned j = 0; j < 6; ++j) {
        snprintf(path, sizeof(path), "%s/%s", userdata, patterns[j]);
        uint32_t pp = put(path);
        WIN32_FIND_DATAA nd;
        HANDLE find = FindFirstFileA(path, &nd);
        DWORD native_error = GetLastError();
        errors();
        uint32_t gf = K("FindFirstFileA", 2, pp, 0x1800u);
        if (find == INVALID_HANDLE_VALUE) {
            CHECK(gf == UINT32_MAX);
            CHECK(last_error == native_error);
            continue;
        }
        CHECK(gf != UINT32_MAX);
        unsigned native_mask = 0, guest_mask = 0, native_count = 0, guest_count = 0;
        do {
            ++native_count;
            for (unsigned k = 0; k < 4; ++k)
                if (!_stricmp(nd.cFileName, names[k]))
                    native_mask |= 1u << k;
        } while (FindNextFileA(find, &nd));
        FindClose(find);
        do {
            WIN32_FIND_DATAA *gd = (void *)(memory + 0x1800);
            ++guest_count;
            for (unsigned k = 0; k < 4; ++k)
                if (!_stricmp(gd->cFileName, names[k])) {
                    guest_mask |= 1u << k;
                    CHECK(gd->nFileSizeLow == 4);
                    CHECK(!gd->nFileSizeHigh);
                }
        } while (K("FindNextFileA", 2, gf, 0x1800u));
        CHECK(last_error == 18);
        CHECK(native_mask == guest_mask);
        CHECK(native_count == guest_count);
        CHECK(K("FindClose", 1, gf));
        CHECK(!K("FindClose", 1, gf));
        CHECK(last_error == 6);
    }
    snprintf(path, sizeof(path), "%s/settings.ini", userdata);
    FILE *ini = fopen(path, "wb");
    CHECK(ini != NULL);
    fputs("[Section]\r\nOne=1\r\nTwo=two\r\n", ini);
    fclose(ini);
    char block[128] = {0};
    DWORD n = GetPrivateProfileSectionA("Section", block, sizeof(block), path);
    CHECK(K("GetPrivateProfileSectionA", 4, put("Section"), 0x1800u, 128u, put(path)) == n);
    CHECK(!memcmp(block, memory + 0x1800, n + 1));
    CHECK(DeleteFileA(path));
    for (unsigned j = 0; j < 4; ++j) {
        snprintf(path, sizeof(path), "%s/%s", userdata, names[j]);
        CHECK(DeleteFileA(path));
    }
    CHECK(DeleteFileA(asset));
}
static void test_misc(void)
{
    CHECK(!K("IsDebuggerPresent", 0));
    K("OutputDebugStringA", 1, put("oracle debug text"));
    CHECK(K("SetErrorMode", 1, 3u) == 0);
    CHECK(K("SetErrorMode", 1, 0u) == 3);
    errors();
    CHECK(K("LoadLibraryA", 1, put("kernel32.dll")) == AC1_KERNEL32_HANDLE);
    CHECK(last_error == SENTINEL);
    CHECK(K("FreeLibrary", 1, AC1_KERNEL32_HANDLE));
    CHECK(!K("LoadLibraryA", 1, put("definitely_missing.dll")));
    CHECK(last_error == 126);
    CHECK(!K("FreeLibrary", 1, 0u));
    CHECK(last_error == 6);
    unsigned codes[] = {0, 2, 3, 5, 6, 8, 18, 87, 122, 126, 183, 1167, 0xffffffff};
    for (unsigned j = 0; j < sizeof(codes) / sizeof(codes[0]); ++j) {
        char native[4096];
        errors();
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                 codes[j], 0x409, native, sizeof(native), NULL);
        DWORD err = GetLastError();
        CHECK(K("FormatMessageA", 7, FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0u,
                codes[j], 0x409u, 0x1000u, 4096u, 0u) == n);
        CHECK(last_error == err);
        if (n) {
            CHECK(!memcmp(native, memory + 0x1000, n + 1));
            CHECK(K("FormatMessageA", 7,
                    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                        FORMAT_MESSAGE_ALLOCATE_BUFFER,
                    0u, codes[j], 0x409u, 0x1100u, 0u, 0u) == n);
            uint32_t p = rd32(0x1100);
            CHECK(!memcmp(native, memory + p, n + 1));
            CHECK(!K("LocalFree", 1, p));
            errors();
            CHECK(!K("FormatMessageA", 7,
                     FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0u, codes[j],
                     0x409u, 0x1000u, 1u, 0u));
            CHECK(last_error == 122);
        }
    }
    CHECK(!K("LocalFree", 1, 0u));
    char computer[1024];
    DWORD computer_size = sizeof(computer);
    CHECK(GetComputerNameExA(ComputerNameNetBIOS, computer, &computer_size));
    wr32(0x1000, 1);
    CHECK(!K("GetComputerNameExA", 3, 0u, 0x1100u, 0x1000u));
    CHECK(rd32(0x1000) == computer_size + 1);
    CHECK(last_error == 234);
    wr32(0x1000, 1024);
    CHECK(K("GetComputerNameExA", 3, 0u, 0x1100u, 0x1000u));
    CHECK(!strcmp((char *)memory + 0x1100, computer));
    CHECK(rd32(0x1000) == computer_size);
    wr32(0x1000, 2);
    CHECK(!D("ADVAPI32.dll", "GetUserNameA", 2, 0x1100u, 0x1000u));
    CHECK(rd32(0x1000) == 7);
    CHECK(last_error == 122);
    wr32(0x1000, 32);
    CHECK(D("ADVAPI32.dll", "GetUserNameA", 2, 0x1100u, 0x1000u));
    CHECK(!strcmp((char *)memory + 0x1100, "Player"));
    CHECK(D("ADVAPI32.dll", "RegQueryValueExA", 6, 0u, 0u, 0u, 0u, 0u, 0u) == ERROR_INVALID_HANDLE);
    CHECK(D("ADVAPI32.dll", "RegCloseKey", 1, 0u) == ERROR_INVALID_HANDLE);
    CHECK(D("ADVAPI32.dll", "RegCloseKey", 1, 0x80000002u) == ERROR_SUCCESS);
    uint32_t key = put("AC1_COMPAT_FIXTURE_ENV"), value = put("private");
    CHECK(K("SetEnvironmentVariableA", 2, key, value));
    CHECK(!getenv("AC1_COMPAT_FIXTURE_ENV"));
    CHECK(K("GetEnvironmentVariableA", 3, key, 0x1000u, 128u) == 7);
    CHECK(!strcmp((char *)memory + 0x1000, "private"));
    uint32_t env = K("GetEnvironmentStrings", 0);
    CHECK(env);
    CHECK(!strcmp((char *)memory + env, "AC1_COMPAT_FIXTURE_ENV=private"));
    CHECK(memory[env + strlen("AC1_COMPAT_FIXTURE_ENV=private") + 1] == 0);
    CHECK(K("FreeEnvironmentStringsA", 1, env));
    CHECK(K("SetEnvironmentVariableA", 2, key, 0u));
    env = K("GetEnvironmentStrings", 0);
    CHECK(!memory[env] && !memory[env + 1]);
    CHECK(K("FreeEnvironmentStringsA", 1, env));
    CHECK(K("SetConsoleCtrlHandler", 2, 0u, 1u));
    CHECK(K("SetConsoleCtrlHandler", 2, 0x123456u, 1u));
    CHECK(K("SetConsoleCtrlHandler", 2, 0x123456u, 0u));
    CHECK(K("SetStdHandle", 2, (uint32_t)-11, AC1_STD_ERROR_HANDLE));
    CHECK(K("GetStdHandle", 1, (uint32_t)-11) == AC1_STD_ERROR_HANDLE);
    CHECK(K("SetStdHandle", 2, (uint32_t)-11, AC1_STD_OUTPUT_HANDLE));
    CHECK(K("WriteConsoleA", 5, AC1_STD_ERROR_HANDLE, put("console A\n"), 10u, 0x1000u, 0u));
    CHECK(rd32(0x1000) == 10);
    WCHAR wide[] = {L'W', L'\n', 0};
    memcpy(memory + 0x1100, wide, sizeof(wide));
    CHECK(K("WriteConsoleW", 5, AC1_STD_ERROR_HANDLE, 0x1100u, 2u, 0x1000u, 0u));
    CHECK(rd32(0x1000) == 2);
    CHECK(!K("WriteConsoleA", 5, 1u, 0u, 0u, 0x1000u, 0u));
    CHECK(last_error == 6);
    scheduler.count = 2;
    scheduler.threads[0].handle = AC1_THREAD_HANDLE_BASE;
    scheduler.threads[0].id = 1;
    scheduler.threads[1].handle = AC1_THREAD_HANDLE_BASE + 4;
    scheduler.threads[1].id = 2;
    scheduler.threads[1].blocked = 1;
    scheduler.threads[1].wait_kind = 1;
    scheduler.threads[1].wait_handle = fixture_event.handle;
    fixture_event.host_event = CreateEventA(NULL, TRUE, TRUE, NULL);
    CHECK(fixture_event.host_event != NULL);
    HANDLE oracle_event = CreateEventA(NULL, TRUE, TRUE, NULL);
    CHECK(oracle_event != NULL);
    CHECK(PulseEvent(oracle_event));
    CHECK(WaitForSingleObject(oracle_event, 0) == WAIT_TIMEOUT);
    CHECK(K("PulseEvent", 1, fixture_event.handle));
    CHECK(WaitForSingleObject(fixture_event.host_event, 0) == WAIT_TIMEOUT);
    CloseHandle(oracle_event);
    CloseHandle(fixture_event.host_event);
    fixture_event.host_event = NULL;
    CHECK(!scheduler.threads[1].blocked);
    CHECK(!scheduler.threads[1].wait_kind);
    CHECK(!K("PulseEvent", 1, 0u));
    CHECK(last_error == 6);
    HANDLE oracle_thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    CHECK(oracle_thread != NULL);
    HANDLE oracle_duplicate = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), oracle_thread, GetCurrentProcess(),
                          &oracle_duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(CloseHandle(oracle_thread));
    CHECK(GetThreadId(oracle_duplicate) == GetCurrentThreadId());
    CHECK(CloseHandle(oracle_duplicate));
    uint32_t alias = K("OpenThread", 3, THREAD_QUERY_INFORMATION, 0u, 2u);
    CHECK(alias && alias != AC1_THREAD_HANDLE_BASE + 4);
    CHECK(K("GetThreadId", 1, alias) == 2);
    CHECK(K("DuplicateHandle", 7, UINT32_MAX, alias, UINT32_MAX, 0x1000u, 0u, 0u,
            DUPLICATE_SAME_ACCESS));
    uint32_t alias2 = rd32(0x1000);
    CHECK(alias2 != alias);
    CHECK(K("CloseHandle", 1, alias));
    CHECK(K("GetThreadId", 1, alias2) == 2);
    CHECK(K("CloseHandle", 1, AC1_THREAD_HANDLE_BASE + 4));
    CHECK(K("GetThreadId", 1, alias2) == 2);
    CHECK(K("CloseHandle", 1, alias2));
    CHECK(scheduler.threads[1].closed);
    CHECK(K("GetThreadId", 1, alias2) == 0);
    CHECK(last_error == 6);
    CHECK(D("OLEAUT32.dll", "SysAllocString", 1, 0u) == 0);
    memcpy(memory + 0x1100, wide, sizeof(wide));
    uint32_t bstr = D("OLEAUT32.dll", "SysAllocString", 1, 0x1100u);
    CHECK(bstr && rd32(bstr - 4) == 4);
    CHECK(!memcmp(memory + bstr, wide, sizeof(wide)));
    D("OLEAUT32.dll", "SysFreeString", 1, bstr);
}

static void test_user(void)
{
    CHECK(U("MessageBoxA", 4, 0u, put("fixture message"), put("fixture caption"), 0u) == IDOK);
    BYTE state[256];
    CHECK(GetKeyboardState(state));
    CHECK(U("GetKeyboardState", 1, 0x1000u));
    CHECK(!memcmp(state, memory + 0x1000, 256));
    memset(state, 0, sizeof(state));
    memcpy(memory + 0x1000, state, 256);
    WCHAR text[8] = {0};
    int n = ToUnicodeEx('A', MapVirtualKeyA('A', MAPVK_VK_TO_VSC), state, text, 8, 4,
                        GetKeyboardLayout(0));
    CHECK(U("ToUnicode", 6, (uint32_t)'A', MapVirtualKeyA('A', MAPVK_VK_TO_VSC), 0x1000u, 0x1200u,
            8u, 4u) == (uint32_t)n);
    if (n > 0)
        CHECK(!memcmp(text, memory + 0x1200, n * 2));
    STICKYKEYS before = {sizeof(before), 0}, after = {sizeof(after), 0};
    CHECK(SystemParametersInfoA(SPI_GETSTICKYKEYS, sizeof(before), &before, 0));
    memcpy(memory + 0x1000, &before, sizeof(before));
    CHECK(U("SystemParametersInfoA", 4, SPI_GETSTICKYKEYS, (uint32_t)sizeof(before), 0x1000u, 0u));
    CHECK(!memcmp(&before, memory + 0x1000, sizeof(before)));
    wr32(0x1004, before.dwFlags ^ SKF_STICKYKEYSON);
    CHECK(U("SystemParametersInfoA", 4, SPI_SETSTICKYKEYS, (uint32_t)sizeof(before), 0x1000u,
            SPIF_SENDCHANGE));
    CHECK(U("SystemParametersInfoA", 4, SPI_SETTOGGLEKEYS, 0u, 0u, SPIF_SENDCHANGE));
    CHECK(U("SystemParametersInfoA", 4, SPI_SETFILTERKEYS, 0u, 0u, SPIF_SENDCHANGE));
    CHECK(SystemParametersInfoA(SPI_GETSTICKYKEYS, sizeof(after), &after, 0));
    CHECK(!memcmp(&before, &after, sizeof(before)));
    RECT rect;
    CHECK(SystemParametersInfoA(SPI_GETWORKAREA, 0, &rect, 0));
    CHECK(U("SystemParametersInfoA", 4, SPI_GETWORKAREA, 0u, 0x1000u, 0u));
    CHECK(!memcmp(&rect, memory + 0x1000, 16));
    CHECK(U("CallNextHookEx", 4, 0u, 0u, 0u, 0u) == 0);
    CHECK(!U("UnhookWindowsHookEx", 1, 0u));
    CHECK(last_error == ERROR_INVALID_HOOK_HANDLE);
    /* Native linked RT_STRING block copied into a synthetic mapped PE tree. */
    HMODULE self = GetModuleHandleA(NULL);
    HRSRC resource = FindResourceA(self, MAKEINTRESOURCEA(2), RT_STRING);
    CHECK(resource != NULL);
    DWORD bytes = SizeofResource(self, resource);
    void *block = LockResource(LoadResource(self, resource));
    CHECK(block && bytes < 2048);
    unsigned char *pe = memory + image_base;
    memset(pe, 0, 0x10000);
    wr16(image_base, 0x5a4d);
    wr32(image_base + 60, 0x80);
    wr32(image_base + 0x80, 0x4550);
    wr32(image_base + 0x80 + 136, 0x1000);
    wr32(image_base + 0x80 + 140, 0x1000);
    uint32_t r = image_base + 0x1000;
    wr16(r + 14, 1);
    wr32(r + 16, 6);
    wr32(r + 20, 0x80000020);
    wr16(r + 0x20 + 14, 1);
    wr32(r + 0x30, 2);
    wr32(r + 0x34, 0x80000040);
    wr16(r + 0x40 + 14, 1);
    wr32(r + 0x50, 1033);
    wr32(r + 0x54, 0x60);
    wr32(r + 0x60, 0x1200);
    wr32(r + 0x64, bytes);
    memcpy(pe + 0x1200, block, bytes);
    for (unsigned id = 17; id <= 20; ++id)
        for (unsigned cap = 1; cap <= 64; cap *= 2) {
            char native[128] = {0};
            errors();
            int got = LoadStringA(self, id, native, cap);
            CHECK(U("LoadStringA", 4, image_base, id, 0x1800u, cap) == (uint32_t)got);
            CHECK(!strcmp(native, (char *)memory + 0x1800));
        }
    WAVEOUTCAPSA caps;
    MMRESULT result = waveOutGetDevCapsA((UINT_PTR)-1, &caps, sizeof(caps));
    CHECK(D("WINMM.dll", "waveOutGetDevCapsA", 3, UINT32_MAX, 0x1800u, (uint32_t)sizeof(caps)) ==
          result);
    if (result == 0)
        CHECK(!memcmp(&caps, memory + 0x1800, sizeof(caps)));
    HMODULE xi = LoadLibraryExA("xinput1_3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(xi != NULL);
    for (unsigned ordinal = 2; ordinal <= 4; ++ordinal) {
        FARPROC raw = GetProcAddress(xi, (LPCSTR)(uintptr_t)ordinal);
        CHECK(raw != NULL);
        unsigned char data[32] = {0};
        DWORD expected;
        if (ordinal == 4) {
            typedef DWORD(WINAPI * fn_t)(DWORD, DWORD, void *);
            fn_t fn;
            memcpy(&fn, &raw, sizeof(fn));
            expected = fn(10, 0, data);
        } else {
            typedef DWORD(WINAPI * fn_t)(DWORD, void *);
            fn_t fn;
            memcpy(&fn, &raw, sizeof(fn));
            expected = fn(10, data);
        }
        char name[8];
        snprintf(name, sizeof(name), "#%u", ordinal);
        CHECK((ordinal == 4 ? D("XINPUT1_3.dll", name, 3, 10u, 0u, 0x1800u)
                            : D("XINPUT1_3.dll", name, 2, 10u, 0x1800u)) == expected);
    }
    FreeLibrary(xi);
    CHECK(D("WS2_32.dll", "htons", 1, 0x12345678u) == 0x7856);
    CHECK(D("WS2_32.dll", "ntohs", 1, 0x12345678u) == 0x7856);
    CHECK(D("WS2_32.dll", "htonl", 1, 0x12345678u) == 0x78563412);
    CHECK(D("WS2_32.dll", "ntohl", 1, 0x12345678u) == 0x78563412);
    wr32(0x1000, 2);
    wr32(0x1004, 4);
    wr32(0x1008, 8);
    CHECK(D("WS2_32.dll", "__WSAFDIsSet", 2, 8u, 0x1000u) == 1);
    CHECK(!D("WS2_32.dll", "__WSAFDIsSet", 2, 7u, 0x1000u));
}
static void test_traps(void)
{
    const char *names[] = {"RtlUnwind",   "UnhandledExceptionFilter", "DebugBreak",
                           "ExitProcess", "TerminateProcess",         "FatalAppExitA"};
    for (unsigned j = 0; j < 6; ++j) {
        trap_reason[0] = 0;
        if (!setjmp(trap)) {
            if (j == 4)
                K(names[j], 2, UINT32_MAX, 42u);
            else if (j == 5)
                K(names[j], 2, 0u, put("fatal fixture"));
            else
                K(names[j], 4, 42u, 0u, 0u, 0u);
            CHECK(0);
        }
        CHECK(strstr(trap_reason, j < 3 ? "SEH" : "guest process exit") != NULL);
    }
    if (!setjmp(trap)) {
        D("WS2_32.dll", "socket", 3, 2u, 1u, 0u);
        CHECK(0);
    }
    CHECK(strstr(trap_reason, "fallback has not been verified"));
    if (!setjmp(trap)) {
        D("WININET.dll", "InternetGetCookieW", 4, 0u, 0u, 0u, 0u);
        CHECK(0);
    }
    CHECK(strstr(trap_reason, "offline WININET"));
}
int main(void)
{
    if (setjmp(trap)) {
        fprintf(stderr, "unexpected trap: %s\n", trap_reason);
        return 1;
    }
    char base[32768], root[32768];
    CHECK(GetFullPathNameA("work/win32_compat", sizeof(base), base, NULL));
    CHECK(ensure_directory_tree(base));
    char unique[MAX_PATH];
    CHECK(GetTempFileNameA(base, "api", 0, unique));
    CHECK(DeleteFileA(unique));
    CHECK(CreateDirectoryA(unique, NULL));
    snprintf(root, sizeof(root), "%s/assets", unique);
    snprintf(userdata, sizeof(userdata), "%s/userdata", unique);
    CHECK(CreateDirectoryA(root, NULL));
    CHECK(CreateDirectoryA(userdata, NULL));
    strcpy(guest_current_directory, root);
    snprintf(exe_path_copy, sizeof(exe_path_copy), "%s/AssassinsCreed_Dx9.exe", root);
    guest_files = ac1_files_create(root);
    CHECK(guest_files != NULL);
    test_time();
    test_locale();
    test_files(root);
    test_misc();
    test_user();
    test_traps();
    ac1_files_destroy(guest_files);
    guest_files = NULL;
    CHECK(RemoveDirectoryA(root));
    char temp[32768];
    snprintf(temp, sizeof(temp), "%s/temp", userdata);
    CHECK(RemoveDirectoryA(temp));
    CHECK(RemoveDirectoryA(userdata));
    CHECK(RemoveDirectoryA(unique));
    printf("PASS: %u adapter calls, %u assertions; native oracles, userdata policy, resources, "
           "settings, explicit traps\n",
           calls, checks);
    return 0;
}
