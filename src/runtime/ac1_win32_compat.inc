/* AC1 Win32 guest adapter. The en-US locale table, guest-local environment
 * and FILETIME epoch approach adapt recomp-kit runtime/kernel32.cpp
 * (copyright 2026 veritr1x, MIT; LICENSE.recomp-kit.txt). Host pointers and
 * host module handles never enter guest memory. This file is included once. */
#include <limits.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/time.h>
#include <wctype.h>
#else
#include <mmsystem.h>
#include <xinput.h>
#endif

#define WC_FIND_BASE 0xe7000000u
#define WC_ALIAS_BASE 0xe7100000u
#define WC_EPOCH UINT64_C(116444736000000000)
#define WC_TICKS UINT64_C(10000000)

typedef struct {
#ifdef _WIN32
    HANDLE native;
#else
    DIR *native;
#endif
    int used;
    char directory[32768], pattern[260];
} wc_find;
typedef struct {
    char *name, *value;
} wc_env;
typedef struct {
    wc_find find[32];
    wc_env env[128];
    uint32_t error_mode, standard[3], aliases[128], console_handlers[32];
    uint8_t closed_threads[AC1_MAX_THREADS];
    uint64_t creation_time;
    int ignore_ctrl_c;
    struct {
        uint32_t handle;
        uint64_t offset, length;
    } locks[128];
    int settings_logged;
} wc_state;

