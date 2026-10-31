/* The executable's __emutls_get_address (src/runtime/ac1_emutls.c): it is the
 * one linked and reads the TEB, threads see their own zeroed or
 * template-initialized aligned copies, the array grows past its first
 * allocation, first use keeps the thread's LastError, and libstdc++'s own
 * thread-locals (emutls_once.cpp) still work. */
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

unsigned long ac1_emutls_fast_slot(void);
int emutls_call_once_check(void);

static _Thread_local int zero;
static _Thread_local int seeded = 1234;
static _Thread_local _Alignas(64) unsigned char aligned[100];
static _Thread_local uint32_t big[4096];
#define VARIABLES(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) \
    X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) \
    X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) \
    X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)
#define DEFINE(n) static _Thread_local unsigned v##n = n;
VARIABLES(DEFINE)

enum { THREADS = 16 };
static void *addresses[THREADS + 1];

static DWORD WINAPI exercise(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    SetLastError(0xBEEF0000u + id);
    int *first = &zero;
    assert(GetLastError() == 0xBEEF0000u + id);
    assert(*first == 0 && seeded == 1234 && !((uintptr_t)aligned & 63u));
    addresses[id] = first;
#define CHECK_INITIAL(n) assert(v##n == n);
    VARIABLES(CHECK_INITIAL)
    zero = (int)id;
    seeded = (int)id * 3;
    for (unsigned i = 0; i < 4096u; ++i) big[i] = id ^ i;
    for (unsigned i = 0; i < sizeof(aligned); ++i) aligned[i] = (unsigned char)(id + i);
#define SET(n) v##n = n + id * 100u;
    VARIABLES(SET)
    for (unsigned round = 0; round < 200u; ++round) {
        SwitchToThread();
        assert(zero == (int)id && seeded == (int)id * 3);
        for (unsigned i = 0; i < 4096u; i += 97u) assert(big[i] == (id ^ i));
        for (unsigned i = 0; i < sizeof(aligned); ++i) assert(aligned[i] == (unsigned char)(id + i));
#define CHECK_SET(n) assert(v##n == n + id * 100u);
        VARIABLES(CHECK_SET)
    }
    return 0;
}

int main(void)
{
    assert(ac1_emutls_fast_slot() == TLS_OUT_OF_INDEXES);
    HANDLE threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) {
        threads[i] = CreateThread(NULL, 0, exercise, (void *)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
    }
    exercise((void *)(uintptr_t)THREADS);
    assert(WaitForMultipleObjects(THREADS, threads, TRUE, 60000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < THREADS; ++i) {
        DWORD code = 1;
        assert(GetExitCodeThread(threads[i], &code) && code == 0);
        CloseHandle(threads[i]);
    }
    for (unsigned i = 0; i <= THREADS; ++i)
        for (unsigned j = 0; j < i; ++j) assert(addresses[i] != addresses[j]);
    assert(ac1_emutls_fast_slot() != TLS_OUT_OF_INDEXES);   /* this resolver, reading the TEB */
    assert(emutls_call_once_check());
    printf("emulated TLS: %u threads, 52 variables, isolation and LastError passed\n", THREADS + 1);
    return 0;
}
