/* Emulated TLS for the MinGW-w64 GCC host build without winpthreads.
 *
 * GCC for x86_64-w64-mingw32 has no native TLS: every _Thread_local or
 * thread_local access calls __emutls_get_address. libgcc reads the thread's
 * array through winpthreads' pthread_getspecific, which takes a spin lock and
 * saves and restores GetLastError around TlsGetValue; with the runtime's
 * thread-locals read several times per import (critical sections, clocks,
 * guest TLS) that was ~10% of the main thread in gameplay. This definition,
 * linked into the executable ahead of libgcc's, keeps libgcc's control-object
 * layout and semantics (indices assigned on first use, zeroed or
 * template-initialized aligned storage) but keeps the array pointer in a
 * TlsAlloc slot read straight from the TEB: no calls, locks or LastError
 * traffic once a variable exists on a thread.
 *
 * Control objects of other modules (libstdc++'s std::call_once state, used
 * from inline code here and from the DLL itself) stay with libgcc's resolver:
 * indices assigned here carry OWN_INDEX, so a foreign index never matches.
 *
 * Unlike libgcc, thread exit does not free the storage. AC1 runs at most 32
 * guest threads and a few host threads, and C++ thread_local destructors that
 * run at thread exit keep valid objects. */
#if defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libgcc's control object (libgcc/emutls.c), emitted as __emutls_v.<name>. */
struct __emutls_object {
    size_t size;
    size_t align;
    union {
        size_t offset;
        void *ptr;
    } loc;
    void *templ;
};

typedef struct emutls_array {
    size_t size;
    void *data[];
} emutls_array;

#define OWN_INDEX ((size_t)1 << 63)
/* TEB (x64): TlsSlots[64] at 0x1480, the TlsExpansionSlots pointer at 0x1780.
 * Verified against TlsGetValue when the slot is allocated. */
#define TEB_TLS_SLOTS 0x1480u
#define TEB_TLS_EXPANSION 0x1780u

extern IMAGE_DOS_HEADER __ImageBase;
static DWORD tls_slot = TLS_OUT_OF_INDEXES;
/* tls_slot once the TEB read is verified: the only state the fast path reads. */
static DWORD fast_slot = TLS_OUT_OF_INDEXES;
static INIT_ONCE emutls_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK emutls_lock = SRWLOCK_INIT;
static size_t emutls_count;
static void *(*libgcc_resolver)(void *);

static emutls_array *teb_array(DWORD slot)
{
    if (slot < 64u) return (emutls_array *)__readgsqword(TEB_TLS_SLOTS + 8u * slot);
    void **expansion = (void **)__readgsqword(TEB_TLS_EXPANSION);
    return expansion ? (emutls_array *)expansion[slot - 64u] : NULL;
}

static BOOL CALLBACK emutls_init(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) {
        fprintf(stderr, "AC1 emulated TLS: TlsAlloc failed (%lu)\n", GetLastError());
        abort();
    }
    /* Probe the TEB layout once; on a mismatch every access takes TlsGetValue. */
    static emutls_array probe;
    TlsSetValue(slot, &probe);
    int teb_reads = teb_array(slot) == &probe;
    TlsSetValue(slot, NULL);
    tls_slot = slot;
    if (teb_reads) __atomic_store_n(&fast_slot, slot, __ATOMIC_RELEASE);
    return TRUE;
}

static int own_object(const void *object)
{
    const unsigned char *base = (const unsigned char *)&__ImageBase;
    const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);
    return (const unsigned char *)object >= base &&
           (const unsigned char *)object < base + headers->OptionalHeader.SizeOfImage;
}

static void *foreign_address(struct __emutls_object *object)
{
    void *(*resolver)(void *) = __atomic_load_n(&libgcc_resolver, __ATOMIC_ACQUIRE);
    if (!resolver) {
        HMODULE libgcc = GetModuleHandleA("libgcc_s_seh-1.dll");
        resolver = libgcc ? (void *(*)(void *))(void *)GetProcAddress(libgcc, "__emutls_get_address") : NULL;
        if (!resolver) {
            fprintf(stderr, "AC1 emulated TLS: variable at %p belongs to another module, "
                            "but libgcc_s_seh-1.dll's resolver is not loaded\n", (void *)object);
            abort();
        }
        __atomic_store_n(&libgcc_resolver, resolver, __ATOMIC_RELEASE);
    }
    return resolver(object);
}

static void *emutls_alloc(const struct __emutls_object *object)
{
    size_t align = object->align > sizeof(void *) ? object->align : sizeof(void *);
    char *raw = malloc(object->size + align - 1u + sizeof(void *));
    if (!raw) {
        fprintf(stderr, "AC1 emulated TLS: out of memory for %zu bytes\n", object->size);
        abort();
    }
    void **data = (void **)(((uintptr_t)raw + sizeof(void *) + align - 1u) & ~(uintptr_t)(align - 1u));
    data[-1] = raw;
    if (object->templ) memcpy(data, object->templ, object->size);
    else memset(data, 0, object->size);
    return data;
}

static __attribute__((noinline)) void *emutls_slow(struct __emutls_object *object)
{
    size_t index = __atomic_load_n(&object->loc.offset, __ATOMIC_ACQUIRE);
    if (!(index & OWN_INDEX) && (index || !own_object(object))) return foreign_address(object);
    DWORD error = GetLastError();
    InitOnceExecuteOnce(&emutls_once, emutls_init, NULL, NULL);
    if (!index) {
        AcquireSRWLockExclusive(&emutls_lock);
        index = object->loc.offset;
        if (!index) {
            index = ++emutls_count | OWN_INDEX;
            __atomic_store_n(&object->loc.offset, index, __ATOMIC_RELEASE);
        }
        ReleaseSRWLockExclusive(&emutls_lock);
    }
    index &= ~OWN_INDEX;
    emutls_array *array = (emutls_array *)TlsGetValue(tls_slot);
    if (!array || array->size < index) {
        size_t old = array ? array->size : 0u, size = index + 32u;
        emutls_array *grown = (emutls_array *)realloc(array, sizeof(*grown) + size * sizeof(void *));
        if (!grown) {
            fprintf(stderr, "AC1 emulated TLS: out of memory for %zu slots\n", size);
            abort();
        }
        memset(grown->data + old, 0, (size - old) * sizeof(void *));
        grown->size = size;
        array = grown;
        TlsSetValue(tls_slot, array);
    }
    void *data = array->data[index - 1u];
    if (!data) data = array->data[index - 1u] = emutls_alloc(object);
    SetLastError(error);
    return data;
}

/* For tests: the slot the fast path reads, TLS_OUT_OF_INDEXES before first use. */
unsigned long ac1_emutls_fast_slot(void)
{
    return __atomic_load_n(&fast_slot, __ATOMIC_ACQUIRE);
}

void *__emutls_get_address(void *control)
{
    struct __emutls_object *object = (struct __emutls_object *)control;
    size_t index = __atomic_load_n(&object->loc.offset, __ATOMIC_ACQUIRE);
    DWORD slot = __atomic_load_n(&fast_slot, __ATOMIC_ACQUIRE);
    if (__builtin_expect((index & OWN_INDEX) && slot != TLS_OUT_OF_INDEXES, 1)) {
        const emutls_array *array = teb_array(slot);
        index &= ~OWN_INDEX;
        if (__builtin_expect(array && index <= array->size, 1)) {
            void *data = array->data[index - 1u];
            if (__builtin_expect(data != NULL, 1)) return data;
        }
    }
    return emutls_slow(object);
}
#endif