static void wc_destroy(void *pointer)
{
    wc_state *s = pointer;
    if (!s)
        return;
    for (unsigned i = 0; i < 32; ++i)
        if (s->find[i].used) {
#ifdef _WIN32
            FindClose(s->find[i].native);
#else
            closedir(s->find[i].native);
#endif
        }
    for (unsigned i = 0; i < 128; ++i) {
        free(s->env[i].name);
        free(s->env[i].value);
    }
    free(s);
}
static wc_state *wc_get_state(void)
{
    void **p = ac1_files_compat_state(guest_files, wc_destroy);
    if (!p)
        return NULL;
    if (!*p) {
        wc_state *s = calloc(1, sizeof(*s));
        if (!s)
            return NULL;
#ifndef _WIN32
        struct timespec real;
        clock_gettime(CLOCK_REALTIME, &real);
        s->creation_time = WC_EPOCH + (uint64_t)real.tv_sec * WC_TICKS + real.tv_nsec / 100;
#endif
        s->standard[0] = AC1_STD_INPUT_HANDLE;
        s->standard[1] = AC1_STD_OUTPUT_HANDLE;
        s->standard[2] = AC1_STD_ERROR_HANDLE;
        *p = s;
    }
    return *p;
}
static uint32_t wc_arg(CPU *cpu, unsigned n)
{
    return rd32(cpu->esp + 4u * n);
}
static int wc_return(CPU *cpu, uint32_t result, unsigned n)
{
    cpu->eax = result;
    return_stdcall(cpu, n * 4u);
    return 1;
}
static void *wc_ptr(uint32_t address, uint64_t bytes, const import_record *i)
{
    void *p = bytes <= SIZE_MAX && bytes <= UINT32_MAX
                  ? guest_memory_try_ptr(address, (size_t)bytes)
                  : NULL;
    if (!p) {
        char reason[256];
        snprintf(reason, sizeof(reason), "%s!%s invalid guest range 0x%08x + %llu", i->dll, i->name,
                 address, (unsigned long long)bytes);
        stop_guest(reason, i->token);
    }
    return p;
}
static void wc_string(uint32_t p, char *out, size_t cap, const import_record *i)
{
    if (!copy_guest_string(p, out, cap)) {
        char reason[240];
        snprintf(reason, sizeof(reason), "%s invalid/overlong guest string", i->name);
        stop_guest(reason, i->token);
    }
}
static size_t wc_length(uint32_t p, int32_t n, unsigned width, int include_nul,
                        const import_record *i)
{
    if (n >= 0) {
        if (n)
            wc_ptr(p, (uint64_t)(uint32_t)n * width, i);
        return (size_t)n;
    }
    if (n != -1)
        return SIZE_MAX;
    for (uint32_t j = 0; j < 0x1000000u; ++j) {
        uint32_t at = p + j * width;
        if (at < p)
            break;
        uint8_t *v = wc_ptr(at, width, i);
        if (!v[0] && (width == 1 || !v[1]))
            return j + (include_nul != 0);
    }
    stop_guest("Win32 string exceeds guest limit", i->token);
    return 0;
}
static uint64_t wc_read64(uint32_t p)
{
    return rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wc_write64(uint32_t p, uint64_t n)
{
    wr32(p, (uint32_t)n);
    wr32(p + 4, (uint32_t)(n >> 32));
}
#ifndef _WIN32
static uint32_t wc_errno(void)
{
    switch (errno) {
    case ENOENT:
        return 2;
    case ENOTDIR:
        return 3;
    case EACCES:
    case EPERM:
    case EISDIR:
    case ELOOP:
        return 5;
    case EBADF:
        return 6;
    case ENOMEM:
        return 8;
    case EEXIST:
        return 183;
    case ENOTEMPTY:
        return 145;
    case ENOSPC:
        return 112;
    case EINVAL:
        return 87;
    default:
        return 1117;
    }
}
#endif
static int wc_resolve(const char *guest, char *host, int writing, int missing)
{
    char root[32768];
    if (!get_user_data_root(root, sizeof(root)))
        root[0] = 0;
    uint32_t error;
    int ok = ac1_files_resolve(guest_files, root, guest, writing, missing, host, 32768, &error);
    if (!ok)
        last_error = error;
    return ok;
}
static intptr_t wc_file(uint32_t handle, int writing)
{
    intptr_t native = -1;
    uint32_t error;
    if (!ac1_files_native(guest_files, handle, writing, &native, &error))
        last_error = error;
    return native;
}
#ifndef _WIN32
static int wc_wildcard(const char *pattern, const char *name)
{
    /* Win32's ubiquitous *.* also includes names with no extension. */
    if (!strcmp(pattern, "*.*"))
        pattern = "*";
    const char *star = NULL, *retry = NULL;
    while (*name) {
        if (*pattern == '*') {
            star = ++pattern;
            retry = name;
        } else if (*pattern == '?' ||
                   tolower((unsigned char)*pattern) == tolower((unsigned char)*name)) {
            ++pattern;
            ++name;
        } else if (star) {
            pattern = star;
            name = ++retry;
        } else
            return 0;
    }
    while (*pattern == '*' || *pattern == '?')
        ++pattern;
    return !*pattern;
}
static uint64_t wc_unix_filetime(time_t seconds, long ns)
{
    return WC_EPOCH + (uint64_t)seconds * WC_TICKS + (uint64_t)ns / 100;
}
#endif
static int wc_find_next(wc_find *f, void *output)
{
    uint8_t *data = output;
#ifdef _WIN32
    WIN32_FIND_DATAA native;
    while (FindNextFileA(f->native, &native)) {
        memcpy(data, &native, 320);
        return 1;
    }
    last_error = GetLastError();
#else
    struct dirent *entry;
    while ((entry = readdir(f->native))) {
        if (!wc_wildcard(f->pattern, entry->d_name))
            continue;
        if (strlen(entry->d_name) >= 260)
            continue;
        char path[32768];
        if (snprintf(path, sizeof(path), "%s/%s", f->directory, entry->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (lstat(path, &st))
            continue;
        if (S_ISLNK(st.st_mode))
            continue;
        memset(data, 0, 320);
        uint32_t attributes = S_ISDIR(st.st_mode) ? 16 : 32;
        memcpy(data, &attributes, 4);
        uint64_t t = wc_unix_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
        memcpy(data + 4, &t, 8);
        t = wc_unix_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
        memcpy(data + 12, &t, 8);
        t = wc_unix_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
        memcpy(data + 20, &t, 8);
        uint32_t high = (uint32_t)((uint64_t)st.st_size >> 32), low = (uint32_t)st.st_size;
        memcpy(data + 28, &high, 4);
        memcpy(data + 32, &low, 4);
        strcpy((char *)data + 44, entry->d_name);
        return 1;
    }
    last_error = 18;
#endif
    return 0;
}

/* Gregorian civil date algorithms (400-year eras), independent of time_t,
 * so FILETIME conversion covers Win32's full supported 1601..30827 range. */
static int64_t wc_days(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yr = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return (int64_t)era * 146097 + yr * 365 + yr / 4 - yr / 100 + doy - 719468;
}
static int wc_system_to_file(const uint16_t *s, uint64_t *out)
{
    int y = s[0], m = s[1], d = s[3];
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1601 || y > 30827 || m < 1 || m > 12 || d < 1 ||
        d > md[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) || s[4] > 23 ||
        s[5] > 59 || s[6] > 59 || s[7] > 999)
        return 0;
    *out = (uint64_t)(wc_days(y, m, d) - wc_days(1601, 1, 1)) * 86400 * WC_TICKS +
           ((uint64_t)s[4] * 3600 + s[5] * 60 + s[6]) * WC_TICKS + s[7] * UINT64_C(10000);
    return 1;
}
static int wc_file_to_system(uint64_t t, uint16_t *s)
{
    if (t >= UINT64_C(0x8000000000000000))
        return 0;
    int64_t days = (int64_t)(t / (86400 * WC_TICKS)) + wc_days(1601, 1, 1);
    int64_t z = days + 719468;
    int era = (int)((z >= 0 ? z : z - 146096) / 146097);
    unsigned doe = (unsigned)(z - (int64_t)era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    s[0] = (uint16_t)y;
    s[1] = (uint16_t)m;
    s[2] = (uint16_t)(((days + 4) % 7 + 7) % 7);
    s[3] = (uint16_t)d;
    s[4] = (uint16_t)(t / WC_TICKS / 3600 % 24);
    s[5] = (uint16_t)(t / WC_TICKS / 60 % 60);
    s[6] = (uint16_t)(t / WC_TICKS % 60);
    s[7] = (uint16_t)(t / 10000 % 1000);
    return 1;
}
#ifndef _WIN32
static const char *wc_locale(uint32_t type)
{
    /* en-US, following recomp-kit; unrecognized queries fail explicitly. */
    switch (type & 0xffff) {
    case 1:
        return "0409";
    case 2:
        return "English (United States)";
    case 3:
        return "ENU";
    case 4:
        return "English (United States)";
    case 5:
        return "1";
    case 6:
        return "United States";
    case 7:
        return "USA";
    case 8:
        return "United States";
    case 9:
        return "0409";
    case 10:
        return "1";
    case 11:
        return "437";
    case 12:
        return ",";
    case 13:
        return "0";
    case 14:
        return ".";
    case 15:
        return ",";
    case 16:
        return "3;0";
    case 17:
        return "2";
    case 18:
        return "1";
    case 19:
        return "0123456789";
    case 20:
        return "$";
    case 21:
        return "USD";
    case 22:
        return ".";
    case 23:
        return ",";
    case 24:
        return "3;0";
    case 25:
    case 26:
        return "2";
    case 27:
    case 28:
        return "0";
    case 29:
        return "/";
    case 30:
        return ":";
    case 31:
        return "M/d/yyyy";
    case 32:
        return "dddd, MMMM d, yyyy";
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
        return "0";
    case 40:
        return "AM";
    case 41:
        return "PM";
    case 0x1003:
        return "1";
    case 0x1004:
        return "1252";
    case 0x1009:
        return "1";
    case 0x100e:
        return "h:mm:ss tt";
    case 0x100a:
        return "0";
    case 0x100b:
        return "0";
    case 0x100c:
        return "0";
    case 0x100d:
        return "0";
    default:
        return NULL;
    }
}

#endif

/* Read RT_STRING directly from the already mapped PE, without loading host
 * code. All offsets are bounded both by the resource directory and memory. */
static uint32_t wc_resource_entry(uint32_t base, uint32_t size, uint32_t dir, unsigned id,
                                  int first)
{
    if (dir > size || size - dir < 16)
        return 0;
    uint8_t *d = guest_memory_try_ptr(base + dir, 16);
    if (!d)
        return 0;
    unsigned n = image_u16(d + 12) + image_u16(d + 14);
    if ((uint64_t)dir + 16 + (uint64_t)n * 8 > size)
        return 0;
    for (unsigned j = 0; j < n; ++j) {
        uint8_t *e = guest_memory_try_ptr(base + dir + 16 + j * 8, 8);
        if (!e)
            return 0;
        uint32_t name = image_u32(e);
        if (!(name & 0x80000000u) && (first || name == id))
            return image_u32(e + 4);
    }
    return 0;
}
static int wc_load_string(uint32_t module, unsigned id, char *out, int capacity)
{
    if (capacity <= 0)
        return 0;
    out[0] = 0;
    if (module && module != image_base) {
        last_error = 1814;
        return 0;
    }
    uint8_t *dos = guest_memory_try_ptr(image_base, 64);
    if (!dos || image_u16(dos) != 0x5a4d)
        return 0;
    uint32_t nt = image_u32(dos + 60);
    if ((uint64_t)nt + 144 > image_size_loaded)
        return 0;
    uint8_t *pe = guest_memory_try_ptr(image_base + nt, 144);
    if (!pe || image_u32(pe) != 0x4550)
        return 0;
    uint32_t rva = image_u32(pe + 136), size = image_u32(pe + 140);
    if (!rva || (uint64_t)rva + size > image_size_loaded)
        return 0;
    uint32_t base = image_base + rva;
    uint32_t type = wc_resource_entry(base, size, 0, 6, 0);
    if (!(type & 0x80000000u))
        return 0;
    uint32_t block = wc_resource_entry(base, size, type & 0x7fffffffu, (id >> 4) + 1, 0);
    if (!(block & 0x80000000u))
        return 0;
    uint32_t leaf = wc_resource_entry(base, size, block & 0x7fffffffu, 0, 1);
    if (!leaf || (leaf & 0x80000000u) || leaf > size || size - leaf < 16)
        return 0;
    uint8_t *entry = guest_memory_try_ptr(base + leaf, 16);
    if (!entry)
        return 0;
    uint32_t text = image_u32(entry), bytes = image_u32(entry + 4);
    if ((uint64_t)text + bytes > image_size_loaded)
        return 0;
    uint8_t *p = guest_memory_try_ptr(image_base + text, bytes);
    if (!p)
        return 0;
    for (unsigned j = 0; j <= (id & 15); ++j) {
        if (bytes < 2)
            return 0;
        unsigned n = image_u16(p);
        p += 2;
        bytes -= 2;
        if (n * 2 > bytes)
            return 0;
        if (j == (id & 15)) {
            unsigned copy = n < (unsigned)capacity - 1 ? n : (unsigned)capacity - 1;
#ifdef _WIN32
            int result = WideCharToMultiByte(1252, 0, (LPCWCH)p, n, NULL, 0, NULL, NULL);
            char *temp = malloc((size_t)result + 1);
            if (!temp)
                return 0;
            WideCharToMultiByte(1252, 0, (LPCWCH)p, n, temp, result, NULL, NULL);
            copy = (unsigned)result < (unsigned)capacity - 1 ? (unsigned)result
                                                             : (unsigned)capacity - 1;
            memcpy(out, temp, copy);
            free(temp);
#else
            for (unsigned k = 0; k < copy; ++k) {
                uint16_t c = image_u16(p + k * 2);
                uint8_t byte;
                out[k] = cp1252_byte(c, &byte) ? (char)byte : '?';
            }
#endif
            out[copy] = 0;
            return (int)copy;
        }
        p += n * 2;
        bytes -= n * 2;
    }
    return 0;
}

static int wc_file_import(CPU *cpu, const import_record *import, wc_state *s)
{
#define A(n) wc_arg(cpu, n)
#define IS(n) (!strcmp(import->name, n))
#define RET(v, n) return wc_return(cpu, (uint32_t)(v), n)
    if (IS("CreateFileA")) {
        char path[32768], root[32768];
        wc_string(A(1), path, sizeof(path), import);
        if (!get_user_data_root(root, sizeof(root)) || !profile_path_is_in_userdata(path))
            return 0;
        uint32_t handle = UINT32_MAX;
        if (A(4) || A(7))
            stop_guest("userdata CreateFileA security/template unsupported", import->token);
        uint32_t error;
        int ok = ac1_files_open_userdata(guest_files, root, path, A(2), A(3), A(5), A(6), &handle,
                                         &error);
        if (!ok || A(5) == 2 || A(5) == 4)
            last_error = error;
        RET(handle, 7);
    }
    if (IS("WriteFile") && A(1) != AC1_STD_OUTPUT_HANDLE && A(1) != AC1_STD_ERROR_HANDLE) {
        intptr_t native = wc_file(A(1), 1);
        uint32_t count = 0;
        int ok = 0;
        if (A(4))
            wc_ptr(A(4), 4, import);
        if (native != -1) {
            if (A(5))
                stop_guest("userdata WriteFile OVERLAPPED is unsupported", import->token);
            void *buffer = A(3) ? wc_ptr(A(2), A(3), import) : NULL;
#ifdef _WIN32
            DWORD written = 0;
            ok = WriteFile((HANDLE)native, buffer, A(3), &written, NULL);
            count = written;
            if (!ok)
                last_error = GetLastError();
#else
            ssize_t written = write((int)native, buffer, A(3));
            ok = written >= 0;
            if (ok)
                count = (uint32_t)written;
            else
                last_error = wc_errno();
#endif
        }
        if (A(4))
            wr32(A(4), count);
        RET(ok, 5);
    }
    if (IS("GetFileSize")) {
        uint64_t size = 0;
        uint32_t err = 0;
        int ok = ac1_files_get_size(guest_files, A(1), &size, &err);
        if (A(2))
            wc_ptr(A(2), 4, import);
        if (!ok || err) {
            last_error = err ? err : 6;
            RET(UINT32_MAX, 2);
        }
        if (A(2))
            wr32(A(2), (uint32_t)(size >> 32));
        if ((uint32_t)size == UINT32_MAX)
            last_error = 0;
        RET(size, 2);
    }
    if (IS("GetFileTime") || IS("SetFileTime")) {
        int setting = IS("SetFileTime");
        intptr_t native = wc_file(A(1), setting);
        void *p[3];
        for (int j = 0; j < 3; ++j)
            p[j] = A(j + 2) ? wc_ptr(A(j + 2), 8, import) : NULL;
        int ok = 0;
        if (native != -1) {
#ifdef _WIN32
            ok = setting ? SetFileTime((HANDLE)native, p[0], p[1], p[2])
                         : GetFileTime((HANDLE)native, p[0], p[1], p[2]);
            if (!ok)
                last_error = GetLastError();
#else
            struct stat st;
            ok = !fstat((int)native, &st);
            if (ok && setting) {
                /* POSIX has no creation-time setter. Refuse a requested
                 * creation time rather than silently discard it. */
                if (p[0] && wc_read64(A(2)) != 0 && wc_read64(A(2)) != UINT64_MAX) {
                    last_error = 50;
                    RET(0, 4);
                }
                struct timespec ts[2] = {st.st_atim, st.st_mtim};
                for (int j = 0; j < 2; ++j)
                    if (p[j + 1]) {
                        uint64_t t = wc_read64(A(j + 3));
                        if (t && t != UINT64_MAX) {
                            int64_t delta = (int64_t)(t - WC_EPOCH);
                            ts[j].tv_sec = delta / (int64_t)WC_TICKS;
                            ts[j].tv_nsec = delta % (int64_t)WC_TICKS * 100;
                            if (ts[j].tv_nsec < 0) {
                                --ts[j].tv_sec;
                                ts[j].tv_nsec += 1000000000;
                            }
                        }
                    }
                ok = !futimens((int)native, ts);
            } else if (ok) {
                if (p[0])
                    wc_write64(A(2), wc_unix_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec));
                if (p[1])
                    wc_write64(A(3), wc_unix_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec));
                if (p[2])
                    wc_write64(A(4), wc_unix_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec));
            }
            if (!ok)
                last_error = wc_errno();
#endif
        }
        RET(ok, 4);
    }
    if (IS("SetEndOfFile") || IS("FlushFileBuffers")) {
        intptr_t native = wc_file(A(1), 1);
        int ok = 0;
        if (native != -1) {
#ifdef _WIN32
            ok = IS("SetEndOfFile") ? SetEndOfFile((HANDLE)native)
                                    : FlushFileBuffers((HANDLE)native);
            if (!ok)
                last_error = GetLastError();
#else
            if (IS("SetEndOfFile")) {
                off_t pos = lseek((int)native, 0, SEEK_CUR);
                ok = pos >= 0 && !ftruncate((int)native, pos);
            } else
                ok = !fsync((int)native);
            if (!ok)
                last_error = wc_errno();
#endif
        }
        RET(ok, 1);
    }
    if (IS("LockFile") || IS("LockFileEx") || IS("UnlockFile")) {
        unsigned nargs = IS("LockFileEx") ? 6 : 5;
        if (wc_file(A(1), 0) == -1)
            RET(0, nargs);
        uint64_t off, len;
        if (IS("LockFileEx")) {
            if (A(2) & ~3u || A(3)) {
                last_error = 87;
                RET(0, 6);
            }
            wc_ptr(A(6), 20, import);
            off = wc_read64(A(6) + 8);
            len = A(4) | ((uint64_t)A(5) << 32);
        } else {
            off = A(2) | ((uint64_t)A(3) << 32);
            len = A(4) | ((uint64_t)A(5) << 32);
        }
        if (len && off > UINT64_MAX - len) {
            last_error = 87;
            RET(0, nargs);
        }
        if (IS("UnlockFile")) {
            for (unsigned j = 0; j < 128; ++j)
                if (s->locks[j].handle == A(1) && s->locks[j].offset == off &&
                    s->locks[j].length == len) {
                    s->locks[j].handle = 0;
                    RET(1, nargs);
                }
            last_error = 158;
            RET(0, nargs);
        }
        for (unsigned j = 0; j < 128; ++j)
            if (s->locks[j].handle == A(1) && len && s->locks[j].length &&
                off < s->locks[j].offset + s->locks[j].length && s->locks[j].offset < off + len) {
                last_error = 33;
                RET(0, nargs);
            }
        for (unsigned j = 0; j < 128; ++j)
            if (!s->locks[j].handle) {
                s->locks[j].handle = A(1);
                s->locks[j].offset = off;
                s->locks[j].length = len;
                RET(1, nargs);
            }
        last_error = 8;
        RET(0, nargs);
    }
    if (IS("CloseHandle")) {
        intptr_t native;
        uint32_t err;
        if (!ac1_files_native(guest_files, A(1), 0, &native, &err))
            return 0;
        for (unsigned j = 0; j < 128; ++j)
            if (s->locks[j].handle == A(1))
                s->locks[j].handle = 0;
        return 0; /* existing file service owns close/cancellation */
    }
    if (IS("FindClose") || IS("FindNextFileA")) {
        uint32_t slot = (A(1) - WC_FIND_BASE) / 4;
        if (A(1) < WC_FIND_BASE || (A(1) & 3) || slot >= 32 || !s->find[slot].used) {
            last_error = 6;
            RET(0, IS("FindClose") ? 1 : 2);
        }
        wc_find *f = &s->find[slot];
        if (IS("FindClose")) {
#ifdef _WIN32
            int ok = FindClose(f->native);
            if (!ok)
                last_error = GetLastError();
#else
            int ok = !closedir(f->native);
            if (!ok)
                last_error = wc_errno();
#endif
            f->used = 0;
            RET(ok, 1);
        }
        void *out = wc_ptr(A(2), 320, import);
        int ok = wc_find_next(f, out);
        RET(ok, 2);
    }
    if (IS("FindFirstFileA")) {
        char path[32768];
        wc_string(A(1), path, sizeof(path), import);
        char *leaf = path;
        for (char *p = path; *p; ++p)
            if (*p == '/' || *p == '\\')
                leaf = p + 1;
        char pattern[260];
        if (!*leaf || strlen(leaf) >= sizeof(pattern)) {
            last_error = 2;
            RET(UINT32_MAX, 2);
        }
        strcpy(pattern, leaf);
        if (leaf == path)
            strcpy(path, ".");
        else {
            /* Keep C:/ intact. */
            if (leaf == path + 3 && path[1] == ':')
                *leaf = 0;
            else
                leaf[-1] = 0;
        }
        unsigned slot;
        for (slot = 0; slot < 32 && s->find[slot].used; ++slot) {
        }
        if (slot == 32) {
            last_error = 4;
            RET(UINT32_MAX, 2);
        }
        wc_find *f = &s->find[slot];
        if (!wc_resolve(path, f->directory, 0, 0))
            RET(UINT32_MAX, 2);
        strcpy(f->pattern, pattern);
        void *out = wc_ptr(A(2), 320, import);
        int ok = 0;
#ifdef _WIN32
        char search[32768];
        if (snprintf(search, sizeof(search), "%s/%s", f->directory, pattern) >=
            (int)sizeof(search)) {
            last_error = 206;
            RET(UINT32_MAX, 2);
        }
        WIN32_FIND_DATAA data;
        f->native = FindFirstFileA(search, &data);
        if (f->native == INVALID_HANDLE_VALUE) {
            last_error = GetLastError();
            RET(UINT32_MAX, 2);
        }
        memcpy(out, &data, 320);
        ok = 1;
#else
        f->native = opendir(f->directory);
        if (!f->native) {
            last_error = wc_errno();
            RET(UINT32_MAX, 2);
        }
#endif
        f->used = 1;
        if (!ok)
            ok = wc_find_next(f, out);
        if (!ok) {
#ifdef _WIN32
            FindClose(f->native);
#else
            closedir(f->native);
#endif
            f->used = 0;
            last_error = 2;
            RET(UINT32_MAX, 2);
        }
        RET(WC_FIND_BASE + slot * 4, 2);
    }
    if (IS("GetTempPathA")) {
        char root[32768], path[32768];
        if (!get_user_data_root(root, sizeof(root)) ||
            snprintf(path, sizeof(path), "%s/temp", root) >= (int)sizeof(path) - 1) {
            last_error = 5;
            RET(0, 2);
        }
        char checked[32768];
        if (!wc_resolve(path, checked, 1, 1))
            RET(0, 2);
        if (!ensure_directory_tree(checked)) {
            last_error = 5;
            RET(0, 2);
        }
        if (!wc_resolve(path, checked, 1, 0))
            RET(0, 2);
#ifdef _WIN32
        strcat(path, "\\");
#else
        strcat(path, "/");
#endif
        size_t n = strlen(path);
        if (A(1) <= n)
            RET(n + 1, 2);
        memcpy(wc_ptr(A(2), n + 1, import), path, n + 1);
        RET(n, 2);
    }
    if (IS("GetFullPathNameA")) {
        char path[32768], full[32768];
        wc_string(A(1), path, sizeof(path), import);
#ifdef _WIN32
        char combined[32768];
        if (profile_path_is_absolute(path))
            snprintf(combined, sizeof(combined), "%s", path);
        else if (snprintf(combined, sizeof(combined), "%s\\%s", guest_current_directory, path) >=
                 (int)sizeof(combined)) {
            last_error = 206;
            RET(0, 4);
        }
        DWORD n = GetFullPathNameA(combined, sizeof(full), full, NULL);
        if (!n || n >= sizeof(full)) {
            last_error = n ? 206 : GetLastError();
            RET(0, 4);
        }
#else
        if (profile_component_is_parent(path)) {
            last_error = 5;
            RET(0, 4);
        }
        if (profile_path_is_absolute(path))
            snprintf(full, sizeof(full), "%s", path);
        else if (snprintf(full, sizeof(full), "%s\\%s", guest_current_directory, path) >=
                 (int)sizeof(full)) {
            last_error = 206;
            RET(0, 4);
        }
        size_t n = strlen(full);
#endif
        if (A(2) <= n)
            RET(n + 1, 4);
        memcpy(wc_ptr(A(3), n + 1, import), full, n + 1);
        if (A(4)) {
            wc_ptr(A(4), 4, import);
            const char *leaf = full;
            for (const char *p = full; *p; ++p)
                if (*p == '/' || *p == '\\')
                    leaf = p + 1;
            wr32(A(4), *leaf ? A(3) + (uint32_t)(leaf - full) : 0);
        }
        RET(n, 4);
    }
    if (IS("GetDriveTypeA")) {
        if (!A(1))
            RET(3, 1);
        char path[32768], host[32768];
        wc_string(A(1), path, sizeof(path), import);
        /* Only roots represented by the virtual installation/userdata exist. */
        if (strlen(path) == 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\') &&
            tolower((unsigned char)path[0]) == tolower((unsigned char)guest_current_directory[0]))
            RET(3, 1);
        if (!wc_resolve(path, host, 0, 0))
            RET(1, 1);
        RET(3, 1);
    }
    if (IS("GetPrivateProfileSectionA")) {
        char path[32768], host[32768], section[4096];
        wc_string(A(1), section, sizeof(section), import);
        wc_string(A(4), path, sizeof(path), import);
        if (!A(3))
            RET(0, 4);
        char *out = wc_ptr(A(2), A(3), import);
        out[0] = 0;
        if (A(3) > 1)
            out[1] = 0;
        if (!wc_resolve(path, host, 0, 0))
            RET(0, 4);
#ifdef _WIN32
        RET(GetPrivateProfileSectionA(section, out, A(3), host), 4);
#else
        FILE *f = fopen(host, "rb");
        if (!f)
            RET(0, 4);
        char line[8192];
        int in = 0;
        size_t used = 0;
        int truncated = 0;
        while (fgets(line, sizeof(line), f)) {
            char *p = trim_profile_text(line);
            if (*p == '[') {
                char *end = strchr(p, ']');
                if (end)
                    *end = 0;
                in = end && equal_ascii_fold(p + 1, section);
                continue;
            }
            if (!in || !*p || *p == ';' || !strchr(p, '='))
                continue;
            size_t n = strlen(p) + 1;
            if (used + n + 1 > A(3)) {
                truncated = 1;
                break;
            }
            memcpy(out + used, p, n);
            used += n;
        }
        fclose(f);
        out[used] = 0;
        if (truncated) {
            out[A(3) - 1] = 0;
            if (A(3) > 1)
                out[A(3) - 2] = 0;
            RET(A(3) > 1 ? A(3) - 2 : 0, 4);
        }
        RET(used, 4);
#endif
    }
    int get_attr = IS("GetFileAttributesA"), set_attr = IS("SetFileAttributesA");
    int create_dir = IS("CreateDirectoryA"), remove_dir = IS("RemoveDirectoryA");
    int del = IS("DeleteFileA"), move = IS("MoveFileA"), copy = IS("CopyFileA");
    if (get_attr || set_attr || create_dir || remove_dir || del || move || copy) {
        char path[32768], host[32768], dest[32768], target[32768];
        wc_string(A(1), path, sizeof(path), import);
        unsigned args = copy ? 3 : (set_attr || create_dir || move) ? 2 : 1;
        if (create_dir && A(2))
            stop_guest("CreateDirectoryA security attributes unsupported", import->token);
        if (!wc_resolve(path, host, !get_attr && !copy, create_dir))
            RET(get_attr ? UINT32_MAX : 0, args);
        if (move || copy) {
            wc_string(A(2), dest, sizeof(dest), import);
            if (!wc_resolve(dest, target, 1, 1))
                RET(0, args);
        }
#ifdef _WIN32
        if (get_attr) {
            DWORD attr = GetFileAttributesA(host);
            if (attr == INVALID_FILE_ATTRIBUTES)
                last_error = GetLastError();
            RET(attr, args);
        }
        int ok = set_attr     ? SetFileAttributesA(host, A(2))
                 : create_dir ? CreateDirectoryA(host, NULL)
                 : remove_dir ? RemoveDirectoryA(host)
                 : del        ? DeleteFileA(host)
                 : move       ? MoveFileA(host, target)
                              : CopyFileA(host, target, A(3));
        if (!ok)
            last_error = GetLastError();
#else
        struct stat st;
        if (get_attr) {
            if (stat(host, &st)) {
                last_error = wc_errno();
                RET(UINT32_MAX, args);
            }
            RET((S_ISDIR(st.st_mode) ? 16 : 32) | ((st.st_mode & S_IWUSR) ? 0 : 1), args);
        }
        int ok = 0;
        if (set_attr) {
            if (A(2) & ~(1u | 2u | 4u | 32u | 128u)) {
                last_error = 87;
                RET(0, args);
            }
            if (!stat(host, &st))
                ok = !chmod(host, (st.st_mode & ~0222) | ((A(2) & 1) ? 0 : 0200));
        } else if (create_dir)
            ok = !mkdir(host, 0700);
        else if (remove_dir)
            ok = !rmdir(host);
        else if (del)
            ok = !unlink(host);
        else if (move) {
            if (!lstat(target, &st)) {
                last_error = 183;
                RET(0, args);
            }
            ok = !rename(host, target);
        } else {
            int in = open(host, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            int out =
                in < 0
                    ? -1
                    : open(target,
                           O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (A(3) ? O_EXCL : O_TRUNC),
                           0600);
            ok = in >= 0 && out >= 0;
            if (ok) {
                char block[65536];
                ssize_t n;
                while ((n = read(in, block, sizeof(block))) > 0) {
                    ssize_t offset = 0;
                    while (offset < n) {
                        ssize_t w = write(out, block + offset, (size_t)(n - offset));
                        if (w <= 0) {
                            ok = 0;
                            break;
                        }
                        offset += w;
                    }
                    if (!ok)
                        break;
                }
                if (n < 0)
                    ok = 0;
            }
            int saved = errno;
            if (in >= 0)
                close(in);
            if (out >= 0)
                close(out);
            errno = saved;
        }
        if (!ok)
            last_error = copy && errno == EEXIST ? 80 : wc_errno();
#endif
        RET(ok, args);
    }
#undef A
#undef IS
#undef RET
    return 0;
}

static int wc_time_import(CPU *cpu, const import_record *import, wc_state *state)
{
#define A(n) wc_arg(cpu, n)
#define IS(n) (!strcmp(import->name, n))
#define RET(v, n) return wc_return(cpu, (uint32_t)(v), n)
    (void)state;
    if (IS("GetSystemTime") || IS("GetLocalTime")) {
        uint16_t *out = wc_ptr(A(1), 16, import);
#ifdef _WIN32
        if (IS("GetLocalTime"))
            GetLocalTime((SYSTEMTIME *)out);
        else
            GetSystemTime((SYSTEMTIME *)out);
#else
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        struct tm tm;
        if (IS("GetLocalTime"))
            localtime_r(&now.tv_sec, &tm);
        else
            gmtime_r(&now.tv_sec, &tm);
        out[0] = tm.tm_year + 1900;
        out[1] = tm.tm_mon + 1;
        out[2] = tm.tm_wday;
        out[3] = tm.tm_mday;
        out[4] = tm.tm_hour;
        out[5] = tm.tm_min;
        out[6] = tm.tm_sec;
        out[7] = now.tv_nsec / 1000000;
#endif
        RET(0, 1); /* void */
    }
    if (IS("SystemTimeToFileTime") || IS("FileTimeToSystemTime")) {
        int tofile = IS("SystemTimeToFileTime");
        void *in = wc_ptr(A(1), tofile ? 16 : 8, import),
             *out = wc_ptr(A(2), tofile ? 8 : 16, import);
        uint64_t t;
        uint16_t st[8];
        int ok = tofile ? wc_system_to_file(in, &t) : wc_file_to_system(wc_read64(A(1)), st);
        if (ok) {
            if (tofile)
                wc_write64(A(2), t);
            else
                memcpy(out, st, 16);
        } else
            last_error = 87;
        RET(ok, 2);
    }
    if (IS("FileTimeToLocalFileTime") || IS("LocalFileTimeToFileTime")) {
        wc_ptr(A(1), 8, import);
        wc_ptr(A(2), 8, import);
#ifdef _WIN32
        int ok = IS("FileTimeToLocalFileTime")
                     ? FileTimeToLocalFileTime(wc_ptr(A(1), 8, import), wc_ptr(A(2), 8, import))
                     : LocalFileTimeToFileTime(wc_ptr(A(1), 8, import), wc_ptr(A(2), 8, import));
        if (!ok)
            last_error = GetLastError();
#else
        uint64_t raw = wc_read64(A(1));
        uint16_t st[8];
        if (!wc_file_to_system(raw, st)) {
            last_error = 87;
            RET(0, 2);
        }
        struct tm tm = {0};
        time_t sec;
        if (IS("FileTimeToLocalFileTime")) {
            sec = (time_t)((int64_t)(raw - WC_EPOCH) / (int64_t)WC_TICKS);
            if (!localtime_r(&sec, &tm)) {
                last_error = 87;
                RET(0, 2);
            }
            sec = timegm(&tm);
        } else {
            tm.tm_year = st[0] - 1900;
            tm.tm_mon = st[1] - 1;
            tm.tm_mday = st[3];
            tm.tm_hour = st[4];
            tm.tm_min = st[5];
            tm.tm_sec = st[6];
            tm.tm_isdst = -1;
            sec = mktime(&tm);
        }
        wc_write64(A(2), WC_EPOCH + (uint64_t)sec * WC_TICKS + raw % WC_TICKS);
        int ok = 1;
#endif
        RET(ok, 2);
    }
    if (IS("GetTimeZoneInformation")) {
        void *out = wc_ptr(A(1), 172, import);
#ifdef _WIN32
        DWORD result = GetTimeZoneInformation(out);
        if (result == UINT32_MAX)
            last_error = GetLastError();
        RET(result, 1);
#else
        /* Describe the host current offset without inventing transition rules. */
        memset(out, 0, 172);
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        wr32(A(1), (uint32_t)(-tm.tm_gmtoff / 60));
        const char *name = tm.tm_zone ? tm.tm_zone : "UTC";
        for (unsigned j = 0; j < 31 && name[j]; ++j)
            wr16(A(1) + 4 + j * 2, (unsigned char)name[j]);
        RET(0, 1);
#endif
    }
    if (IS("GetProcessTimes")) {
        if (A(1) != UINT32_MAX) {
            last_error = 6;
            RET(0, 5);
        }
        for (int j = 2; j <= 5; ++j)
            wc_ptr(A(j), 8, import);
#ifdef _WIN32
        int ok =
            GetProcessTimes(GetCurrentProcess(), wc_ptr(A(2), 8, import), wc_ptr(A(3), 8, import),
                            wc_ptr(A(4), 8, import), wc_ptr(A(5), 8, import));
        if (!ok) {
            last_error = GetLastError();
        }
        RET(ok, 5);
#else
        /* Creation time is captured once with the guest process state. */
        struct timespec run;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &run);
        wc_write64(A(2), state->creation_time);
        wc_write64(A(3), 0);
        wc_write64(A(4), 0);
        wc_write64(A(5), (uint64_t)run.tv_sec * WC_TICKS + run.tv_nsec / 100);
        RET(1, 5);
#endif
    }
    if (IS("GetDateFormatA") || IS("GetTimeFormatA")) {
        int date = IS("GetDateFormatA");
        void *st = A(3) ? wc_ptr(A(3), 16, import) : NULL;
        char format[4096];
        const char *fmt = NULL;
        if (A(4)) {
            wc_string(A(4), format, sizeof(format), import);
            fmt = format;
        }
        char *out = A(6) ? wc_ptr(A(5), A(6), import) : NULL;
#ifdef _WIN32
        int r = date ? GetDateFormatA(A(1), A(2), st, fmt, out, (int)A(6))
                     : GetTimeFormatA(A(1), A(2), st, fmt, out, (int)A(6));
        if (!r) {
            last_error = GetLastError();
        }
        RET(r, 6);
#else
        if (fmt || A(2) & ~(date ? 0x80000007u : 0x8000000fu))
            stop_guest("date/time custom format or flags unsupported on this platform",
                       import->token);
        uint16_t local[8];
        if (st)
            memcpy(local, st, 16);
        else {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            local[0] = tm.tm_year + 1900;
            local[1] = tm.tm_mon + 1;
            local[3] = tm.tm_mday;
            local[4] = tm.tm_hour;
            local[5] = tm.tm_min;
            local[6] = tm.tm_sec;
            local[7] = 0;
        }
        uint64_t check;
        if (!wc_system_to_file(local, &check)) {
            last_error = 87;
            RET(0, 6);
        }
        char text[256];
        if (date) {
            if (A(2) & ~0x80000001u)
                stop_guest("long/year date format unsupported on this platform", import->token);
            snprintf(text, sizeof(text), "%u/%u/%04u", local[1], local[3], local[0]);
        } else {
            unsigned flags = A(2);
            char tail[64] = "";
            if (!(flags & 1)) {
                if (flags & 2)
                    snprintf(tail, sizeof(tail), ":%02u", local[5]);
                else
                    snprintf(tail, sizeof(tail), ":%02u:%02u", local[5], local[6]);
            }
            unsigned hour = (flags & 8) ? local[4] : (local[4] % 12 ? local[4] % 12 : 12);
            snprintf(text, sizeof(text), "%u%s%s%s", hour, tail, (flags & (4 | 8)) ? "" : " ",
                     (flags & (4 | 8)) ? ""
                     : local[4] < 12   ? "AM"
                                       : "PM");
        }
        size_t n = strlen(text) + 1;
        if (!A(6))
            RET(n, 6);
        if (A(6) < n) {
            last_error = 122;
            RET(0, 6);
        }
        memcpy(out, text, n);
        RET(n, 6);
#endif
    }
#undef A
#undef IS
#undef RET
    return 0;
}

static int wc_locale_import(CPU *cpu, const import_record *import)
{
#define A(n) wc_arg(cpu, n)
#define IS(n) (!strcmp(import->name, n))
#define RET(v, n) return wc_return(cpu, (uint32_t)(v), n)
    if (IS("GetUserDefaultLCID")) {
        RET(0x409, 0);
    }
    if (IS("GetOEMCP") || IS("GetConsoleCP") || IS("GetConsoleOutputCP")) {
#ifdef _WIN32
        RET(IS("GetOEMCP")       ? GetOEMCP()
            : IS("GetConsoleCP") ? GetConsoleCP()
                                 : GetConsoleOutputCP(),
            0);
#else
        RET(437, 0);
#endif
    }
    if (IS("IsValidLocale")) {
        if (A(2) != 1 && A(2) != 2) {
            last_error = 1004;
            RET(0, 2);
        }
        RET(A(1) == 0x409 || A(1) == 0x400 || A(1) == 0x800 || A(1) == 9, 2);
    }
    if (IS("EnumSystemLocalesA")) {
        if (A(2) & ~3u || (A(2) & 3u) == 3) {
            last_error = 1004;
            RET(0, 2);
        }
        uint32_t text = pcrecomp_heap_alloc(process_heap, 9, 0);
        if (!text) {
            last_error = 8;
            RET(0, 2);
        }
        memcpy(wc_ptr(text, 9, import), "00000409", 9);
        uint32_t result = 0;
        int ok = invoke_guest_stdcall_callback(cpu, A(1), &text, 1, &result);
        pcrecomp_heap_free(process_heap, text);
        if (!ok)
            stop_guest("EnumSystemLocalesA callback failed", import->token);
        RET(1, 2); /* callback FALSE stops enumeration, API still succeeds */
    }
    if (IS("CompareStringA") || IS("CompareStringW")) {
        unsigned width = IS("CompareStringW") ? 2 : 1;
        size_t an = wc_length(A(3), (int32_t)A(4), width, 0, import),
               bn = wc_length(A(5), (int32_t)A(6), width, 0, import);
        if (an == SIZE_MAX || bn == SIZE_MAX || !A(3) || !A(5)) {
            last_error = 87;
            RET(0, 6);
        }
        const void *a = wc_ptr(A(3), (an ? an : 1) * width, import),
                   *b = wc_ptr(A(5), (bn ? bn : 1) * width, import);
#ifdef _WIN32
        SetLastError(last_error);
        int r = width == 2 ? CompareStringW(A(1), A(2), a, (int)an, b, (int)bn)
                           : CompareStringA(A(1), A(2), a, (int)an, b, (int)bn);
        last_error = GetLastError();
        RET(r, 6);
#else
        if (A(2) & ~(1u | 0x08000000u))
            stop_guest("CompareString collation flags unsupported on this platform", import->token);
        int order = 0;
        for (size_t j = 0; j < an && j < bn; ++j) {
            uint16_t x = width == 2 ? ((const uint16_t *)a)[j] : ((const uint8_t *)a)[j];
            uint16_t y = width == 2 ? ((const uint16_t *)b)[j] : ((const uint8_t *)b)[j];
            if (x > 127 || y > 127)
                stop_guest("non-ASCII collation needs a platform NLS provider", import->token);
            if (A(2) & 1) {
                x = (uint16_t)tolower(x);
                y = (uint16_t)tolower(y);
            }
            if (x != y) {
                order = x < y ? -1 : 1;
                break;
            }
        }
        if (!order)
            order = an < bn ? -1 : an > bn ? 1 : 0;
        RET(order + 2, 6);
#endif
    }
    if (IS("GetLocaleInfoA") || IS("GetLocaleInfoW")) {
        unsigned width = IS("GetLocaleInfoW") ? 2 : 1;
        if ((int32_t)A(4) < 0) {
            last_error = 87;
            RET(0, 4);
        }
        void *out = A(4) ? wc_ptr(A(3), (uint64_t)A(4) * width, import) : NULL;
#ifdef _WIN32
        /* Stable en-US profile, matching the default LCID above. */
        uint32_t locale = A(1) == 0x400 || A(1) == 0x800 ? 0x409 : A(1);
        SetLastError(last_error);
        int r = width == 2 ? GetLocaleInfoW(locale, A(2), out, (int)A(4))
                           : GetLocaleInfoA(locale, A(2), out, (int)A(4));
        last_error = GetLastError();
        RET(r, 4);
#else
        if (A(1) != 0x409 && A(1) != 0x400 && A(1) != 0x800 && A(1) != 9) {
            last_error = 87;
            RET(0, 4);
        }
        const char *v = wc_locale(A(2));
        if (!v) {
            last_error = 1004;
            RET(0, 4);
        }
        if (A(2) & 0x20000000u) {
            unsigned need = 4 / width;
            if (!A(4))
                RET(need, 4);
            if (A(4) < need) {
                last_error = 122;
                RET(0, 4);
            }
            wr32(A(3), (uint32_t)strtoul(v, NULL, (A(2) & 0xffff) == 1 ? 16 : 10));
            RET(need, 4);
        }
        if (A(2) & ~(0x80000000u | 0x40000000u | 0xffffu)) {
            last_error = 1004;
            RET(0, 4);
        }
        size_t n = strlen(v) + 1;
        if (!A(4))
            RET(n, 4);
        if (A(4) < n) {
            last_error = 122;
            RET(0, 4);
        }
        for (size_t j = 0; j < n; ++j)
            if (width == 2)
                ((uint16_t *)out)[j] = (unsigned char)v[j];
            else
                ((char *)out)[j] = v[j];
        RET(n, 4);
#endif
    }
    if (IS("LCMapStringA") || IS("GetStringTypeA")) {
        int classify = IS("GetStringTypeA");
        size_t n = wc_length(A(3), (int32_t)A(4), 1, 1, import);
        if (n == SIZE_MAX || !n || !A(3)) {
            last_error = 87;
            RET(0, classify ? 5 : 6);
        }
        const char *source = wc_ptr(A(3), n, import);
        if (classify) {
            void *out = wc_ptr(A(5), n * 2, import);
#ifdef _WIN32
            SetLastError(last_error);
            int r = GetStringTypeA(A(1), A(2), source, (int)n, out);
            if (!r) {
                last_error = GetLastError();
            }
            RET(r, 5);
#else
            if (A(2) != 1)
                stop_guest("GetStringTypeA classification type unsupported", import->token);
            for (size_t j = 0; j < n; ++j) {
                unsigned char c = source[j];
                uint16_t bits = 0;
                if (c > 127)
                    stop_guest("GetStringTypeA non-ASCII needs NLS provider", import->token);
                if (isupper(c))
                    bits |= 1 | 0x100;
                if (islower(c))
                    bits |= 2 | 0x100;
                if (isdigit(c))
                    bits |= 4;
                if (isspace(c))
                    bits |= 8;
                if (ispunct(c))
                    bits |= 16;
                if (iscntrl(c))
                    bits |= 32;
                if (c == ' ' || c == '\t')
                    bits |= 64;
                if (isxdigit(c))
                    bits |= 128;
                ((uint16_t *)out)[j] = bits;
            }
            RET(1, 5);
#endif
        }
        if ((int32_t)A(6) < 0) {
            last_error = 87;
            RET(0, 6);
        }
        void *out = A(6) ? wc_ptr(A(5), A(6), import) : NULL;
#ifdef _WIN32
        SetLastError(last_error);
        int r = LCMapStringA(A(1), A(2), source, (int)n, out, (int)A(6));
        last_error = GetLastError();
        RET(r, 6);
#else
        if (A(2) != 0x100 && A(2) != 0x200)
            stop_guest("LCMapStringA mapping flags unsupported", import->token);
        if (!A(6))
            RET(n, 6);
        if (A(6) < n) {
            last_error = 122;
            RET(0, 6);
        }
        for (size_t j = 0; j < n; ++j) {
            unsigned char c = source[j];
            if (c > 127)
                stop_guest("LCMapStringA non-ASCII needs NLS provider", import->token);
            ((char *)out)[j] = (char)(A(2) == 0x100 ? tolower(c) : toupper(c));
        }
        RET(n, 6);
#endif
    }
#undef A
#undef IS
#undef RET
    return 0;
}

static int wc_misc_import(CPU *cpu, const import_record *import, wc_state *s)
{
#define A(n) wc_arg(cpu, n)
#define IS(n) (!strcmp(import->name, n))
#define RET(v, n) return wc_return(cpu, (uint32_t)(v), n)
    if (IS("IsDebuggerPresent")) {
        RET(0, 0);
    }
    if (IS("OutputDebugStringA")) {
        char text[32768];
        wc_string(A(1), text, sizeof(text), import);
        fprintf(stderr, "AC1 debug: %s\n", text);
        RET(0, 1);
    }
    if (IS("SetErrorMode")) {
        uint32_t old = s->error_mode;
        s->error_mode = A(1);
        RET(old, 1);
    }
    if (IS("ExitProcess") || IS("TerminateProcess") || IS("FatalAppExitA")) {
        int terminate = IS("TerminateProcess");
        if (terminate && A(1) != UINT32_MAX) {
            last_error = 6;
            RET(0, 2);
        }
        uint32_t code = IS("FatalAppExitA") ? 1 : A(terminate ? 2 : 1);
        if (IS("FatalAppExitA")) {
            char text[4096];
            wc_string(A(2), text, sizeof(text), import);
            fprintf(stderr, "AC1 fatal: %s\n", text);
        }
        char reason[256];
        snprintf(reason, sizeof(reason),
                 "guest process exit via %s code=%u caller=0x%08x return=0x%08x", import->name,
                 code, cpu->eip, rd32(cpu->esp));
        if (active_guest_thread)
            active_guest_thread->exit_code = code;
        /* All paths use the scheduler's existing cooperative stop and drain;
         * no host ExitProcess/TerminateProcess, no return into guest code. */
        stop_guest(reason, import->token);
        return 1;
    }
    /* Current runtime: main/worker TIB ExceptionList starts at -1;
     * SetUnhandledExceptionFilter stores a pointer but never invokes it;
     * RaiseException accepts only the 0x406d1388 debugger naming protocol.
     * Full support needs guest EXCEPTION_RECORD/CONTEXT marshalling, checked
     * FS:[0] frame traversal, guest handler callbacks and dispositions,
     * nested exceptions, target-frame unwind flags and nonlocal transfer
     * across translated C call frames. A host RtlUnwind cannot do this.
     * DebugBreak likewise requires an EXCEPTION_BREAKPOINT delivery path. */
    if (IS("DebugBreak") || IS("RtlUnwind") || IS("UnhandledExceptionFilter")) {
        char reason[256];
        snprintf(reason, sizeof(reason),
                 "%s requires guest SEH dispatch/unwind (FS:[0] chain, CONTEXT and nested C++ "
                 "handlers); caller=0x%08x",
                 import->name, cpu->eip);
        stop_guest(reason, import->token);
        return 1;
    }
    if (IS("LocalFree")) {
        if (!A(1)) {
            RET(0, 1);
        }
        if (pcrecomp_heap_free(process_heap, A(1))) {
            RET(0, 1);
        }
        last_error = 6;
        RET(A(1), 1);
    }
    if (IS("FormatMessageA")) {
        uint32_t flags = A(1);
        char text[4096];
        if ((flags & ~(0x100u | 0x200u | 0x1000u | 0xffu)) || !(flags & 0x1000u) || A(2))
            stop_guest("FormatMessageA supports FROM_SYSTEM/IGNORE_INSERTS only", import->token);
#ifdef _WIN32
        DWORD n =
            FormatMessageA((flags & ~0x100u) | 0x200u, NULL, A(3), A(4), text, sizeof(text), NULL);
        if (!n) {
            last_error = GetLastError();
            RET(0, 7);
        }
#else
        const char *msg = NULL;
        switch (A(3)) {
        case 0:
            msg = "The operation completed successfully.\r\n";
            break;
        case 2:
            msg = "The system cannot find the file specified.\r\n";
            break;
        case 3:
            msg = "The system cannot find the path specified.\r\n";
            break;
        case 5:
            msg = "Access is denied.\r\n";
            break;
        case 6:
            msg = "The handle is invalid.\r\n";
            break;
        case 8:
            msg = "Not enough memory resources are available to process this command.\r\n";
            break;
        case 18:
            msg = "There are no more files.\r\n";
            break;
        case 87:
            msg = "The parameter is incorrect.\r\n";
            break;
        case 122:
            msg = "The data area passed to a system call is too small.\r\n";
            break;
        case 126:
            msg = "The specified module could not be found.\r\n";
            break;
        case 183:
            msg = "Cannot create a file when that file already exists.\r\n";
            break;
        case 1167:
            msg = "The device is not connected.\r\n";
            break;
        default:
            last_error = 317;
            RET(0, 7);
        }
        strcpy(text, msg);
        size_t n = strlen(text);
        if ((flags & 0xffu) == 0xffu) {
            while (n && (text[n - 1] == '\r' || text[n - 1] == '\n'))
                --n;
            text[n] = 0;
        }
#endif
        if (!(flags & 0x200u)) {
            for (const char *p = text; *p; ++p)
                if (*p == '%' && p[1] >= '1' && p[1] <= '9')
                    stop_guest("FormatMessageA guest insert expansion is unsupported",
                               import->token);
        }
        if (flags & 0x100u) {
            wc_ptr(A(5), 4, import);
            uint32_t bytes = (uint32_t)n + 1;
            if (A(6) > bytes)
                bytes = A(6);
            uint32_t p = pcrecomp_heap_alloc(process_heap, bytes, 1);
            if (!p) {
                last_error = 8;
                RET(0, 7);
            }
            memcpy(wc_ptr(p, (uint64_t)n + 1, import), text, (size_t)n + 1);
            wr32(A(5), p);
        } else {
            if (A(6) <= n) {
                last_error = 122;
                RET(0, 7);
            }
            memcpy(wc_ptr(A(5), (uint64_t)n + 1, import), text, (size_t)n + 1);
        }
        RET(n, 7);
    }
    if (IS("GetComputerNameExA")) {
        wc_ptr(A(3), 4, import);
        uint32_t cap = rd32(A(3));
        if (A(1) > 7) {
            last_error = 87;
            RET(0, 3);
        }
#ifdef _WIN32
        char *out = cap ? wc_ptr(A(2), cap, import) : NULL;
        DWORD count = cap;
        int ok = GetComputerNameExA(A(1), out, &count);
        wr32(A(3), count);
        if (!ok)
            last_error = GetLastError();
        RET(ok, 3);
#else
        const char *name = (A(1) == 2 || A(1) == 6) ? "" : "AC1";
        uint32_t n = (uint32_t)strlen(name);
        if (cap <= n) {
            wr32(A(3), n + 1);
            last_error = 234;
            RET(0, 3);
        }
        memcpy(wc_ptr(A(2), n + 1, import), name, n + 1);
        wr32(A(3), n);
        RET(1, 3);
#endif
    }
    if (IS("LoadLibraryA")) {
        char name[32768];
        wc_string(A(1), name, sizeof(name), import);
        /* Paths must not accidentally resolve to a known basename. */
        if (equal_ascii_fold(name, "kernel32.dll") || equal_ascii_fold(name, "kernel32")) {
            RET(AC1_KERNEL32_HANDLE, 1);
        }
        if (bink_image_mapped &&
            (equal_ascii_fold(name, "binkw32.dll") || equal_ascii_fold(name, bink_module_path))) {
            RET(bink_image.load_base, 1);
        }
        if (equal_ascii_fold(name, exe_path_copy) ||
            equal_ascii_fold(name, "AssassinsCreed_Dx9.exe")) {
            RET(image_base, 1);
        }
        last_error = 126;
        RET(0, 1);
    }
    if (IS("FreeLibrary")) {
        int ok = A(1) == AC1_KERNEL32_HANDLE || A(1) == image_base ||
                 (bink_image_mapped && A(1) == bink_image.load_base);
        if (!ok) {
            last_error = 6;
        }
        RET(ok, 1);
    }
    if (IS("SetEnvironmentVariableA") || IS("GetEnvironmentVariableA")) {
        char key[32768];
        wc_string(A(1), key, sizeof(key), import);
        if (!*key || strchr(key, '=')) {
            last_error = 87;
            RET(0, IS("SetEnvironmentVariableA") ? 2 : 3);
        }
        unsigned slot, empty = 128;
        for (slot = 0; slot < 128; ++slot) {
            if (!s->env[slot].name) {
                if (empty == 128)
                    empty = slot;
            } else if (equal_ascii_fold(s->env[slot].name, key))
                break;
        }
        if (IS("GetEnvironmentVariableA")) {
            if (slot == 128) {
                last_error = 203;
                RET(0, 3);
            }
            size_t n = strlen(s->env[slot].value);
            if (A(3) <= n) {
                RET(n + 1, 3);
            }
            memcpy(wc_ptr(A(2), n + 1, import), s->env[slot].value, n + 1);
            RET(n, 3);
        }
        if (!A(2)) {
            if (slot < 128) {
                free(s->env[slot].name);
                free(s->env[slot].value);
                memset(&s->env[slot], 0, sizeof(s->env[slot]));
            }
            RET(1, 2);
        }
        char value[32768];
        wc_string(A(2), value, sizeof(value), import);
        if (slot == 128)
            slot = empty;
        if (slot == 128) {
            last_error = 8;
            RET(0, 2);
        }
        char *newkey = strdup(key), *newvalue = strdup(value);
        if (!newkey || !newvalue) {
            free(newkey);
            free(newvalue);
            last_error = 8;
            RET(0, 2);
        }
        free(s->env[slot].name);
        free(s->env[slot].value);
        s->env[slot].name = newkey;
        s->env[slot].value = newvalue;
        RET(1, 2);
    }
    if (IS("GetEnvironmentStrings") || IS("GetEnvironmentStringsA")) {
        size_t n = 1;
        unsigned count = 0;
        for (unsigned j = 0; j < 128; ++j)
            if (s->env[j].name) {
                n += strlen(s->env[j].name) + strlen(s->env[j].value) + 2;
                ++count;
            }
        if (!count)
            ++n;
        uint32_t p = pcrecomp_heap_alloc(process_heap, (uint32_t)n, 1);
        if (!p) {
            last_error = 8;
            RET(0, 0);
        }
        char *out = wc_ptr(p, n, import);
        size_t used = 0;
        for (unsigned j = 0; j < 128; ++j)
            if (s->env[j].name) {
                int wrote =
                    snprintf(out + used, n - used, "%s=%s", s->env[j].name, s->env[j].value);
                used += (size_t)wrote + 1;
            }
        out[used] = 0;
        RET(p, 0);
    }
    if (IS("FreeEnvironmentStringsA")) {
        int ok = pcrecomp_heap_free(process_heap, A(1));
        if (!ok)
            last_error = 87;
        RET(ok, 1);
    }
    if (IS("SetConsoleCtrlHandler")) {
        if (!A(1)) {
            s->ignore_ctrl_c = A(2) != 0;
            RET(1, 2);
        }
        if (A(2)) {
            for (unsigned j = 0; j < 32; ++j)
                if (!s->console_handlers[j]) {
                    s->console_handlers[j] = A(1);
                    RET(1, 2);
                }
            last_error = 8;
            RET(0, 2);
        }
        for (unsigned j = 32; j > 0; --j)
            if (s->console_handlers[j - 1] == A(1)) {
                s->console_handlers[j - 1] = 0;
                RET(1, 2);
            }
        last_error = 87;
        RET(0, 2);
    }
    if (IS("SetStdHandle")) {
        int which = A(1) == (uint32_t)-10   ? 0
                    : A(1) == (uint32_t)-11 ? 1
                    : A(1) == (uint32_t)-12 ? 2
                                            : -1;
        if (which < 0) {
            last_error = 87;
            RET(0, 2);
        }
        s->standard[which] = A(2);
        RET(1, 2);
    }
    if (IS("GetStdHandle")) {
        int which = A(1) == (uint32_t)-10   ? 0
                    : A(1) == (uint32_t)-11 ? 1
                    : A(1) == (uint32_t)-12 ? 2
                                            : -1;
        if (which < 0) {
            last_error = 87;
            RET(UINT32_MAX, 1);
        }
        RET(s->standard[which], 1);
    }
    if (IS("WriteConsoleA") || IS("WriteConsoleW")) {
        int wide = IS("WriteConsoleW");
        uint32_t handle = A(1), units = A(3);
        if (A(4)) {
            wc_ptr(A(4), 4, import);
            wr32(A(4), 0);
        }
        if (A(5) || (handle != AC1_STD_OUTPUT_HANDLE && handle != AC1_STD_ERROR_HANDLE)) {
            last_error = A(5) ? 87 : 6;
            RET(0, 5);
        }
        const void *data = units ? wc_ptr(A(2), (uint64_t)units * (wide ? 2 : 1), import) : NULL;
        FILE *stream = handle == AC1_STD_ERROR_HANDLE ? stderr : stdout;
        int ok = 1;
        if (wide) {
            const uint16_t *w = data;
            for (uint32_t j = 0; j < units; ++j) {
                uint32_t c = w[j];
                if (c >= 0xd800 && c <= 0xdbff && j + 1 < units && w[j + 1] >= 0xdc00 &&
                    w[j + 1] <= 0xdfff)
                    c = 0x10000 + ((c - 0xd800) << 10) + (w[++j] - 0xdc00);
                else if (c >= 0xd800 && c <= 0xdfff)
                    c = 0xfffd;
                unsigned char b[4];
                size_t n;
                if (c < 128) {
                    b[0] = (unsigned char)c;
                    n = 1;
                } else if (c < 2048) {
                    b[0] = 0xc0 | (c >> 6);
                    b[1] = 0x80 | (c & 63);
                    n = 2;
                } else if (c < 65536) {
                    b[0] = 0xe0 | (c >> 12);
                    b[1] = 0x80 | ((c >> 6) & 63);
                    b[2] = 0x80 | (c & 63);
                    n = 3;
                } else {
                    b[0] = 0xf0 | (c >> 18);
                    b[1] = 0x80 | ((c >> 12) & 63);
                    b[2] = 0x80 | ((c >> 6) & 63);
                    b[3] = 0x80 | (c & 63);
                    n = 4;
                }
                if (fwrite(b, 1, n, stream) != n) {
                    ok = 0;
                    break;
                }
            }
        } else if (units && fwrite(data, 1, units, stream) != units)
            ok = 0;
        fflush(stream);
        if (!ok)
            last_error = 29;
        if (A(4))
            wr32(A(4), ok ? units : 0);
        RET(ok, 5);
    }
    if (IS("PulseEvent")) {
        sync_object *event = sync_object_for_handle(A(1));
        if (!event || event->kind != 1) {
            last_error = 6;
            RET(0, 1);
        }
        /* Only threads already waiting are released; no signaled state remains.
         * This matches guest scheduler waits, not host PulseEvent timing. */
#ifdef _WIN32
        if (event->host_event)
            ResetEvent(event->host_event);
#else
        event->signaled = 0;
#endif
        unsigned released = 0;
        if (active_scheduler) {
            scheduler_lock(active_scheduler);
            for (unsigned j = 0; j < active_scheduler->count; ++j) {
                ac1_guest_thread *t = &active_scheduler->threads[j];
                if (!t->blocked || t->wait_kind != 1)
                    continue;
                unsigned index = 0;
                int match = t->wait_handle == A(1);
                if (t->wait_count) {
                    match = 0;
                    for (index = 0; index < t->wait_count; ++index)
                        if (t->wait_handles[index] == A(1)) {
                            match = 1;
                            break;
                        }
                }
                if (match) {
                    t->blocked = 0;
                    t->wait_result = AC1_WAIT_OBJECT_0 + index;
                    t->cpu.eax = t->wait_result;
                    t->wait_kind = 0;
                    t->wait_count = 0;
                    ++released;
                    if (!event->manual_reset)
                        break;
                }
            }
            if (released) {
                scheduler_notify(active_scheduler);
            }
            scheduler_unlock(active_scheduler);
        }
        RET(1, 1);
    }
    if (IS("OpenThread") || IS("DuplicateHandle")) {
        ac1_guest_thread *thread = NULL;
        int duplicate = IS("DuplicateHandle");
        if (duplicate) {
            if (A(1) != UINT32_MAX || A(3) != UINT32_MAX || A(7) & ~3u) {
                last_error = 87;
                RET(0, 7);
            }
            uint32_t h = A(2);
            if (h >= WC_ALIAS_BASE && (h - WC_ALIAS_BASE) / 4 < 128 && !(h & 3))
                h = s->aliases[(h - WC_ALIAS_BASE) / 4];
            int from_alias =
                A(2) >= WC_ALIAS_BASE && !(A(2) & 3) && (A(2) - WC_ALIAS_BASE) / 4 < 128;
            thread = h == UINT32_MAX - 1 ? active_guest_thread : thread_for_handle(h);
            if (!from_alias && h >= AC1_THREAD_HANDLE_BASE && !(h & 3) &&
                (h - AC1_THREAD_HANDLE_BASE) / 4 < AC1_MAX_THREADS &&
                s->closed_threads[(h - AC1_THREAD_HANDLE_BASE) / 4])
                thread = NULL;
            wc_ptr(A(4), 4, import);
        } else if (active_scheduler) {
            for (unsigned j = 0; j < active_scheduler->count; ++j)
                if (active_scheduler->threads[j].id == A(3))
                    thread = &active_scheduler->threads[j];
        }
        if (!thread) {
            last_error = duplicate ? 6 : 87;
            RET(0, duplicate ? 7 : 3);
        }
        unsigned j;
        for (j = 0; j < 128 && s->aliases[j]; ++j) {
        }
        if (j == 128) {
            last_error = 4;
            RET(0, duplicate ? 7 : 3);
        }
        thread->closed = 0; /* An OpenThread reference keeps the object available. */
        s->aliases[j] = thread->handle;
        uint32_t alias = WC_ALIAS_BASE + j * 4;
        if (duplicate) {
            wr32(A(4), alias);
            if (A(7) & 1u) {
                uint32_t source = A(2);
                if (source >= WC_ALIAS_BASE && (source - WC_ALIAS_BASE) / 4 < 128 && !(source & 3))
                    s->aliases[(source - WC_ALIAS_BASE) / 4] = 0;
                else if (source >= AC1_THREAD_HANDLE_BASE &&
                         (source - AC1_THREAD_HANDLE_BASE) / 4 < AC1_MAX_THREADS)
                    s->closed_threads[(source - AC1_THREAD_HANDLE_BASE) / 4] = 1;
            }
            RET(1, 7);
        }
        RET(alias, 3);
    }
#undef A
#undef IS
#undef RET
    return 0;
}

static int wc_user_import(CPU *cpu, const import_record *import, wc_state *s)
{
#define A(n) wc_arg(cpu, n)
#define IS(n) (!strcmp(import->name, n))
#define RET(v, n) return wc_return(cpu, (uint32_t)(v), n)
    if (IS("MessageBoxA")) {
        char text[32768] = "", caption[4096] = "";
        if (A(2))
            wc_string(A(2), text, sizeof(text), import);
        if (A(3))
            wc_string(A(3), caption, sizeof(caption), import);
        fprintf(stderr, "AC1 MessageBox [%s]: %s\n", caption, text);
        RET(1, 4);
    }
    if (IS("GetKeyboardState")) {
        void *out = wc_ptr(A(1), 256, import);
#ifdef _WIN32
        int ok = GetKeyboardState(out);
        if (!ok)
            last_error = GetLastError();
        RET(ok, 1);
#else
        (void)out;
        last_error = 120;
        RET(0, 1);
#endif
    }
    if (IS("ToUnicode")) {
        void *keys = wc_ptr(A(3), 256, import);
        if ((int32_t)A(5) <= 0) {
            last_error = 87;
            RET(0, 6);
        }
        void *out = wc_ptr(A(4), (uint64_t)A(5) * 2, import);
#ifdef _WIN32
        RET(ToUnicodeEx(A(1), A(2), keys, out, (int)A(5), A(6), GetKeyboardLayout(0)), 6);
#else
        (void)keys;
        (void)out;
        stop_guest("ToUnicode requires a platform keyboard layout provider", import->token);
        return 1;
#endif
    }
    if (IS("CallNextHookEx")) {
        RET(0, 4);
    } /* no native or guest hooks are installed */
    if (IS("UnhookWindowsHookEx")) {
        last_error = 1404;
        RET(0, 1);
    } /* ERROR_INVALID_HOOK_HANDLE */
    if (IS("LoadStringA")) {
        if ((int32_t)A(4) <= 0)
            RET(0, 4);
        char *out = wc_ptr(A(3), A(4), import);
        int n = wc_load_string(A(1), A(2), out, (int)A(4));
        RET(n, 4);
    }
    if (IS("SystemParametersInfoA")) {
        uint32_t action = A(1), bytes = 0;
        /* SET accessibility actions are intentionally guest-local no-ops. */
        if (action == 0x33 || action == 0x35 || action == 0x3b) {
            if (!s->settings_logged) {
                fprintf(stderr, "AC1 suppressed accessibility system-setting changes\n");
                s->settings_logged = 1;
            }
            RET(1, 4);
        }
        switch (action) {
        case 0x32:
            bytes = 24;
            break; /* FILTERKEYS */
        case 0x34:
        case 0x3a:
        case 0x48:
            bytes = 8;
            break; /* TOGGLEKEYS, STICKYKEYS */
        case 0x30:
            bytes = 16;
            break; /* WORKAREA */
        case 3:
            bytes = 12;
            break; /* MOUSE */
        case 1:
        case 5:
        case 0xa:
        case 0xe:
        case 0x10:
        case 0x16:
        case 0x26:
        case 0x38:
        case 0x46:
        case 0x4a:
        case 0x1000:
        case 0x1002:
        case 0x1004:
        case 0x1006:
        case 0x1008:
        case 0x100a:
        case 0x100c:
        case 0x100e:
        case 0x1010:
        case 0x1012:
        case 0x1014:
        case 0x1016:
        case 0x1018:
        case 0x101a:
        case 0x101c:
        case 0x101e:
        case 0x1022:
        case 0x1024:
        case 0x1026:
        case 0x1028:
        case 0x103e:
        case 0x1042:
        case 0x1044:
        case 0x1046:
        case 0x1048:
        case 0x2000:
        case 0x2002:
        case 0x2004:
        case 0x2006:
        case 0x2008:
        case 0x200a:
        case 0x200c:
        case 0x200e:
        case 0x2010:
        case 0x2012:
            bytes = 4;
            break;
        default:
            stop_guest("SystemParametersInfoA action needs explicit GET/SET classification",
                       import->token);
            return 1;
        }
        void *out = wc_ptr(A(3), bytes, import);
        if ((action == 0x32 || action == 0x34 || action == 0x3a || action == 0x48) &&
            rd32(A(3)) != bytes) {
            last_error = 87;
            RET(0, 4);
        }
#ifdef _WIN32
        int ok = SystemParametersInfoA(action, A(2), out, 0);
        if (!ok)
            last_error = GetLastError();
        RET(ok, 4);
#else
        (void)out;
        last_error = 120;
        RET(0, 4);
#endif
    }
#undef A
#undef IS
#undef RET
    return 0;
}

static int wc_thread_alias(CPU *cpu, const import_record *import, wc_state *s)
{
    if (import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    uint32_t a = wc_arg(cpu, 1);
    if (!strcmp(import->name, "WaitForMultipleObjects")) {
        unsigned n = a;
        if (!n || n > 64)
            return 0;
        uint32_t array = wc_arg(cpu, 2);
        wc_ptr(array, n * 4, import);
        uint32_t original[64];
        int changed = 0;
        for (unsigned j = 0; j < n; ++j) {
            uint32_t h = original[j] = rd32(array + j * 4);
            if (h >= WC_ALIAS_BASE && !(h & 3) && (h - WC_ALIAS_BASE) / 4 < 128) {
                uint32_t canonical = s->aliases[(h - WC_ALIAS_BASE) / 4];
                wr32(array + j * 4, canonical);
                changed = 1;
            } else if (h >= AC1_THREAD_HANDLE_BASE && !(h & 3) &&
                       (h - AC1_THREAD_HANDLE_BASE) / 4 < AC1_MAX_THREADS &&
                       s->closed_threads[(h - AC1_THREAD_HANDLE_BASE) / 4]) {
                wr32(array + j * 4, 0);
                changed = 1;
            }
        }
        if (!changed)
            return 0;
        int handled = handle_sync_import(cpu, import);
        for (unsigned j = 0; j < n; ++j) {
            wr32(array + j * 4, original[j]);
        }
        return handled;
    }
    static const char *const consumers[] = {"CloseHandle",           "GetThreadId",
                                            "GetExitCodeThread",     "GetThreadPriority",

                                            "SetThreadPriority",     "SetThreadPriorityBoost",
                                            "SetThreadAffinityMask", "ResumeThread",
                                            "SuspendThread",         "WaitForSingleObject"};
    int consume = 0;
    for (unsigned j = 0; j < sizeof(consumers) / sizeof(consumers[0]); ++j)
        if (!strcmp(import->name, consumers[j])) {
            consume = 1;
            break;
        }
    if (!consume)
        return 0;
    if (a >= AC1_THREAD_HANDLE_BASE && !(a & 3) &&
        (a - AC1_THREAD_HANDLE_BASE) / 4 < AC1_MAX_THREADS) {
        unsigned thread_slot = (a - AC1_THREAD_HANDLE_BASE) / 4;
        if (!strcmp(import->name, "CloseHandle") && !s->closed_threads[thread_slot]) {
            for (unsigned j = 0; j < 128; ++j)
                if (s->aliases[j] == a) {
                    s->closed_threads[thread_slot] = 1;
                    return wc_return(cpu, 1, 1);
                }
        }
        if (!s->closed_threads[thread_slot])
            return 0;
        /* Pass an invalid canonical token to the ordinary API so its own
         * return shape and stdcall cleanup remain authoritative. */
        uint32_t location = cpu->esp + 4;
        wr32(location, 0);
        int handled = handle_thread_import(cpu, import) || handle_sync_import(cpu, import) ||
                      handle_heap_import(cpu, import) || handle_import(cpu, import);
        wr32(location, a);
        return handled;
    }
    if (a < WC_ALIAS_BASE || (a & 3) || (a - WC_ALIAS_BASE) / 4 >= 128)
        return 0;
    unsigned slot = (a - WC_ALIAS_BASE) / 4;
    uint32_t canonical = s->aliases[slot];
    if (!canonical) {
        last_error = 6;
        int one = !strcmp(import->name, "CloseHandle") || !strcmp(import->name, "GetThreadId") ||
                  !strcmp(import->name, "GetThreadPriority") ||
                  !strcmp(import->name, "ResumeThread") || !strcmp(import->name, "SuspendThread");
        uint32_t failure =
            !strcmp(import->name, "GetThreadPriority") ? 0x7fffffffu
            : (!strcmp(import->name, "ResumeThread") || !strcmp(import->name, "SuspendThread") ||
               !strcmp(import->name, "WaitForSingleObject"))
                ? UINT32_MAX
                : 0;
        return wc_return(cpu, failure, one ? 1 : 2);
    }
    if (!strcmp(import->name, "CloseHandle")) {
        s->aliases[slot] = 0;
        unsigned thread_slot = (canonical - AC1_THREAD_HANDLE_BASE) / 4;
        if (thread_slot < AC1_MAX_THREADS && s->closed_threads[thread_slot]) {
            unsigned j;
            for (j = 0; j < 128 && s->aliases[j] != canonical; ++j) {
            }
            if (j == 128) {
                ac1_guest_thread *t = thread_for_handle(canonical);
                if (t)
                    t->closed = 1;
            }
        }
        return wc_return(cpu, 1, 1);
    }
    uint32_t location = cpu->esp + 4;
    wr32(location, canonical);
    int handled = handle_thread_import(cpu, import) || handle_sync_import(cpu, import) ||
                  handle_heap_import(cpu, import) || handle_import(cpu, import);
    wr32(location, a);
    if (!handled)
        stop_guest("thread alias consumer has no runtime handler", import->token);
    return handled;
}

static int handle_win32_compat_import(CPU *cpu, const import_record *import)
{
    if (!import)
        return 0;
    if (equal_ascii_fold(import->dll, "XINPUT1_3.dll") && import->by_ordinal &&
        import->ordinal >= 2 && import->ordinal <= 4) {
        unsigned ordinal = import->ordinal;
        uint32_t user = wc_arg(cpu, 1), flags = ordinal == 4 ? wc_arg(cpu, 2) : 0;
        void *buffer = wc_ptr(wc_arg(cpu, ordinal == 4 ? 3 : 2),
                              ordinal == 2   ? 16
                              : ordinal == 3 ? 4
                                             : 20,
                              import);
        uint32_t result = 1167;
#ifdef _WIN32
        HMODULE module = LoadLibraryExA("xinput1_3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            FARPROC p = GetProcAddress(module, (LPCSTR)(uintptr_t)ordinal);
            if (p) {
                if (ordinal == 4) {
                    typedef DWORD(WINAPI * caps_fn)(DWORD, DWORD, XINPUT_CAPABILITIES *);
                    caps_fn fn;
                    memcpy(&fn, &p, sizeof(fn));
                    result = fn(user, flags, buffer);
                } else {
                    typedef DWORD(WINAPI * state_fn)(DWORD, void *);
                    state_fn fn;
                    memcpy(&fn, &p, sizeof(fn));
                    result = fn(user, buffer);
                }
            }
            FreeLibrary(module);
        }
#else
        (void)user;
        (void)flags;
        (void)buffer;
#endif
        return wc_return(cpu, result, ordinal == 4 ? 3 : 2);
    }
    if (import->by_ordinal)
        return 0;
    const char *name = import->name;
    if (equal_ascii_fold(import->dll, "WININET.dll") &&
        (!strcmp(name, "InternetGetCookieW") || !strcmp(name, "InternetSetCookieW"))) {
        char reason[256];
        snprintf(reason, sizeof(reason),
                 "offline WININET!%s: caller fallback has not been verified (caller=0x%08x)", name,
                 cpu->eip);
        stop_guest(reason, import->token);
        return 1;
    }
    if (equal_ascii_fold(import->dll, "WS2_32.dll")) {
        /* Byte-order helpers need no network connection or WSAStartup. */
        if (!strcmp(name, "htons") || !strcmp(name, "ntohs")) {
            uint32_t v = wc_arg(cpu, 1);
            return wc_return(cpu, ((v & 255) << 8) | ((v >> 8) & 255), 1);
        }
        if (!strcmp(name, "htonl") || !strcmp(name, "ntohl")) {
            uint32_t v = wc_arg(cpu, 1);
            return wc_return(
                cpu, ((v & 255) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24), 1);
        }
        if (!strcmp(name, "__WSAFDIsSet")) {
            uint32_t set = wc_arg(cpu, 2);
            wc_ptr(set, 4, import);
            uint32_t count = rd32(set);
            if (count > 64)
                stop_guest("__WSAFDIsSet fd_count exceeds guest FD_SETSIZE", import->token);
            wc_ptr(set, 4 + (uint64_t)count * 4, import);
            int found = 0;
            for (unsigned j = 0; j < count; ++j)
                if (rd32(set + 4 + j * 4) == wc_arg(cpu, 1))
                    found = 1;
            return wc_return(cpu, found, 2);
        }
        static const char *const offline[] = {
            "socket",      "select",        "ioctlsocket",    "connect",     "bind",
            "getsockname", "closesocket",   "WSAAsyncSelect", "setsockopt",  "sendto",
            "inet_ntoa",   "gethostbyname", "gethostbyaddr",  "getpeername", "send",
            "recvfrom",    "recv",          "listen",         "accept",      "gethostname",
            "getsockopt"};
        for (unsigned j = 0; j < sizeof(offline) / sizeof(offline[0]); ++j)
            if (!strcmp(name, offline[j])) {
                char reason[256];
                snprintf(reason, sizeof(reason),
                         "offline WS2_32!%s: caller fallback has not been verified (caller=0x%08x)",
                         name, cpu->eip);
                stop_guest(reason, import->token);
                return 1;
            }
        return 0; /* existing WSAStartup/cleanup/error/inet_addr */
    }
    if (equal_ascii_fold(import->dll, "WINMM.dll") && !strcmp(name, "waveOutGetDevCapsA")) {
        void *out = wc_arg(cpu, 3) ? wc_ptr(wc_arg(cpu, 2), wc_arg(cpu, 3), import) : NULL;
        uint32_t result = 2; /* MMSYSERR_BADDEVICEID */
#ifdef _WIN32
        HMODULE module = LoadLibraryExA("winmm.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            typedef MMRESULT(WINAPI * caps_fn)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
            FARPROC raw = GetProcAddress(module, "waveOutGetDevCapsA");
            caps_fn fn;
            memcpy(&fn, &raw, sizeof(fn));
            if (fn) {
                result = fn(wc_arg(cpu, 1) == UINT32_MAX ? (UINT_PTR)-1 : wc_arg(cpu, 1), out,
                            wc_arg(cpu, 3));
            }
            FreeLibrary(module);
        }
#else
        (void)out;
#endif
        return wc_return(cpu, result, 3);
    }
    if (equal_ascii_fold(import->dll, "ADVAPI32.dll")) {
        if (!strcmp(name, "GetUserNameA")) {
            uint32_t count = wc_arg(cpu, 2);
            wc_ptr(count, 4, import);
            uint32_t cap = rd32(count);
            wr32(count, 7);
            if (cap < 7) {
                last_error = 122;
                return wc_return(cpu, 0, 2);
            }
            memcpy(wc_ptr(wc_arg(cpu, 1), 7, import), "Player", 7);
            return wc_return(cpu, 1, 2);
        }
        /* RegOpenKeyExA exposes no registry handles: its only known key
         * returns FILE_NOT_FOUND. Never forward unknown handles to the host. */
        uint32_t key = wc_arg(cpu, 1);
        int predefined = key >= 0x80000000u && key <= 0x80000006u;
        if (!strcmp(name, "RegQueryValueExA"))
            return wc_return(cpu, predefined ? 2 : 6, 6);
        if (!strcmp(name, "RegCloseKey"))
            return wc_return(cpu, predefined ? 0 : 6, 1);
        return 0;
    }
    if (equal_ascii_fold(import->dll, "OLEAUT32.dll")) {
        if (!strcmp(name, "SysAllocString")) {
            uint32_t source = wc_arg(cpu, 1);
            if (!source)
                return wc_return(cpu, 0, 1);
            size_t n = wc_length(source, -1, 2, 0, import);
            uint32_t p = pcrecomp_heap_alloc(process_heap, (uint32_t)n * 2 + 6, 0);
            if (p) {
                wr32(p, (uint32_t)n * 2);
                memcpy(wc_ptr(p + 4, n * 2 + 2, import), wc_ptr(source, n * 2 + 2, import),
                       n * 2 + 2);
            }
            return wc_return(cpu, p ? p + 4 : 0, 1);
        }
        if (!strcmp(name, "SysFreeString")) {
            uint32_t p = wc_arg(cpu, 1);
            if (p)
                pcrecomp_heap_free(process_heap, p - 4);
            return wc_return(cpu, 0, 1);
        }
        return 0;
    }
    if (!equal_ascii_fold(import->dll, "KERNEL32.dll") &&
        !equal_ascii_fold(import->dll, "USER32.dll"))
        return 0;
    wc_state *s = wc_get_state();
    if (!s)
        stop_guest("Win32 compatibility state allocation failed", import->token);
    if (equal_ascii_fold(import->dll, "USER32.dll"))
        return wc_user_import(cpu, import, s);
    return wc_thread_alias(cpu, import, s) || wc_file_import(cpu, import, s) ||
           wc_time_import(cpu, import, s) || wc_locale_import(cpu, import) ||
           wc_misc_import(cpu, import, s);
}
