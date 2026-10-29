#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ac1_runtime.h"
#include "ac1_virtual_query.h"
#include "ac1_profile_int.h"
#include "cpu.h"
#include "guest_memory.h"
#include "pe32_loader.h"
#include "pcrecomp_heap.h"
#include "ac1_network.h"
#include "ac1_crypto.h"
#include "ac1_files.h"
#include "initial_dispatch.h"
#include "bink_dispatch.h"
#include "bink_module.h"
#include "ac1_fingerprint.h"
#include "../platform/ac1_audio_win32.h"
#if defined(AC1_HOST_D3D9)
#include "../graphics/ac1_d3d9_bridge.h"
#if defined(__ANDROID__)
extern void *ac1_android_d3d9_factory(void *context, uint32_t sdk_version);
extern uintptr_t ac1_android_d3d9_guest_hwnd(void *context, uint32_t guest_hwnd);
#endif
#endif

#include <setjmp.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include "../platform/ac1_window_win32.h"
#include "../platform/ac1_ds8_bridge.h"
#include "../platform/ac1_dinput8_bridge.h"
#else
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sched.h>
#endif

#define AC1_STACK_BASE 0x7FE00000u
#define AC1_STACK_SIZE 0x00100000u
#define AC1_TIB_BASE 0x7FFD0000u
#define AC1_TIB_SIZE 0x00001000u
#define AC1_IMPORT_BASE 0xF1000000u
#define AC1_RETURN_SENTINEL 0xFFFFF000u
#define AC1_DISPATCH_LIMIT_DEFAULT UINT64_C(200000000)
#define AC1_DISPATCH_DEPTH 4096u
#define AC1_IMPORT_LIMIT 2048u
#define AC1_PROCESS_HEAP_HANDLE 0xE0000001u
#define AC1_PRIVATE_HEAP_BASE 0xE0000100u
#define AC1_MAX_PRIVATE_HEAPS 32u
#define AC1_KERNEL32_HANDLE 0x76000000u
#define AC1_STD_INPUT_HANDLE 0xE1000001u
#define AC1_STD_OUTPUT_HANDLE 0xE1000002u
#define AC1_STD_ERROR_HANDLE 0xE1000003u
#define AC1_TLS_SLOTS 1088u
#define AC1_SYNC_HANDLE_BASE 0xE2000000u
#define AC1_MAX_SYNC_OBJECTS 64u
#define AC1_THREAD_HANDLE_BASE 0xE3000000u
#define AC1_MAX_THREADS 32u
#define AC1_WAIT_OBJECT_0 0u
#define AC1_WAIT_TIMEOUT 258u
#define AC1_STILL_ACTIVE 259u
#define AC1_WAIT_CRITICAL_SECTION 3

_Thread_local uint32_t g_image_delta;

typedef struct ac1_guest_thread ac1_guest_thread;
typedef struct ac1_scheduler ac1_scheduler;
static _Thread_local ac1_guest_thread *active_guest_thread;

struct ac1_guest_thread {
    ac1_scheduler *scheduler;
    unsigned slot;
    uint32_t id, handle, start, parameter, exit_code, image_delta;
    uint64_t guest_dispatches;
    uint32_t stack_base, stack_size, tib_base;
    uint32_t static_tls_data, static_tls_vector;
    uint32_t affinity_mask;
    uint32_t tls_values[AC1_TLS_SLOTS];
    CPU cpu;
#ifdef _WIN32
    HANDLE host;
#else
    pthread_t host;
#endif
    jmp_buf exit_env;
    int spawned, started, finished, closed, suspended, blocked;
    int wait_kind;
    int priority;
    int priority_boost_disabled;
    uint32_t wait_handle, wait_result;
    uint64_t wait_deadline;
    uint64_t return_slice_deadline;
    unsigned return_slice_checks;
};

struct ac1_scheduler {
#ifdef _WIN32
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    guest_address_space *space;
    ac1_guest_thread threads[AC1_MAX_THREADS];
    unsigned count, owner;
    uint32_t next_thread_id;
    int stopping, failed;
    uint32_t failure_target;
    uint32_t failure_caller, failure_thread_id;
    CPU failure_cpu;
    uint32_t failure_stack[8];
    uint32_t failure_object_words[8];
    uint32_t failure_vtable_words[8];
    int failure_object_readable;
    int failure_vtable_readable;
    char failure_reason[512];
};

static _Thread_local jmp_buf stop_runtime;
static _Thread_local int runtime_stop_ready;
static int trace_imports;
static int trace_object_types;
static int object_type_registry_traced;
static unsigned object_type_parent_records;
static unsigned object_type_descriptor_count;
static int crt_handoff_logged;
static _Thread_local unsigned trace_import_count;
static _Thread_local unsigned trace_callback_count;
static _Thread_local unsigned trace_file_read_count;
static _Thread_local uint32_t last_guest_target;
static _Thread_local uint64_t dispatch_count;
static uint64_t dispatch_limit = AC1_DISPATCH_LIMIT_DEFAULT;
static _Thread_local unsigned dispatch_depth;
static uint64_t scheduler_return_slice_ns = UINT64_C(1000000);
static pcrecomp_heap *process_heap;
static ac1_files *guest_files;
static char exe_path_copy[32768];
static char guest_current_directory[32768];
#ifdef __ANDROID__
/* POSIX path used only by the host filesystem. Guest APIs see the virtual
 * Windows install root in guest_current_directory/exe_path_copy. */
static char host_game_directory[32768];
#endif
static uint32_t command_line_guest_address;
static struct { uint32_t handle; pcrecomp_heap *heap; } private_heaps[AC1_MAX_PRIVATE_HEAPS];
static _Thread_local uint32_t last_error;
static uint32_t max_guest_handles;
static uint32_t unhandled_exception_filter;
static char user_data_root[32768];
static _Thread_local uint32_t com_init_count;
static _Thread_local uint32_t com_apartment_model;
static uint32_t guest_affinity_mask;
static _Thread_local uint32_t guest_thread_affinity_mask;
static uint32_t image_base;
static uint32_t image_size_loaded;
static pe32_module bink_image;
static int bink_image_mapped;
static char bink_module_path[32768];
#ifdef __ANDROID__
static char bink_host_path[32768];
#endif
#ifdef _WIN32
static ac1_window_win32 *window_bridge;
static int ds8_configured;
static int dinput8_configured;
typedef struct { uintptr_t native; uint32_t guest; } ac1_guest_monitor;
static ac1_guest_monitor *guest_monitors;
static size_t guest_monitor_count;
static size_t guest_monitor_capacity;
static uint32_t next_guest_monitor = 0xE6000004u;
static SRWLOCK guest_monitor_lock = SRWLOCK_INIT;
#endif
#if defined(AC1_HOST_D3D9)
typedef struct { uint32_t guest_address, heap_address; } ac1_d3d9_heap_block;
static ac1_d3d9_heap_block *d3d9_heap_blocks;
static size_t d3d9_heap_block_count;
static size_t d3d9_heap_block_capacity;
static int d3d9_configured;
#endif
static uint8_t tls_used[AC1_TLS_SLOTS];
static _Thread_local uint32_t tls_values[AC1_TLS_SLOTS];
static _Thread_local uint32_t static_tls_data_base;
static _Thread_local uint32_t static_tls_vector_base;
static const uint8_t *static_tls_template;
static uint32_t static_tls_template_size;
static uint32_t static_tls_zero_fill;
static uint32_t static_tls_index_address;
static uint32_t virtual_alloc_bases[GUEST_MEMORY_MAX_REGIONS];
static ac1_scheduler *active_scheduler;

static int virtual_allocation_contains(uint32_t base)
{
    for (size_t i = 0; i < GUEST_MEMORY_MAX_REGIONS; ++i)
        if (virtual_alloc_bases[i] == base) return 1;
    return 0;
}

static int virtual_allocation_add(uint32_t base)
{
    for (size_t i = 0; i < GUEST_MEMORY_MAX_REGIONS; ++i) {
        if (virtual_alloc_bases[i] == base) return 1;
        if (!virtual_alloc_bases[i]) {
            virtual_alloc_bases[i] = base;
            return 1;
        }
    }
    return 0;
}

static void virtual_allocation_remove(uint32_t base)
{
    for (size_t i = 0; i < GUEST_MEMORY_MAX_REGIONS; ++i)
        if (virtual_alloc_bases[i] == base) {
            virtual_alloc_bases[i] = 0u;
            return;
        }
}

static void scheduler_lock(ac1_scheduler *scheduler);
static void scheduler_unlock(ac1_scheduler *scheduler);
static void scheduler_notify(ac1_scheduler *scheduler);
static void scheduler_wait_condition(ac1_scheduler *scheduler, uint32_t milliseconds);
static void scheduler_destroy_primitives(ac1_scheduler *scheduler);
static unsigned scheduler_next_runnable(ac1_scheduler *scheduler, unsigned after);
static ac1_guest_thread *thread_for_handle(uint32_t handle);
static int equal_ascii_fold(const char *a, const char *b);
static void return_stdcall(CPU *cpu, uint32_t argument_bytes);

#ifdef _WIN32
static CRITICAL_SECTION ac1_run_lock;
static INIT_ONCE ac1_run_lock_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_ac1_run_lock(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&ac1_run_lock);
    return TRUE;
}
static void lock_ac1_run(void) { InitOnceExecuteOnce(&ac1_run_lock_once, init_ac1_run_lock, NULL, NULL); EnterCriticalSection(&ac1_run_lock); }
static void unlock_ac1_run(void) { LeaveCriticalSection(&ac1_run_lock); }
#else
static pthread_mutex_t ac1_run_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_ac1_run(void) { pthread_mutex_lock(&ac1_run_lock); }
static void unlock_ac1_run(void) { pthread_mutex_unlock(&ac1_run_lock); }
#endif

typedef struct {
    uint32_t token;
    char dll[96];
    char name[160];
    uint16_t ordinal;
    int by_ordinal;
} import_record;

typedef struct {
    uint32_t handle;
    uint32_t count;
    uint32_t maximum;
    uint8_t kind; /* 1 event, 2 semaphore */
    uint8_t manual_reset;
    uint8_t signaled;
    uint8_t io_backed;
#ifdef _WIN32
    HANDLE host_event;
#endif
    char name[256];
} sync_object;

static import_record imports[AC1_IMPORT_LIMIT];
static sync_object sync_objects[AC1_MAX_SYNC_OBJECTS];
static size_t import_count;
static _Thread_local char stop_reason[512];
static _Thread_local char last_run_error[512];
static void stop_guest(const char *reason, uint32_t target);
static int invoke_guest_stdcall_callback(CPU *cpu, uint32_t procedure,
                                         const uint32_t *arguments,
                                         uint32_t argument_count,
                                         uint32_t *result);

static int prepare_guest_thread(ac1_guest_thread *thread)
{
    thread->stack_base = guest_memory_find_gap(0x60000000u, thread->stack_size,
                                                0x10000u, 0x70000000u);
    thread->tib_base = guest_memory_find_gap(0x7F000000u, AC1_TIB_SIZE,
                                              0x10000u, 0x7FF00000u);
    if (!thread->stack_base || !thread->tib_base ||
        !guest_memory_allocate(thread->stack_base, thread->stack_size) ||
        !guest_memory_allocate(thread->tib_base, AC1_TIB_SIZE))
        return 0;

    uint32_t stack_top = thread->stack_base + thread->stack_size - 0x100u;
    thread->cpu.esp = stack_top - 8u;
    thread->cpu.eip = thread->start;
    thread->cpu.eflags_misc = 2u;
    thread->cpu.fs_base = thread->tib_base;
    wr32(thread->cpu.esp, AC1_RETURN_SENTINEL);
    wr32(thread->cpu.esp + 4u, thread->parameter);
    wr32(thread->tib_base, UINT32_MAX);
    wr32(thread->tib_base + 4u, stack_top);
    wr32(thread->tib_base + 8u, thread->stack_base);
    wr32(thread->tib_base + 0x18u, thread->tib_base);
    wr32(thread->tib_base + 0x20u, 1u);
    wr32(thread->tib_base + 0x24u, thread->id);

    if (static_tls_index_address) {
        uint32_t total = static_tls_template_size + static_tls_zero_fill;
        if (!total) total = 1u;
        thread->static_tls_data = guest_memory_find_gap(0x70000000u, total, 16u,
                                                         AC1_STACK_BASE);
        thread->static_tls_vector = guest_memory_find_gap(0x7FC00000u,
                                                          64u * 4u, 16u,
                                                          AC1_TIB_BASE);
        if (!thread->static_tls_data || !thread->static_tls_vector ||
            !guest_memory_allocate(thread->static_tls_data, total) ||
            !guest_memory_allocate(thread->static_tls_vector, 64u * 4u))
            return 0;
        memset(guest_ptr(thread->static_tls_data, total), 0, total);
        if (static_tls_template_size)
            memcpy(guest_ptr(thread->static_tls_data, static_tls_template_size),
                   static_tls_template, static_tls_template_size);
        wr32(static_tls_index_address, 0u);
        wr32(thread->static_tls_vector, thread->static_tls_data);
        wr32(thread->tib_base + 0x2cu, thread->static_tls_vector);
        thread->tls_values[0] = thread->static_tls_data;
    }
    return 1;
}

static void run_guest_thread(ac1_guest_thread *thread)
{
    ac1_scheduler *scheduler = thread->scheduler;
    scheduler_lock(scheduler);
    while (!scheduler->stopping && !scheduler->failed && scheduler->owner != thread->slot)
        scheduler_wait_condition(scheduler, 0u);
    int can_run = !scheduler->stopping && !scheduler->failed;
    thread->started = can_run;
    scheduler_unlock(scheduler);

    if (can_run) {
        /* Read mutable per-process/thread state only after receiving the guest
         * baton. The primary thread can still service imports while this worker
         * is waiting, including SetThreadAffinityMask. */
        active_guest_thread = thread;
        thread->return_slice_deadline = scheduler_return_slice_ns
            ? guest_monotonic_ns() + scheduler_return_slice_ns : 0u;
        thread->return_slice_checks = 0u;
        guest_address_space_select(scheduler->space);
        static_tls_data_base = thread->static_tls_data;
        static_tls_vector_base = thread->static_tls_vector;
        guest_thread_affinity_mask = thread->affinity_mask;
        memcpy(tls_values, thread->tls_values, sizeof(tls_values));
        last_error = 0u;
        com_init_count = 0u;
        com_apartment_model = UINT32_MAX;
        g_image_delta = thread->image_delta;
        dispatch_count = 0u;
        dispatch_depth = 0u;
        trace_import_count = 0u;
        last_guest_target = thread->start;
    }

    if (can_run && setjmp(thread->exit_env) == 0) {
        if (trace_imports)
            fprintf(stderr, "AC1 guest worker start id=%u start=0x%08x parameter=0x%08x stack=0x%08x tib=0x%08x\n",
                    thread->id, thread->start, thread->parameter,
                    thread->stack_base, thread->tib_base);
        if (setjmp(stop_runtime) == 0) {
            runtime_stop_ready = 1;
            dispatch(&thread->cpu, thread->start);
            runtime_stop_ready = 0;
            thread->exit_code = thread->cpu.eax;
        } else {
            runtime_stop_ready = 0;
        }
    }
    runtime_stop_ready = 0;

    scheduler_lock(scheduler);
    thread->guest_dispatches = dispatch_count;
    thread->finished = 1;
    if (scheduler->failed) scheduler->owner = 0u;
    else if (scheduler->owner == thread->slot) {
        unsigned next = scheduler_next_runnable(scheduler, thread->slot);
        scheduler->owner = next < scheduler->count ? next : 0u;
    }
    int worker_failed = scheduler->failed;
    scheduler_notify(scheduler);
    scheduler_unlock(scheduler);
    if (thread->started && trace_imports)
        fprintf(stderr, "AC1 guest worker end id=%u exit=0x%08x failed=%d\n",
                thread->id, thread->exit_code, worker_failed);
    active_guest_thread = NULL;
    guest_address_space_select(NULL);
}

#ifdef _WIN32
static DWORD WINAPI guest_thread_entry(LPVOID parameter)
{
    run_guest_thread((ac1_guest_thread *)parameter);
    return 0u;
}
#else
static void *guest_thread_entry(void *parameter)
{
    run_guest_thread((ac1_guest_thread *)parameter);
    return NULL;
}
#endif

static void scheduler_stop_and_join(ac1_scheduler *scheduler)
{
    if (!scheduler) return;
    scheduler_lock(scheduler);
    unsigned created = 0u, started = 0u, exited_before_teardown = 0u, blocked_at_teardown = 0u;
    for (unsigned i = 1u; i < scheduler->count; ++i) {
        ac1_guest_thread *thread = &scheduler->threads[i];
        if (thread->spawned) ++created;
        if (thread->started) ++started;
        if (thread->finished) ++exited_before_teardown;
        if (thread->blocked) ++blocked_at_teardown;
    }
    scheduler->stopping = 1;
    scheduler_notify(scheduler);
    scheduler_unlock(scheduler);
    guest_address_space_select(scheduler->space);
    for (unsigned i = 1u; i < scheduler->count; ++i) {
        ac1_guest_thread *thread = &scheduler->threads[i];
        if (thread->spawned) {
#ifdef _WIN32
            WaitForSingleObject(thread->host, INFINITE);
            CloseHandle(thread->host);
#else
            pthread_join(thread->host, NULL);
#endif
        }
    }
    /* Native OVERLAPPED reads may still target guest buffers. Drain them only
     * after guest workers have stopped, but before releasing any guest stack,
     * TLS, or heap-backed mappings. */
    ac1_files_cancel_pending(guest_files);
    for (unsigned i = 1u; i < scheduler->count; ++i) {
        ac1_guest_thread *thread = &scheduler->threads[i];
        if (thread->static_tls_vector) guest_memory_release(thread->static_tls_vector);
        if (thread->static_tls_data) guest_memory_release(thread->static_tls_data);
        if (thread->tib_base) guest_memory_release(thread->tib_base);
        if (thread->stack_base) guest_memory_release(thread->stack_base);
    }
    unsigned joined = 0u;
    uint64_t worker_dispatches = 0u;
    for (unsigned i = 1u; i < scheduler->count; ++i) {
        ac1_guest_thread *thread = &scheduler->threads[i];
        if (thread->spawned) ++joined;
        worker_dispatches += thread->guest_dispatches;
    }
    fprintf(stderr, "AC1 scheduler summary: workers created=%u started=%u exited-before-teardown=%u joined=%u blocked-at-teardown=%u guest-dispatches=%llu\n",
            created, started, exited_before_teardown, joined, blocked_at_teardown,
            (unsigned long long)worker_dispatches);
    active_guest_thread = NULL;
    active_scheduler = NULL;
    scheduler_destroy_primitives(scheduler);
}

static int create_guest_thread(CPU *cpu, const import_record *import)
{
    if (!active_scheduler || !active_guest_thread) return 0;
    uint32_t attributes = rd32(cpu->esp + 4u);
    uint32_t requested_stack = rd32(cpu->esp + 8u);
    uint32_t start = rd32(cpu->esp + 12u);
    uint32_t parameter = rd32(cpu->esp + 16u);
    uint32_t flags = rd32(cpu->esp + 20u);
    uint32_t thread_id_output = rd32(cpu->esp + 24u);
    if (attributes || !start || (flags & ~(4u | 0x10000u)) ||
        active_scheduler->count >= AC1_MAX_THREADS) {
        cpu->eax = 0u;
        last_error = attributes ? 87u : 8u;
        return_stdcall(cpu, 24u);
        return 1;
    }
    if (thread_id_output && !guest_memory_try_ptr(thread_id_output, 4u))
        stop_guest("CreateThread thread-id output is unmapped", import->token);
    uint32_t stack_size = requested_stack ? requested_stack : 0x00100000u;
    if (stack_size > 0x01000000u) {
        cpu->eax = 0u;
        last_error = 8u;
        return_stdcall(cpu, 24u);
        return 1;
    }
    stack_size = (stack_size + 0xffffu) & ~0xffffu;
    if (!stack_size) stack_size = 0x00100000u;

    ac1_scheduler *scheduler = active_scheduler;
    unsigned slot = scheduler->count;
    ac1_guest_thread *thread = &scheduler->threads[slot];
    memset(thread, 0, sizeof(*thread));
    thread->scheduler = scheduler;
    thread->slot = slot;
    thread->id = scheduler->next_thread_id;
    scheduler->next_thread_id += 4u;
    thread->handle = AC1_THREAD_HANDLE_BASE + slot * 4u;
    thread->start = start;
    thread->parameter = parameter;
    thread->stack_size = stack_size;
    thread->image_delta = g_image_delta;
    thread->affinity_mask = guest_affinity_mask;
    thread->suspended = (flags & 4u) != 0u;
    if (!prepare_guest_thread(thread)) {
        if (thread->static_tls_vector) guest_memory_release(thread->static_tls_vector);
        if (thread->static_tls_data) guest_memory_release(thread->static_tls_data);
        if (thread->tib_base) guest_memory_release(thread->tib_base);
        if (thread->stack_base) guest_memory_release(thread->stack_base);
        memset(thread, 0, sizeof(*thread));
        cpu->eax = 0u;
        last_error = 8u;
        return_stdcall(cpu, 24u);
        return 1;
    }

    scheduler_lock(scheduler);
#ifdef _WIN32
    thread->host = CreateThread(NULL, 0, guest_thread_entry, thread, 0, NULL);
    thread->spawned = thread->host != NULL;
#else
    thread->spawned = pthread_create(&thread->host, NULL, guest_thread_entry, thread) == 0;
#endif
    if (thread->spawned) ++scheduler->count;
    scheduler_notify(scheduler);
    scheduler_unlock(scheduler);
    if (!thread->spawned) {
        if (thread->static_tls_vector) guest_memory_release(thread->static_tls_vector);
        if (thread->static_tls_data) guest_memory_release(thread->static_tls_data);
        if (thread->tib_base) guest_memory_release(thread->tib_base);
        if (thread->stack_base) guest_memory_release(thread->stack_base);
        memset(thread, 0, sizeof(*thread));
        cpu->eax = 0u;
        last_error = 8u;
        return_stdcall(cpu, 24u);
        return 1;
    }
    if (thread_id_output) wr32(thread_id_output, thread->id);
    cpu->eax = thread->handle;
    last_error = 0u;
    return_stdcall(cpu, 24u);
    return 1;
}

static int handle_thread_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    if (!strcmp(import->name, "CreateThread")) return create_guest_thread(cpu, import);
    if (!strcmp(import->name, "Sleep")) {
        uint32_t milliseconds = rd32(cpu->esp + 4u);
        if (active_guest_thread && active_scheduler && milliseconds) {
            active_guest_thread->blocked = 1;
            active_guest_thread->wait_kind = 2;
            active_guest_thread->wait_deadline = milliseconds == UINT32_MAX ? 0u :
                guest_monotonic_ns() + (uint64_t)milliseconds * 1000000u;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(import->name, "SwitchToThread")) {
        int another = active_scheduler && active_scheduler->count > 1u;
        cpu->eax = another ? 1u : 0u;
        return_stdcall(cpu, 0u);
        return 1;
    }
    if (!strcmp(import->name, "ResumeThread") || !strcmp(import->name, "SuspendThread")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        ac1_guest_thread *thread = thread_for_handle(handle);
        if (!thread) {
            cpu->eax = UINT32_MAX;
            last_error = 6u;
        } else {
            cpu->eax = thread->suspended ? 1u : 0u;
            if (!strcmp(import->name, "ResumeThread")) thread->suspended = 0;
            else thread->suspended = 1;
            if (active_scheduler) {
                scheduler_lock(active_scheduler);
                scheduler_notify(active_scheduler);
                scheduler_unlock(active_scheduler);
            }
            last_error = 0u;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(import->name, "GetExitCodeThread")) {
        uint32_t output = rd32(cpu->esp + 8u);
        ac1_guest_thread *thread = thread_for_handle(rd32(cpu->esp + 4u));
        if (!thread || !guest_memory_try_ptr(output, 4u)) {
            cpu->eax = 0u;
            last_error = thread ? 87u : 6u;
        } else {
            wr32(output, thread->finished ? thread->exit_code : AC1_STILL_ACTIVE);
            cpu->eax = 1u;
            last_error = 0u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(import->name, "GetThreadId")) {
        ac1_guest_thread *thread = thread_for_handle(rd32(cpu->esp + 4u));
        cpu->eax = thread ? thread->id : 0u;
        last_error = thread ? 0u : 6u;
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(import->name, "SetThreadPriority")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        int32_t priority = (int32_t)rd32(cpu->esp + 8u);
        ac1_guest_thread *thread = thread_for_handle(handle);
        if (handle == UINT32_MAX - 1u) thread = active_guest_thread;
        if (!thread || priority < -15 || priority > 15) {
            cpu->eax = 0u;
            last_error = thread ? 87u : 6u;
        } else {
            thread->priority = priority;
            cpu->eax = 1u;
            last_error = 0u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(import->name, "GetThreadPriority")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        ac1_guest_thread *thread = thread_for_handle(handle);
        if (handle == UINT32_MAX - 1u) thread = active_guest_thread;
        cpu->eax = thread ? (uint32_t)thread->priority : UINT32_C(0x7fffffff);
        last_error = thread ? 0u : 6u;
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(import->name, "SetThreadPriorityBoost")) {
        ac1_guest_thread *thread = thread_for_handle(rd32(cpu->esp + 4u));
        if (!thread && rd32(cpu->esp + 4u) == UINT32_MAX - 1u)
            thread = active_guest_thread;
        if (!thread) {
            cpu->eax = 0u;
            last_error = 6u;
        } else {
            thread->priority_boost_disabled = rd32(cpu->esp + 8u) != 0u;
            cpu->eax = 1u;
            last_error = 0u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(import->name, "ExitThread")) {
        if (!active_guest_thread || active_guest_thread->slot == 0u)
            stop_guest("ExitThread on the primary guest thread is unsupported", import->token);
        active_guest_thread->exit_code = rd32(cpu->esp + 4u);
        longjmp(active_guest_thread->exit_env, 1);
    }
    return 0;
}

static void scheduler_lock(ac1_scheduler *scheduler)
{
#ifdef _WIN32
    EnterCriticalSection(&scheduler->mutex);
#else
    pthread_mutex_lock(&scheduler->mutex);
#endif
}

static void scheduler_unlock(ac1_scheduler *scheduler)
{
#ifdef _WIN32
    LeaveCriticalSection(&scheduler->mutex);
#else
    pthread_mutex_unlock(&scheduler->mutex);
#endif
}

static void scheduler_notify(ac1_scheduler *scheduler)
{
#ifdef _WIN32
    WakeAllConditionVariable(&scheduler->condition);
#else
    pthread_cond_broadcast(&scheduler->condition);
#endif
}

static void scheduler_wait_condition(ac1_scheduler *scheduler, uint32_t milliseconds)
{
#ifdef _WIN32
    SleepConditionVariableCS(&scheduler->condition, &scheduler->mutex,
                             milliseconds ? milliseconds : INFINITE);
#else
    if (!milliseconds) {
        pthread_cond_wait(&scheduler->condition, &scheduler->mutex);
    } else {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        uint64_t ns = (uint64_t)until.tv_nsec + (uint64_t)milliseconds * 1000000u;
        until.tv_sec += (time_t)(ns / 1000000000u);
        until.tv_nsec = (long)(ns % 1000000000u);
        (void)pthread_cond_timedwait(&scheduler->condition, &scheduler->mutex, &until);
    }
#endif
}

static void scheduler_initialize(ac1_scheduler *scheduler, guest_address_space *space)
{
    memset(scheduler, 0, sizeof(*scheduler));
#ifdef _WIN32
    InitializeCriticalSection(&scheduler->mutex);
    InitializeConditionVariable(&scheduler->condition);
#else
    pthread_mutex_init(&scheduler->mutex, NULL);
    pthread_cond_init(&scheduler->condition, NULL);
#endif
    scheduler->space = space;
    scheduler->count = 1u;
    scheduler->owner = 0u;
    scheduler->next_thread_id = 0x1000u;
}

static void scheduler_destroy_primitives(ac1_scheduler *scheduler)
{
#ifdef _WIN32
    DeleteCriticalSection(&scheduler->mutex);
#else
    pthread_cond_destroy(&scheduler->condition);
    pthread_mutex_destroy(&scheduler->mutex);
#endif
}

static ac1_guest_thread *thread_for_handle(uint32_t handle)
{
    if (!active_scheduler || handle < AC1_THREAD_HANDLE_BASE || (handle & 3u)) return NULL;
    unsigned slot = (handle - AC1_THREAD_HANDLE_BASE) / 4u;
    if (slot >= active_scheduler->count) return NULL;
    ac1_guest_thread *thread = &active_scheduler->threads[slot];
    return thread->handle == handle && !thread->closed ? thread : NULL;
}

static void scheduler_refresh_waits(ac1_scheduler *scheduler);
static uint32_t scheduler_next_timeout(ac1_scheduler *scheduler);

static unsigned scheduler_next_runnable(ac1_scheduler *scheduler, unsigned after)
{
    scheduler_refresh_waits(scheduler);
    int best_priority = INT32_MIN;
    unsigned best = scheduler->count;
    for (unsigned step = 1; step <= scheduler->count; ++step) {
        unsigned slot = (after + step) % scheduler->count;
        ac1_guest_thread *thread = &scheduler->threads[slot];
        if (!thread->finished && !thread->suspended && !thread->blocked &&
            thread->priority > best_priority) {
            best = slot;
            best_priority = thread->priority;
        }
    }
    return best;
}

static void scheduler_checkpoint(void)
{
    ac1_scheduler *scheduler = active_scheduler;
    ac1_guest_thread *thread = active_guest_thread;
    if (!scheduler || !thread || scheduler->count < 2u) return;
    scheduler_lock(scheduler);
    for (;;) {
        if (scheduler->failed || scheduler->stopping) break;
        unsigned next = scheduler_next_runnable(scheduler, thread->slot);
        if (next == thread->slot) break;
        if (next < scheduler->count) {
            scheduler->owner = next;
            scheduler_notify(scheduler);
            while (!scheduler->stopping && !scheduler->failed &&
                   scheduler->owner != thread->slot)
                scheduler_wait_condition(scheduler, 0u);
            /* Once this thread gets the baton back, let it execute. A regular
             * checkpoint must not immediately hand it to the same peer again. */
            scheduler_refresh_waits(scheduler);
            if (!thread->blocked) {
                if (thread->wait_kind == AC1_WAIT_CRITICAL_SECTION) {
                    if (trace_imports)
                        fprintf(stderr,
                                "AC1 CS waiter resumed: thread=%u section=0x%08x owner=%u lockcount=%u\n",
                                thread->id, thread->wait_handle,
                                rd32(thread->wait_handle + 12u),
                                rd32(thread->wait_handle + 4u));
                    thread->wait_kind = 0;
                    thread->wait_handle = 0u;
                }
                break;
            }
        } else {
            scheduler_wait_condition(scheduler, scheduler_next_timeout(scheduler));
        }
    }
    int failed = scheduler->failed;
    int stopping = scheduler->stopping;
    uint32_t target = scheduler->failure_target;
    char failure_reason[sizeof(scheduler->failure_reason)];
    snprintf(failure_reason, sizeof(failure_reason), "%s", scheduler->failure_reason);
    scheduler_unlock(scheduler);
    if (scheduler_return_slice_ns) {
        thread->return_slice_deadline = guest_monotonic_ns() + scheduler_return_slice_ns;
        thread->return_slice_checks = 0u;
    }
    if ((stopping || failed) && thread->slot != 0u)
        longjmp(thread->exit_env, 1);
    if (failed) stop_guest(failure_reason[0] ? failure_reason : "guest worker failed", target);
}

/* Generated-function returns are cooperative yield opportunities, not blocking
 * API boundaries. Amortize their scheduler scan while keeping API checkpoints
 * above immediate. A zero slice setting preserves the legacy every-return path. */
static void scheduler_return_checkpoint(void)
{
    ac1_scheduler *scheduler = active_scheduler;
    ac1_guest_thread *thread = active_guest_thread;
    if (!scheduler || !thread || scheduler->count < 2u ||
        scheduler_return_slice_ns == 0u) {
        scheduler_checkpoint();
        return;
    }
    if (!thread->return_slice_deadline)
        thread->return_slice_deadline = guest_monotonic_ns() + scheduler_return_slice_ns;
    if (++thread->return_slice_checks < 64u) return;
    thread->return_slice_checks = 0u;
    if (guest_monotonic_ns() < thread->return_slice_deadline) return;
    scheduler_checkpoint();
}

const char *ac1_last_error(void) { return last_run_error; }

int ac1_set_userdata_root(const char *path)
{
    if (!path || !*path || strlen(path) >= sizeof(user_data_root)) return 0;
#ifdef _WIN32
    int absolute = (path[0] == '\\' && path[1] == '\\') ||
                   (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                    path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
#else
    int absolute = path[0] == '/';
#endif
    if (!absolute) return 0;
    lock_ac1_run();
    memcpy(user_data_root, path, strlen(path) + 1u);
    unlock_ac1_run();
    return 1;
}

static int directory_exists(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
}

static int make_one_directory(const char *path)
{
#ifdef _WIN32
    if (!_mkdir(path) || (errno == EEXIST && directory_exists(path))) return 1;
    return directory_exists(path);
#else
    if (!mkdir(path, 0700) || (errno == EEXIST && directory_exists(path))) return 1;
    return directory_exists(path);
#endif
}

static int ensure_directory_tree(const char *path)
{
    char copy[32768];
    size_t length = strlen(path);
    if (!length || length >= sizeof(copy)) return 0;
    memcpy(copy, path, length + 1u);
    size_t start = 1u;
#ifdef _WIN32
    if (length >= 3u && copy[1] == ':') start = 3u;
#endif
    for (size_t i = start; i <= length; ++i) {
        if (copy[i] == '\\' || copy[i] == '/' || copy[i] == '\0') {
            char saved = copy[i];
            copy[i] = '\0';
            if (copy[0] && !directory_exists(copy) && !make_one_directory(copy)) return 0;
            copy[i] = saved;
        }
    }
    return directory_exists(copy);
}

static int get_user_data_root(char *output, size_t capacity)
{
    if (user_data_root[0]) {
        size_t n = strlen(user_data_root);
        if (n >= capacity) return 0;
        memcpy(output, user_data_root, n + 1u);
        return 1;
    }
#ifdef AC1_DEFAULT_USERDATA_ROOT
    {
        const char *configured_root = AC1_DEFAULT_USERDATA_ROOT;
        size_t n = strlen(configured_root);
        if (n >= capacity) return 0;
        memcpy(output, configured_root, n + 1u);
        return 1;
    }
#else
#ifdef _WIN32
    DWORD n = GetCurrentDirectoryA((DWORD)capacity, output);
    if (!n || n >= capacity) return 0;
    const char *suffix = "\\work\\userdata";
#else
    if (!getcwd(output, capacity)) return 0;
    size_t n = strlen(output);
    const char *suffix = "/work/userdata";
#endif
    size_t suffix_length = strlen(suffix);
    if ((size_t)n + suffix_length >= capacity) return 0;
    memcpy(output + n, suffix, suffix_length + 1u);
    return 1;
#endif
}

#ifdef _WIN32
extern char **_environ;
#else
extern char **environ;
#endif

static size_t utf8_to_utf16_count(const unsigned char *text)
{
    size_t units = 0;
    while (*text) {
        uint32_t cp;
        size_t n;
        if (*text < 0x80u) { cp = *text; n = 1; }
        else if ((*text & 0xe0u) == 0xc0u && text[1]) {
            cp = ((uint32_t)(text[0] & 0x1fu) << 6) | (text[1] & 0x3fu); n = cp >= 0x80u ? 2u : 1u;
        } else if ((*text & 0xf0u) == 0xe0u && text[1] && text[2]) {
            cp = ((uint32_t)(text[0] & 0x0fu) << 12) | ((uint32_t)(text[1] & 0x3fu) << 6) | (text[2] & 0x3fu); n = cp >= 0x800u ? 3u : 1u;
        } else if ((*text & 0xf8u) == 0xf0u && text[1] && text[2] && text[3]) {
            cp = ((uint32_t)(text[0] & 7u) << 18) | ((uint32_t)(text[1] & 0x3fu) << 12) | ((uint32_t)(text[2] & 0x3fu) << 6) | (text[3] & 0x3fu); n = cp >= 0x10000u && cp <= 0x10ffffu ? 4u : 1u;
        } else { cp = 0xfffdu; n = 1u; }
        (void)cp;
        units += n == 4u ? 2u : 1u;
        text += n;
    }
    return units;
}

static uint16_t *utf8_to_utf16_write(uint16_t *out, const unsigned char *text)
{
    while (*text) {
        uint32_t cp;
        size_t n;
        if (*text < 0x80u) { cp = *text; n = 1; }
        else if ((*text & 0xe0u) == 0xc0u && text[1]) {
            cp = ((uint32_t)(text[0] & 0x1fu) << 6) | (text[1] & 0x3fu); n = cp >= 0x80u ? 2u : 1u;
            if (n == 1u) cp = 0xfffdu;
        } else if ((*text & 0xf0u) == 0xe0u && text[1] && text[2]) {
            cp = ((uint32_t)(text[0] & 0x0fu) << 12) | ((uint32_t)(text[1] & 0x3fu) << 6) | (text[2] & 0x3fu); n = cp >= 0x800u && !(cp >= 0xd800u && cp <= 0xdfffu) ? 3u : 1u;
            if (n == 1u) cp = 0xfffdu;
        } else if ((*text & 0xf8u) == 0xf0u && text[1] && text[2] && text[3]) {
            cp = ((uint32_t)(text[0] & 7u) << 18) | ((uint32_t)(text[1] & 0x3fu) << 12) | ((uint32_t)(text[2] & 0x3fu) << 6) | (text[3] & 0x3fu); n = cp >= 0x10000u && cp <= 0x10ffffu ? 4u : 1u;
            if (n == 1u) cp = 0xfffdu;
        } else { cp = 0xfffdu; n = 1u; }
        if (n == 4u) { cp -= 0x10000u; *out++ = (uint16_t)(0xd800u + (cp >> 10)); *out++ = (uint16_t)(0xdc00u + (cp & 0x3ffu)); }
        else *out++ = (uint16_t)cp;
        text += n;
    }
    *out++ = 0;
    return out;
}

static size_t emit_utf8(uint32_t cp, uint8_t *out)
{
    if (cp <= 0x7fu) { if (out) out[0] = (uint8_t)cp; return 1u; }
    if (cp <= 0x7ffu) {
        if (out) { out[0] = (uint8_t)(0xc0u | (cp >> 6)); out[1] = (uint8_t)(0x80u | (cp & 0x3fu)); }
        return 2u;
    }
    if (cp <= 0xffffu) {
        if (out) { out[0] = (uint8_t)(0xe0u | (cp >> 12)); out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu)); out[2] = (uint8_t)(0x80u | (cp & 0x3fu)); }
        return 3u;
    }
    if (out) { out[0] = (uint8_t)(0xf0u | (cp >> 18)); out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu)); out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu)); out[3] = (uint8_t)(0x80u | (cp & 0x3fu)); }
    return 4u;
}

static int cp1252_byte(uint32_t cp, uint8_t *byte)
{
    static const uint16_t upper[32] = {
        0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,
        0x0090,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178
    };
    if (cp <= 0xffu && !(cp >= 0x80u && cp <= 0x9fu)) { *byte = (uint8_t)cp; return 1; }
    for (unsigned i = 0; i < 32u; ++i) if (upper[i] == cp) { *byte = (uint8_t)(0x80u + i); return 1; }
    return 0;
}

static size_t convert_wide_to_ansi(const uint16_t *source, size_t units,
                                   int utf8, uint8_t replacement,
                                   uint8_t *output, int *used_default)
{
    size_t bytes = 0;
    for (size_t i = 0; i < units; ++i) {
        uint32_t cp = source[i];
        if (cp >= 0xd800u && cp <= 0xdbffu && i + 1u < units &&
            source[i + 1u] >= 0xdc00u && source[i + 1u] <= 0xdfffu) {
            cp = 0x10000u + (((cp - 0xd800u) << 10) | (source[++i] - 0xdc00u));
        } else if (cp >= 0xd800u && cp <= 0xdfffu) {
            cp = 0xfffdu;
        }
        if (utf8) {
            bytes += emit_utf8(cp, output ? output + bytes : NULL);
        } else {
            uint8_t value;
            if (!cp1252_byte(cp, &value)) { value = replacement; if (used_default) *used_default = 1; }
            if (output) output[bytes] = value;
            ++bytes;
        }
    }
    return bytes;
}

static uint32_t cp1252_codepoint(uint8_t byte)
{
    static const uint16_t upper[32] = {
        0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,
        0x0090,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178
    };
    return byte >= 0x80u && byte <= 0x9fu ? upper[byte - 0x80u] : byte;
}

static size_t convert_ansi_to_wide(const uint8_t *source, size_t bytes,
                                   int utf8, uint16_t *output, int strict,
                                   int *valid)
{
    size_t units = 0;
    *valid = 1;
    for (size_t i = 0; i < bytes;) {
        uint32_t cp;
        size_t n = 1u;
        uint8_t b = source[i];
        if (!utf8) cp = cp1252_codepoint(b);
        else if (b < 0x80u) cp = b;
        else {
            if (b >= 0xc2u && b <= 0xdfu && i + 1u < bytes && (source[i + 1u] & 0xc0u) == 0x80u) {
                cp = ((uint32_t)(b & 0x1fu) << 6) | (source[i + 1u] & 0x3fu); n = 2u;
            } else if (b >= 0xe0u && b <= 0xefu && i + 2u < bytes &&
                       (source[i + 1u] & 0xc0u) == 0x80u && (source[i + 2u] & 0xc0u) == 0x80u) {
                cp = ((uint32_t)(b & 0x0fu) << 12) | ((uint32_t)(source[i + 1u] & 0x3fu) << 6) | (source[i + 2u] & 0x3fu);
                if (cp < 0x800u || (cp >= 0xd800u && cp <= 0xdfffu)) { cp = 0xfffdu; n = 1u; }
                else n = 3u;
            } else if (b >= 0xf0u && b <= 0xf4u && i + 3u < bytes &&
                       (source[i + 1u] & 0xc0u) == 0x80u && (source[i + 2u] & 0xc0u) == 0x80u && (source[i + 3u] & 0xc0u) == 0x80u) {
                cp = ((uint32_t)(b & 7u) << 18) | ((uint32_t)(source[i + 1u] & 0x3fu) << 12) | ((uint32_t)(source[i + 2u] & 0x3fu) << 6) | (source[i + 3u] & 0x3fu);
                if (cp < 0x10000u || cp > 0x10ffffu) { cp = 0xfffdu; n = 1u; }
                else n = 4u;
            } else { cp = 0xfffdu; *valid = 0; }
            if (!*valid && strict) return 0;
        }
        if (output) {
            if (cp <= 0xffffu) output[units] = (uint16_t)cp;
            else {
                cp -= 0x10000u;
                output[units] = (uint16_t)(0xd800u + (cp >> 10));
                output[units + 1u] = (uint16_t)(0xdc00u + (cp & 0x3ffu));
            }
        }
        units += cp > 0xffffu ? 2u : 1u;
        i += n;
    }
    return units;
}

#ifndef _WIN32
static uint16_t simple_case_map(uint16_t c, int upper)
{
    if (upper) {
        if (c >= 'a' && c <= 'z') return (uint16_t)(c - ('a' - 'A'));
        if ((c >= 0x00e0u && c <= 0x00f6u) || (c >= 0x00f8u && c <= 0x00feu)) return (uint16_t)(c - 0x20u);
        if (c == 0x00ffu) return 0x0178u;
    } else {
        if (c >= 'A' && c <= 'Z') return (uint16_t)(c + ('a' - 'A'));
        if ((c >= 0x00c0u && c <= 0x00d6u) || (c >= 0x00d8u && c <= 0x00deu)) return (uint16_t)(c + 0x20u);
        if (c == 0x0178u) return 0x00ffu;
    }
    return c;
}
#endif

static uint32_t rotr32(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
    };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4u] << 24) | ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) | block[i * 4u + 3u];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15u], 7) ^ rotr32(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17) ^ rotr32(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

static void sha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    uint32_t state[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                         0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    size_t full = size / 64u;
    for (size_t i = 0; i < full; ++i) sha256_block(state, data + i * 64u);
    size_t remainder = size % 64u;
    uint8_t tail[128] = {0};
    memcpy(tail, data + full * 64u, remainder);
    tail[remainder] = 0x80u;
    size_t tail_size = remainder < 56u ? 64u : 128u;
    uint64_t bit_length = (uint64_t)size * 8u;
    for (unsigned i = 0; i < 8; ++i)
        tail[tail_size - 1u - i] = (uint8_t)(bit_length >> (i * 8u));
    sha256_block(state, tail);
    if (tail_size == 128u) sha256_block(state, tail + 64u);
    for (unsigned i = 0; i < 8; ++i) {
        digest[i * 4u] = (uint8_t)(state[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(state[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(state[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)state[i];
    }
}

static void digest_hex(const uint8_t digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        hex[i * 2u] = digits[digest[i] >> 4];
        hex[i * 2u + 1u] = digits[digest[i] & 15u];
    }
    hex[64] = '\0';
}

static uint16_t image_u16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t image_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int initialize_static_tls(pe32_module *image, char *error, size_t error_capacity)
{
    if (!image || !image->image || image->image_size < 0x40u) {
        snprintf(error, error_capacity, "invalid image for static TLS setup");
        return 0;
    }
    uint32_t nt = image_u32(image->image + 0x3cu);
    if ((uint64_t)nt + 24u + 96u > image->image_size ||
        image_u32(image->image + nt) != 0x00004550u) {
        snprintf(error, error_capacity, "invalid PE headers during static TLS setup");
        return 0;
    }
    uint16_t optional_size = image_u16(image->image + nt + 20u);
    uint32_t optional = nt + 24u;
    if (optional_size < 96u || (uint64_t)optional + optional_size > image->image_size ||
        image_u16(image->image + optional) != 0x010bu) {
        snprintf(error, error_capacity, "invalid PE32 optional header during static TLS setup");
        return 0;
    }
    uint32_t directory_count = image_u32(image->image + optional + 92u);
    if (directory_count <= 9u) return 1;
    uint32_t directory = optional + 96u + 9u * 8u;
    if ((uint64_t)directory + 8u > (uint64_t)optional + optional_size) {
        snprintf(error, error_capacity, "TLS data-directory entry is truncated");
        return 0;
    }
    uint32_t tls_rva = image_u32(image->image + directory);
    uint32_t tls_size = image_u32(image->image + directory + 4u);
    if (!tls_rva && !tls_size) return 1;
    if (!tls_rva || tls_size < 24u || (uint64_t)tls_rva + 24u > image->image_size) {
        snprintf(error, error_capacity, "invalid PE32 TLS directory");
        return 0;
    }
    const uint8_t *tls = image->image + tls_rva;
    uint32_t raw_start = image_u32(tls + 0u);
    uint32_t raw_end = image_u32(tls + 4u);
    uint32_t index_va = image_u32(tls + 8u);
    uint32_t callbacks_va = image_u32(tls + 12u);
    uint32_t zero_fill = image_u32(tls + 16u);
    if (raw_end < raw_start || raw_start < image->load_base ||
        (uint64_t)raw_end - image->load_base > image->image_size ||
        index_va < image->load_base || (uint64_t)index_va + 4u > (uint64_t)image->load_base + image->image_size) {
        snprintf(error, error_capacity, "TLS template or index is outside the image");
        return 0;
    }
    if (zero_fill > 0x1000000u || (uint64_t)(raw_end - raw_start) + zero_fill > 0x1000000u) {
        snprintf(error, error_capacity, "TLS template exceeds the bounded initial profile");
        return 0;
    }
    if (callbacks_va) {
        if (callbacks_va < image->load_base ||
            (uint64_t)callbacks_va + 4u > (uint64_t)image->load_base + image->image_size) {
            snprintf(error, error_capacity, "TLS callback vector is outside the image");
            return 0;
        }
        uint32_t callback = image_u32(image->image + (callbacks_va - image->load_base));
        if (callback) {
            snprintf(error, error_capacity, "static TLS callbacks are not yet implemented (first=0x%08x)", callback);
            return 0;
        }
    }

    uint32_t index = 0u; /* Single executable module; its loader-assigned TLS index is zero. */
    uint32_t template_size = raw_end - raw_start;
    uint32_t total_size = template_size + zero_fill;
    if (!total_size) total_size = 1u;
    static_tls_template = image->image + (raw_start - image->load_base);
    static_tls_template_size = template_size;
    static_tls_zero_fill = zero_fill;
    static_tls_index_address = index_va;
    static_tls_data_base = guest_memory_find_gap(0x70000000u, total_size, 16u, AC1_STACK_BASE);
    static_tls_vector_base = guest_memory_find_gap(0x7FC00000u, 64u * 4u, 16u, AC1_TIB_BASE);
    if (!static_tls_data_base || !static_tls_vector_base) {
        snprintf(error, error_capacity, "no guest range for static TLS block/vector");
        return 0;
    }
    uint8_t *tls_data = (uint8_t *)guest_memory_allocate(static_tls_data_base, total_size);
    uint32_t *tls_vector = (uint32_t *)guest_memory_allocate(static_tls_vector_base, 64u * 4u);
    void *index_pointer = guest_memory_try_ptr(index_va, 4u);
    if (!tls_data || !tls_vector || !index_pointer) {
        snprintf(error, error_capacity, "could not allocate or map static TLS data");
        return 0;
    }
    memset(tls_data, 0, total_size);
    if (template_size)
        memcpy(tls_data, image->image + (raw_start - image->load_base), template_size);
    tls_vector[index] = static_tls_data_base;
    memcpy(index_pointer, &index, sizeof(index));
    wr32(AC1_TIB_BASE + 0x2cu, static_tls_vector_base);
    tls_used[index] = 1u;
    tls_values[index] = static_tls_data_base;
    return 1;
}

static void stop_guest(const char *reason, uint32_t target);

void ac1_unsupported_instruction(uint32_t guest_va, const char *reason)
{
    stop_guest(reason ? reason : "unsupported translated instruction", guest_va);
}

static int equal_ascii_fold(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static void return_stdcall(CPU *cpu, uint32_t argument_bytes)
{
    cpu->eip = rd32(cpu->esp);
    cpu->esp += 4u + argument_bytes;
}

static pcrecomp_heap *heap_by_handle(uint32_t handle)
{
    if (handle == AC1_PROCESS_HEAP_HANDLE) return process_heap;
    for (size_t i = 0; i < AC1_MAX_PRIVATE_HEAPS; ++i)
        if (private_heaps[i].handle == handle) return private_heaps[i].heap;
    return NULL;
}

#ifdef _WIN32
static uint32_t guest_monitor_for_host(uintptr_t native_monitor)
{
    if (!native_monitor) return 0u;
    AcquireSRWLockExclusive(&guest_monitor_lock);
    for (size_t i = 0u; i < guest_monitor_count; ++i) {
        if (guest_monitors[i].native == native_monitor) {
            uint32_t token = guest_monitors[i].guest;
            ReleaseSRWLockExclusive(&guest_monitor_lock);
            return token;
        }
    }
    if (guest_monitor_count == guest_monitor_capacity) {
        size_t new_capacity = guest_monitor_capacity
            ? guest_monitor_capacity * 2u : 8u;
        if (new_capacity < guest_monitor_capacity ||
            new_capacity > SIZE_MAX / sizeof(*guest_monitors)) {
            ReleaseSRWLockExclusive(&guest_monitor_lock);
            return 0u;
        }
        ac1_guest_monitor *grown = (ac1_guest_monitor *)realloc(
            guest_monitors, new_capacity * sizeof(*guest_monitors));
        if (!grown) {
            ReleaseSRWLockExclusive(&guest_monitor_lock);
            return 0u;
        }
        guest_monitors = grown;
        guest_monitor_capacity = new_capacity;
    }
    if (next_guest_monitor > 0xE6FFFFFCu) {
        ReleaseSRWLockExclusive(&guest_monitor_lock);
        return 0u;
    }
    uint32_t token = next_guest_monitor;
    next_guest_monitor += 4u;
    guest_monitors[guest_monitor_count].native = native_monitor;
    guest_monitors[guest_monitor_count].guest = token;
    ++guest_monitor_count;
    ReleaseSRWLockExclusive(&guest_monitor_lock);
    return token;
}
#endif

#if defined(AC1_HOST_D3D9)
static void *d3d9_guest_resolve(void *context, uint32_t address,
                               uint32_t size, uint32_t access)
{
    (void)context;
    if (!size || !access || (access & ~(AC1_D3D9_GUEST_READ | AC1_D3D9_GUEST_WRITE)))
        return NULL;
    return guest_memory_try_ptr(address, size);
}

static uint32_t d3d9_guest_alloc(void *context, uint32_t size, uint32_t alignment)
{
    (void)context;
    if (!process_heap || !size || !alignment || (alignment & (alignment - 1u)) ||
        alignment > 4096u || size > UINT32_MAX - alignment + 1u) {
        if (trace_imports)
            fprintf(stderr,
                    "AC1 D3D9 guest allocation rejected: size=%u alignment=%u process_heap=%u\n",
                    size, alignment, process_heap != NULL);
        return 0u;
    }
    size_t slot;
    for (slot = 0; slot < d3d9_heap_block_count; ++slot)
        if (!d3d9_heap_blocks[slot].guest_address) break;
    if (slot == d3d9_heap_block_count) {
        if (d3d9_heap_block_count == d3d9_heap_block_capacity) {
            size_t new_capacity = d3d9_heap_block_capacity
                ? d3d9_heap_block_capacity * 2u : 64u;
            if (new_capacity < d3d9_heap_block_capacity ||
                new_capacity > SIZE_MAX / sizeof(*d3d9_heap_blocks)) {
                if (trace_imports)
                    fprintf(stderr,
                            "AC1 D3D9 heap ledger capacity overflow: entries=%zu\n",
                            d3d9_heap_block_capacity);
                return 0u;
            }
            ac1_d3d9_heap_block *grown = (ac1_d3d9_heap_block *)realloc(
                d3d9_heap_blocks, new_capacity * sizeof(*d3d9_heap_blocks));
            if (!grown) {
                if (trace_imports)
                    fprintf(stderr,
                            "AC1 D3D9 heap ledger growth failed: old_entries=%zu new_entries=%zu\n",
                            d3d9_heap_block_capacity, new_capacity);
                return 0u;
            }
            memset(grown + d3d9_heap_block_capacity, 0,
                   (new_capacity - d3d9_heap_block_capacity) * sizeof(*grown));
            d3d9_heap_blocks = grown;
            d3d9_heap_block_capacity = new_capacity;
        }
        ++d3d9_heap_block_count;
    }
    uint32_t raw = pcrecomp_heap_alloc(process_heap, size + alignment - 1u, 1);
    if (!raw) {
        if (trace_imports)
            fprintf(stderr,
                    "AC1 D3D9 guest heap allocation failed: size=%u alignment=%u tracked=%zu\n",
                    size, alignment, d3d9_heap_block_count);
        return 0u;
    }
    uint32_t aligned = (raw + alignment - 1u) & ~(alignment - 1u);
    d3d9_heap_blocks[slot].guest_address = aligned;
    d3d9_heap_blocks[slot].heap_address = raw;
    return aligned;
}

static void d3d9_guest_free(void *context, uint32_t address)
{
    (void)context;
    for (size_t i = 0; i < d3d9_heap_block_count; ++i) {
        if (d3d9_heap_blocks[i].guest_address != address) continue;
        if (process_heap) pcrecomp_heap_free(process_heap, d3d9_heap_blocks[i].heap_address);
        memset(&d3d9_heap_blocks[i], 0, sizeof(d3d9_heap_blocks[i]));
        return;
    }
}

#ifdef _WIN32
#if defined(AC1_HOST_D3D9)
static uint32_t d3d9_host_monitor_to_guest(void *context,
                                          uintptr_t native_monitor)
{
    (void)context;
    uint32_t token = guest_monitor_for_host(native_monitor);
    if (!token && trace_imports)
        fprintf(stderr, "AC1 D3D9 monitor mapping failed: native=%p\n",
                (void *)native_monitor);
    return token;
}
#endif

static uintptr_t d3d9_guest_hwnd_to_host(void *context, uint32_t guest_hwnd)
{
    (void)context;
    return window_bridge
        ? ac1_window_win32_resolve_hwnd(window_bridge, guest_hwnd) : 0u;
}
#endif
#endif /* AC1_HOST_D3D9 */

#ifdef _WIN32
static void *ds8_guest_resolve(void *context, uint32_t address,
                               uint32_t size, uint32_t access)
{
    (void)context;
    if (!size || !access || (access & ~(AC1_DS8_GUEST_READ | AC1_DS8_GUEST_WRITE)))
        return NULL;
    return guest_memory_try_ptr(address, size);
}

static uint32_t ds8_guest_alloc(void *context, uint32_t size, uint32_t alignment)
{
    (void)context;
    if (!process_heap || !size || !alignment || (alignment & (alignment - 1u)) ||
        alignment > 4096u || size > UINT32_MAX - alignment + 1u)
        return 0u;
    uint32_t raw = pcrecomp_heap_alloc(process_heap, size + alignment - 1u, 1);
    if (!raw) return 0u;
    uint32_t aligned = (raw + alignment - 1u) & ~(alignment - 1u);
    if (aligned != raw) {
        /* The guest heap currently returns at least 8-byte aligned blocks. */
        pcrecomp_heap_free(process_heap, raw);
        return 0u;
    }
    return raw;
}

static void ds8_guest_free(void *context, uint32_t address)
{
    (void)context;
    if (process_heap && address) (void)pcrecomp_heap_free(process_heap, address);
}

static uintptr_t ds8_guest_hwnd_to_host(void *context, uint32_t guest_hwnd)
{
    (void)context;
    return window_bridge
        ? ac1_window_win32_resolve_hwnd(window_bridge, guest_hwnd) : 0u;
}

static int ds8_ensure_configured(void)
{
    if (ds8_configured) return 1;
    ac1_ds8_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = ds8_guest_resolve;
    callbacks.guest_alloc = ds8_guest_alloc;
    callbacks.guest_free = ds8_guest_free;
    callbacks.guest_hwnd_to_host = ds8_guest_hwnd_to_host;
    if (!ac1_ds8_configure(&callbacks, NULL)) return 0;
    ds8_configured = 1;
    return 1;
}

static int handle_ds8_import(CPU *cpu, const import_record *import)
{
    if (!import || !import->by_ordinal ||
        !equal_ascii_fold(import->dll, "DSOUND.dll") || import->ordinal != 11u)
        return 0;
    if (!guest_memory_try_ptr(cpu->esp + 4u, 12u))
        stop_guest("DirectSoundCreate8 arguments are unmapped", import->token);
    if (!ds8_ensure_configured())
        stop_guest(ac1_ds8_last_error()[0]
                   ? ac1_ds8_last_error()
                   : "DirectSound bridge configuration failed", import->token);

    uint32_t result = 0u;
    int status = ac1_ds8_create8(rd32(cpu->esp + 4u),
                                 rd32(cpu->esp + 8u),
                                 rd32(cpu->esp + 12u), &result);
    if (status != AC1_DS8_HANDLED) {
        last_error = status == AC1_DS8_METHOD_UNSUPPORTED ? 120u : 87u;
        stop_guest(ac1_ds8_last_error()[0]
                   ? ac1_ds8_last_error()
                   : "DirectSoundCreate8 bridge rejected the guest call",
                   import->token);
    }
    cpu->eax = result;
    return_stdcall(cpu, 12u);
    return 1;
}

static uintptr_t dinput8_guest_hinstance_to_host(void *context,
                                                uint32_t guest_hinstance)
{
    (void)context;
    if (guest_hinstance == image_base)
        return (uintptr_t)GetModuleHandleA(NULL);
    return 0u;
}

static int dinput8_ensure_configured(void)
{
    if (dinput8_configured) return 1;
    ac1_dinput8_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = ds8_guest_resolve;
    callbacks.guest_alloc = ds8_guest_alloc;
    callbacks.guest_free = ds8_guest_free;
    callbacks.guest_hwnd_to_host = ds8_guest_hwnd_to_host;
    callbacks.guest_hinstance_to_host = dinput8_guest_hinstance_to_host;
    if (!ac1_dinput8_configure(&callbacks, NULL)) return 0;
    dinput8_configured = 1;
    return 1;
}

static int handle_dinput8_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "DINPUT8.dll") ||
        strcmp(import->name, "DirectInput8Create"))
        return 0;
    if (!guest_memory_try_ptr(cpu->esp + 4u, 20u))
        stop_guest("DirectInput8Create arguments are unmapped", import->token);
    if (!dinput8_ensure_configured())
        stop_guest(ac1_dinput8_last_error()[0]
                   ? ac1_dinput8_last_error()
                   : "DirectInput8 bridge configuration failed", import->token);

    uint32_t guest_hinstance = rd32(cpu->esp + 4u);
    uint32_t version = rd32(cpu->esp + 8u);
    uint32_t guest_iid = rd32(cpu->esp + 12u);
    uint32_t guest_output = rd32(cpu->esp + 16u);
    uint32_t guest_outer = rd32(cpu->esp + 20u);
    uint32_t result = 0u;
    int status = ac1_dinput8_create8(guest_hinstance, version, guest_iid,
                                     guest_output, guest_outer, &result);
    if (trace_imports) {
        uint32_t proxy = guest_memory_try_ptr(guest_output, 4u)
            ? rd32(guest_output) : 0u;
        fprintf(stderr,
                "AC1 DirectInput factory: status=%d HRESULT=0x%08x hinst=0x%08x version=0x%08x iid=0x%08x out=0x%08x proxy=0x%08x outer=0x%08x\n",
                status, result, guest_hinstance, version, guest_iid,
                guest_output, proxy, guest_outer);
    }
    if (status != AC1_DINPUT8_HANDLED) {
        last_error = status == AC1_DINPUT8_METHOD_UNSUPPORTED ? 120u : 87u;
        stop_guest(ac1_dinput8_last_error()[0]
                   ? ac1_dinput8_last_error()
                   : "DirectInput8Create bridge rejected the guest call",
                   import->token);
    }
    cpu->eax = result;
    return_stdcall(cpu, 20u);
    return 1;
}

static int dispatch_ds8_method(CPU *cpu, uint32_t target)
{
    int argument_count = ac1_ds8_argument_count(target);
    if (!argument_count) return 0;
    if (argument_count < 0 || argument_count > 16) {
        last_error = argument_count == AC1_DS8_METHOD_UNSUPPORTED ? 120u : 87u;
        if (trace_imports && target == AC1_DS8_TOKEN_BASE + 5u * 0x1000u + 11u * 0x10u &&
            guest_memory_try_ptr(cpu->esp + 4u, 8u * 4u)) {
            fprintf(stderr,
                    "AC1 DirectSound Lock stack: this=%08x offset=%08x bytes=%08x ptr1out=%08x bytes1out=%08x ptr2out=%08x bytes2out=%08x flags=%08x\n",
                    rd32(cpu->esp + 4u), rd32(cpu->esp + 8u),
                    rd32(cpu->esp + 12u), rd32(cpu->esp + 16u),
                    rd32(cpu->esp + 20u), rd32(cpu->esp + 24u),
                    rd32(cpu->esp + 28u), rd32(cpu->esp + 32u));
        }
        if (trace_imports && target == AC1_DS8_TOKEN_BASE + 5u * 0x1000u + 12u * 0x10u &&
            guest_memory_try_ptr(cpu->esp + 4u, 4u * 4u)) {
            fprintf(stderr,
                    "AC1 DirectSound Play stack: this=%08x reserved=%08x priority=%08x flags=%08x ebp=%08x\n",
                    rd32(cpu->esp + 4u), rd32(cpu->esp + 8u),
                    rd32(cpu->esp + 12u), rd32(cpu->esp + 16u), cpu->ebp);
        }
        if (trace_imports && target == AC1_DS8_TOKEN_BASE + 5u * 0x1000u + 15u * 0x10u &&
            guest_memory_try_ptr(cpu->esp + 4u, 2u * 4u)) {
            fprintf(stderr, "AC1 DirectSound SetVolume stack: this=%08x volume=%08x\n",
                    rd32(cpu->esp + 4u), rd32(cpu->esp + 8u));
        }
        char message[160];
        if (argument_count == AC1_DS8_METHOD_UNSUPPORTED) {
            const uint32_t token_offset = target - AC1_DS8_TOKEN_BASE;
            snprintf(message, sizeof(message),
                     "unsupported DirectSound interface kind=%u slot=%u",
                     token_offset / 0x1000u,
                     (token_offset % 0x1000u) / 0x10u);
        } else {
            snprintf(message, sizeof(message), "invalid DirectSound COM method token");
        }
        stop_guest(message, target);
    }
    if (!guest_memory_try_ptr(cpu->esp + 4u, (size_t)argument_count * 4u))
        stop_guest("DirectSound COM arguments are unmapped", target);
    uint32_t arguments[16];
    for (int i = 0; i < argument_count; ++i)
        arguments[i] = rd32(cpu->esp + 4u + (uint32_t)i * 4u);
    if (trace_imports && target == AC1_DS8_TOKEN_BASE + 3u * 0x1000u + 14u * 0x10u &&
        argument_count == 2 && guest_memory_try_ptr(arguments[1], 18u)) {
        const uint32_t format = arguments[1];
        fprintf(stderr,
                "AC1 DirectSound format: tag=%u channels=%u samples=%u avg_bytes=%u align=%u bits=%u extra=%u\n",
                rd16(format), rd16(format + 2u), rd32(format + 4u),
                rd32(format + 8u), rd16(format + 12u), rd16(format + 14u),
                rd16(format + 16u));
    }
    if (trace_imports && target == AC1_DS8_TOKEN_BASE + 2u * 0x1000u + 3u * 0x10u &&
        argument_count == 4 && guest_memory_try_ptr(arguments[1], 36u)) {
        const uint32_t descriptor = arguments[1];
        fprintf(stderr,
                "AC1 DirectSound buffer descriptor: size=%u flags=0x%08x bytes=%u reserved=%u format=0x%08x algorithm=%08x-%08x-%08x-%08x\n",
                rd32(descriptor), rd32(descriptor + 4u),
                rd32(descriptor + 8u), rd32(descriptor + 12u),
                rd32(descriptor + 16u), rd32(descriptor + 20u),
                rd32(descriptor + 24u), rd32(descriptor + 28u),
                rd32(descriptor + 32u));
    }
    uint32_t result = 0u;
    int dispatched = ac1_ds8_dispatch(target, arguments,
                                      (uint32_t)argument_count, &result);
    if (dispatched != AC1_DS8_HANDLED) {
        last_error = dispatched == AC1_DS8_METHOD_UNSUPPORTED ? 120u : 87u;
        char message[256];
        if (target == AC1_DS8_TOKEN_BASE + 2u * 0x1000u + 6u * 0x10u &&
            argument_count == 3) {
            snprintf(message, sizeof(message),
                     "DirectSound SetCooperativeLevel bridge failure (HWND=0x%08x priority=%u): %s",
                     arguments[1], arguments[2],
                     ac1_ds8_last_error()[0] ? ac1_ds8_last_error() : "unknown failure");
        } else {
            snprintf(message, sizeof(message), "%s",
                     ac1_ds8_last_error()[0]
                         ? ac1_ds8_last_error()
                         : "DirectSound COM dispatcher rejected the guest call");
        }
        stop_guest(message, target);
    }
    cpu->eax = result;
    return_stdcall(cpu, (uint32_t)argument_count * 4u);
    scheduler_checkpoint();
    return 1;
}

static int dispatch_dinput8_method(CPU *cpu, uint32_t target)
{
    int argument_count = ac1_dinput8_argument_count(target);
    if (!argument_count) return 0;
    if (argument_count < 0 || argument_count > 16) {
        last_error = argument_count == AC1_DINPUT8_METHOD_UNSUPPORTED ? 120u : 87u;
        uint32_t offset = target - AC1_DINPUT8_TOKEN_BASE;
        char message[160];
        if (argument_count == AC1_DINPUT8_METHOD_UNSUPPORTED) {
            if (trace_imports && offset / 0x1000u == 5u &&
                (offset % 0x1000u) / 0x10u == 13u &&
                guest_memory_try_ptr(cpu->esp + 4u, 12u))
                fprintf(stderr,
                        "AC1 DirectInput unsupported slot trace: kind=5 slot=13 eip=0x%08x this=0x%08x hwnd=0x%08x flags=0x%08x\n",
                        cpu->eip, rd32(cpu->esp + 4u), rd32(cpu->esp + 8u),
                        rd32(cpu->esp + 12u));
            snprintf(message, sizeof(message),
                     "unsupported DirectInput interface kind=%u slot=%u",
                     offset / 0x1000u, (offset % 0x1000u) / 0x10u);
        } else
            snprintf(message, sizeof(message), "invalid DirectInput COM method token");
        stop_guest(message, target);
    }
    if (!guest_memory_try_ptr(cpu->esp + 4u, (size_t)argument_count * 4u))
        stop_guest("DirectInput COM arguments are unmapped", target);
    uint32_t arguments[16];
    for (int i = 0; i < argument_count; ++i)
        arguments[i] = rd32(cpu->esp + 4u + (uint32_t)i * 4u);

    uint32_t result = 0u;
    int status = ac1_dinput8_dispatch(target, arguments,
                                      (uint32_t)argument_count, &result);
    if (status != AC1_DINPUT8_HANDLED) {
        last_error = status == AC1_DINPUT8_METHOD_UNSUPPORTED ? 120u : 87u;
        stop_guest(ac1_dinput8_last_error()[0]
                   ? ac1_dinput8_last_error()
                   : "DirectInput COM dispatcher rejected the guest call",
                   target);
    }

    const uint32_t method_offset = target - AC1_DINPUT8_TOKEN_BASE;
    const uint32_t method_kind = method_offset / 0x1000u;
    const uint32_t method_slot = (method_offset % 0x1000u) / 0x10u;
    if (trace_imports &&
        ((method_kind == 3u && (method_slot == 3u || method_slot == 4u)) ||
         (method_kind == 5u && (method_slot == 4u || method_slot == 11u ||
                               method_slot == 13u)))) {
        fprintf(stderr,
                "AC1 DirectInput COM: kind=%u slot=%u HRESULT=0x%08x argc=%d this=0x%08x\n",
                method_kind, method_slot, result, argument_count, arguments[0]);
        if (method_kind == 3u && method_slot == 3u && argument_count == 4) {
            uint32_t proxy = guest_memory_try_ptr(arguments[2], 4u)
                ? rd32(arguments[2]) : 0u;
            fprintf(stderr,
                    "AC1 DirectInput CreateDevice: guid=0x%08x out=0x%08x proxy=0x%08x outer=0x%08x\n",
                    arguments[1], arguments[2], proxy, arguments[3]);
        } else if (method_kind == 5u && method_slot == 11u &&
                   argument_count == 2 &&
                   guest_memory_try_ptr(arguments[1], 24u)) {
            uint32_t format = arguments[1];
            fprintf(stderr,
                    "AC1 DirectInput SetDataFormat: size=%u object_size=%u flags=0x%08x data_size=%u objects=%u array=0x%08x\n",
                    rd32(format), rd32(format + 4u), rd32(format + 8u),
                    rd32(format + 12u), rd32(format + 16u),
                    rd32(format + 20u));
        } else if (method_kind == 5u && method_slot == 13u &&
                   argument_count == 3) {
            fprintf(stderr,
                    "AC1 DirectInput SetCooperativeLevel: guest_hwnd=0x%08x flags=0x%08x\n",
                    arguments[1], arguments[2]);
        }
    }

    const uint32_t enum_devices_token = AC1_DINPUT8_TOKEN_BASE +
        3u * 0x1000u + 4u * 0x10u;
    const uint32_t enum_objects_token = AC1_DINPUT8_TOKEN_BASE +
        5u * 0x1000u + 4u * 0x10u;
    const int is_enum_devices = target == enum_devices_token;
    const int is_enum_objects = target == enum_objects_token;
    if (is_enum_devices || is_enum_objects) {
        const uint32_t callback_index = is_enum_devices ? 2u : 1u;
        const uint32_t ref_index = is_enum_devices ? 3u : 2u;
        const uint32_t expected_arity = is_enum_devices ? 5u : 4u;
        const uint32_t expected_record_size = is_enum_devices ? 0x44cu : 0x240u;
        if ((uint32_t)argument_count != expected_arity ||
            !arguments[callback_index])
            stop_guest("DirectInput enumeration callback/arity is invalid", target);
        ac1_dinput8_enum_batch batch;
        memset(&batch, 0, sizeof(batch));
        int batch_status = ac1_dinput8_enum_batch_take(&batch);
        if (batch_status != 1) {
            if (batch_status > 0) ac1_dinput8_enum_batch_free(&batch);
            stop_guest("DirectInput enumeration did not produce a native batch", target);
        }
        if (batch.record_size != expected_record_size || batch.count > 4096u ||
            batch.count > UINT32_MAX / batch.record_size) {
            ac1_dinput8_enum_batch_free(&batch);
            stop_guest("DirectInput enumeration batch has an invalid x86 W record layout",
                       target);
        }
        if (batch.native_hresult != (int32_t)result) {
            ac1_dinput8_enum_batch_free(&batch);
            stop_guest("DirectInput enumeration batch/result HRESULT mismatch", target);
        }
        uint32_t record_count = batch.count;
        uint32_t record_size = batch.record_size;
        uint32_t records_address = 0u;
        if (record_count) {
            uint32_t bytes = record_count * record_size;
            records_address = pcrecomp_heap_alloc(process_heap, bytes, 1);
            if (!records_address) {
                ac1_dinput8_enum_batch_free(&batch);
                stop_guest("DirectInput enumeration guest-record allocation failed",
                           target);
            }
            uint8_t *destination = (uint8_t *)guest_memory_try_ptr(records_address, bytes);
            if (!destination) {
                pcrecomp_heap_free(process_heap, records_address);
                ac1_dinput8_enum_batch_free(&batch);
                stop_guest("DirectInput enumeration guest-record block is unmapped",
                           target);
            }
            for (uint32_t i = 0u; i < record_count; ++i) {
                const void *record = ac1_dinput8_enum_batch_record(&batch, i);
                if (!record) {
                    pcrecomp_heap_free(process_heap, records_address);
                    ac1_dinput8_enum_batch_free(&batch);
                    stop_guest("DirectInput enumeration native record is unavailable",
                               target);
                }
                memcpy(destination + (size_t)i * batch.record_size,
                       record, record_size);
            }
        }
        ac1_dinput8_enum_batch_free(&batch);

        uint32_t callback_count = 0u;
        for (uint32_t i = 0u; i < record_count; ++i) {
            uint32_t callback_arguments[2] = {
                records_address + i * record_size, arguments[ref_index]
            };
            uint32_t callback_result = 0u;
            if (!invoke_guest_stdcall_callback(cpu, arguments[callback_index],
                                              callback_arguments, 2u,
                                              &callback_result)) {
                if (records_address) pcrecomp_heap_free(process_heap, records_address);
                stop_guest("DirectInput enumeration guest callback frame is unavailable",
                           target);
            }
            ++callback_count;
            if (!callback_result) break;
        }
        if (trace_imports) {
            fprintf(stderr,
                    "AC1 DirectInput enumeration: kind=%u slot=%u HRESULT=0x%08x records=%u record_size=0x%x callback=0x%08x ref=0x%08x delivered=%u",
                    method_kind, method_slot, result, record_count,
                    record_size, arguments[callback_index],
                    arguments[ref_index], callback_count);
            if (is_enum_devices)
                fprintf(stderr, " devtype=0x%08x flags=0x%08x\n",
                        arguments[1], arguments[4]);
            else
                fprintf(stderr, " flags=0x%08x\n", arguments[3]);
        }
        if (records_address) pcrecomp_heap_free(process_heap, records_address);
    }

    cpu->eax = result;
    return_stdcall(cpu, (uint32_t)argument_count * 4u);
    scheduler_checkpoint();
    return 1;
}
#endif

#if defined(AC1_HOST_D3D9)
static int d3d9_ensure_configured(void)
{
    if (d3d9_configured) return 1;
    ac1_d3d9_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = d3d9_guest_resolve;
    callbacks.guest_alloc = d3d9_guest_alloc;
    callbacks.guest_free = d3d9_guest_free;
#ifdef _WIN32
    callbacks.guest_hwnd_to_host = d3d9_guest_hwnd_to_host;
    callbacks.host_monitor_to_guest = d3d9_host_monitor_to_guest;
#elif defined(__ANDROID__)
    callbacks.factory = ac1_android_d3d9_factory;
    callbacks.guest_hwnd_to_host = ac1_android_d3d9_guest_hwnd;
#endif
    if (!ac1_d3d9_configure(&callbacks, NULL)) return 0;
    d3d9_configured = 1;
    return 1;
}

static int handle_d3d9_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "d3d9.dll") ||
        strcmp(import->name, "Direct3DCreate9"))
        return 0;
    if (!d3d9_ensure_configured())
        stop_guest(ac1_d3d9_last_error(), import->token);
    cpu->eax = ac1_d3d9_create9(rd32(cpu->esp + 4u));
    if (trace_imports)
        fprintf(stderr, "AC1 milestone: Direct3DCreate9 sdk=0x%08x guest=0x%08x\n",
                rd32(cpu->esp + 4u), cpu->eax);
    if (!cpu->eax && ac1_d3d9_last_error()[0])
        fprintf(stderr, "AC1 D3D9 factory returned null: %s\n", ac1_d3d9_last_error());
    return_stdcall(cpu, 4u);
    return 1;
}
#endif

static int handle_d3dx_texture_requirements_import(
    CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "d3dx9_36.dll") ||
        strcmp(import->name, "D3DXCheckTextureRequirements"))
        return 0;

#if defined(AC1_HOST_D3D9)
    if (!d3d9_ensure_configured())
        stop_guest(ac1_d3d9_last_error(), import->token);
    if (!guest_memory_try_ptr(cpu->esp + 4u, 7u * 4u))
        stop_guest("D3DXCheckTextureRequirements arguments are unmapped",
                   import->token);

    uint32_t args[7];
    for (unsigned i = 0u; i < 7u; ++i)
        args[i] = rd32(cpu->esp + 4u + i * 4u);

    uint32_t native_hresult = 0u;
    int status = ac1_d3d9_check_texture_requirements(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6],
        &native_hresult);
    if (status != 1) {
        last_error = status == -2 ? 120u : 87u;
        stop_guest(ac1_d3d9_last_error()[0]
                   ? ac1_d3d9_last_error()
                   : (status == -2
                      ? "D3DXCheckTextureRequirements is unavailable"
                      : "D3DXCheckTextureRequirements rejected its guest arguments"),
                   import->token);
    }

    cpu->eax = native_hresult;
    if (trace_imports) {
        uint32_t width = args[1] && guest_memory_try_ptr(args[1], 4u)
            ? rd32(args[1]) : 0u;
        uint32_t height = args[2] && guest_memory_try_ptr(args[2], 4u)
            ? rd32(args[2]) : 0u;
        uint32_t mip_levels = args[3] && guest_memory_try_ptr(args[3], 4u)
            ? rd32(args[3]) : 0u;
        uint32_t format = args[5] && guest_memory_try_ptr(args[5], 4u)
            ? rd32(args[5]) : 0u;
        fprintf(stderr,
                "AC1 D3DXCheckTextureRequirements: device=0x%08x width=0x%08x:%u height=0x%08x:%u mips=0x%08x:%u usage=0x%08x format=0x%08x:%u pool=%u HRESULT=0x%08x\n",
                args[0], args[1], width, args[2], height, args[3],
                mip_levels, args[4], args[5], format, args[6], native_hresult);
    }
    return_stdcall(cpu, 7u * 4u);
    scheduler_checkpoint();
    return 1;
#else
    stop_guest("D3DXCheckTextureRequirements has no portable implementation",
               import->token);
    return 1;
#endif
}

static int handle_d3dx_shader_constant_table_import(
    CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "d3dx9_36.dll") ||
        strcmp(import->name, "D3DXGetShaderConstantTable"))
        return 0;

#if defined(AC1_HOST_D3D9)
    if (!d3d9_ensure_configured())
        stop_guest(ac1_d3d9_last_error(), import->token);
    if (!guest_memory_try_ptr(cpu->esp + 4u, 2u * 4u))
        stop_guest("D3DXGetShaderConstantTable arguments are unmapped",
                   import->token);

    uint32_t guest_bytecode = rd32(cpu->esp + 4u);
    uint32_t guest_output = rd32(cpu->esp + 8u);
    uint32_t native_hresult = 0u;
    int status = ac1_d3d9_get_shader_constant_table(
        guest_bytecode, guest_output, &native_hresult);
    if (status != 1) {
        last_error = status == -2 ? 120u : 87u;
        stop_guest(ac1_d3d9_last_error()[0]
                   ? ac1_d3d9_last_error()
                   : (status == -2
                      ? "D3DXGetShaderConstantTable is unavailable"
                      : "D3DXGetShaderConstantTable rejected its guest arguments"),
                   import->token);
    }

    cpu->eax = native_hresult;
    if (trace_imports) {
        uint32_t guest_table = guest_output &&
            guest_memory_try_ptr(guest_output, 4u) ? rd32(guest_output) : 0u;
        fprintf(stderr,
                "AC1 D3DXGetShaderConstantTable: bytecode=0x%08x output=0x%08x table=0x%08x HRESULT=0x%08x\n",
                guest_bytecode, guest_output, guest_table, native_hresult);
    }
    return_stdcall(cpu, 2u * 4u);
    scheduler_checkpoint();
    return 1;
#else
    stop_guest("D3DXGetShaderConstantTable has no portable implementation",
               import->token);
    return 1;
#endif
}

#if defined(AC1_HOST_D3D9)
static int dispatch_d3d9_method(CPU *cpu, uint32_t target)
{
    int argument_count = ac1_d3d9_argument_count(target);
    if (!argument_count) return 0;
    if (argument_count < 0 || argument_count > 16) {
        last_error = argument_count == -2 ? 120u : 87u;
        stop_guest(argument_count == -2
                   ? "unsupported D3D9 COM method"
                   : "invalid D3D9 COM method token", target);
    }
    if (!guest_memory_try_ptr(cpu->esp + 4u, (size_t)argument_count * 4u))
        stop_guest("D3D9 COM arguments are unmapped", target);
    uint32_t arguments[16];
    for (int i = 0; i < argument_count; ++i)
        arguments[i] = rd32(cpu->esp + 4u + (uint32_t)i * 4u);
    uint32_t result = 0u;
    int dispatched = ac1_d3d9_dispatch(target, arguments,
                                       (uint32_t)argument_count, &result);
    if (dispatched != 1) {
        last_error = dispatched == -2 ? 120u : 87u;
        stop_guest(ac1_d3d9_last_error()[0]
                   ? ac1_d3d9_last_error()
                   : "D3D9 COM dispatcher rejected the guest call", target);
    }
    cpu->eax = result;
    if (trace_imports && argument_count == 7 &&
        target == 0x7e000000u + 1u * 0x1000u + 16u * 0x10u) {
        fprintf(stderr,
                "AC1 milestone: IDirect3D9::CreateDevice HRESULT=0x%08x adapter=%u device_type=%u hwnd=0x%08x\n",
                result, arguments[1], arguments[2], arguments[3]);
    }
    return_stdcall(cpu, (uint32_t)argument_count * 4u);
    scheduler_checkpoint();
    return 1;
}
#endif

static uint32_t current_host_thread_id(void)
{
    if (active_guest_thread) return active_guest_thread->id;
#ifdef _WIN32
    return (uint32_t)GetCurrentThreadId();
#else
    return (uint32_t)syscall(SYS_gettid);
#endif
}

static int guest_string_equal(uint32_t address, const char *expected)
{
    if (!address || !expected) return 0;
    for (size_t i = 0; i < 260u; ++i) {
        const uint8_t *byte = (const uint8_t *)guest_memory_try_ptr(address + (uint32_t)i, 1u);
        if (!byte) return 0;
        unsigned char a = *byte;
        unsigned char b = (unsigned char)expected[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 0;
}

static const import_record *find_named_import(const char *dll, uint32_t name_address)
{
    if (!name_address || name_address <= 0xFFFFu) return NULL;
    char name[160];
    size_t length = 0;
    for (; length + 1u < sizeof(name); ++length) {
        const uint8_t *byte = (const uint8_t *)guest_memory_try_ptr(name_address + (uint32_t)length, 1u);
        if (!byte) return NULL;
        name[length] = (char)*byte;
        if (!name[length]) break;
    }
    if (length + 1u >= sizeof(name) || name[length]) return NULL;
    for (size_t i = 0; i < import_count; ++i)
        if (!imports[i].by_ordinal && equal_ascii_fold(imports[i].dll, dll) &&
            !strcmp(imports[i].name, name)) return &imports[i];
    return NULL;
}

static int handle_module_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    if (!strcmp(import->name, "GetModuleHandleA")) {
        uint32_t name = rd32(cpu->esp + 4u);
        if (!name) cpu->eax = image_base;
        else if (guest_string_equal(name, "kernel32.dll")) cpu->eax = AC1_KERNEL32_HANDLE;
        else if (bink_image_mapped && guest_string_equal(name, "binkw32.dll"))
            cpu->eax = bink_image.load_base;
        else { cpu->eax = 0; last_error = 126u; }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(import->name, "GetModuleFileNameA")) {
        uint32_t module = rd32(cpu->esp + 4u);
        uint32_t output_address = rd32(cpu->esp + 8u);
        uint32_t capacity = rd32(cpu->esp + 12u);
        const char *module_path = NULL;
        if (!module || module == image_base)
            module_path = exe_path_copy;
        else if (bink_image_mapped && module == bink_image.load_base)
            module_path = bink_module_path;
        if (!module_path) {
            cpu->eax = 0u;
            last_error = 126u;
        } else if (!capacity) {
            cpu->eax = 0u;
            last_error = 122u;
        } else {
            char *output = (char *)guest_memory_try_ptr(output_address, capacity);
            if (!output) stop_guest("GetModuleFileNameA output is unmapped", import->token);
            size_t length = strlen(module_path);
            if (length + 1u <= capacity) {
                memcpy(output, module_path, length + 1u);
                cpu->eax = (uint32_t)length;
            } else {
                memcpy(output, module_path, capacity);
                cpu->eax = capacity;
                last_error = 122u;
            }
        }
        return_stdcall(cpu, 12u);
        return 1;
    }
    if (!strcmp(import->name, "GetProcAddress")) {
        uint32_t module = rd32(cpu->esp + 4u);
        uint32_t name = rd32(cpu->esp + 8u);
        const import_record *resolved = NULL;
        uint32_t bink_export = 0u;
        char export_error[192] = {0};
        if (module == AC1_KERNEL32_HANDLE)
            resolved = find_named_import("KERNEL32.dll", name);
        else if (module == image_base)
            resolved = find_named_import("AssassinsCreed_Dx9.exe", name);
        else if (bink_image_mapped && module == bink_image.load_base) {
            if (name <= 0xffffu) {
                (void)pe32_export_by_ordinal(&bink_image, (uint16_t)name,
                                             &bink_export, export_error,
                                             sizeof(export_error));
            } else {
                char export_name[256];
                size_t remaining = 0u;
                int terminated = 0;
                while (remaining < sizeof(export_name)) {
                    if (name > UINT32_MAX - (uint32_t)remaining ||
                        !guest_memory_try_ptr(name + (uint32_t)remaining, 1u))
                        stop_guest("GetProcAddress Bink export name is unmapped", import->token);
                    export_name[remaining] = (char)rd8(name + (uint32_t)remaining);
                    if (!export_name[remaining]) {
                        terminated = 1;
                        break;
                    }
                    ++remaining;
                }
                if (!terminated)
                    stop_guest("GetProcAddress Bink export name is unterminated", import->token);
                (void)pe32_export_by_name(&bink_image, export_name, &bink_export,
                                          export_error, sizeof(export_error));
            }
        }
        cpu->eax = resolved ? resolved->token : bink_export;
        if (!cpu->eax) last_error = 127u; /* ERROR_PROC_NOT_FOUND */
        return_stdcall(cpu, 8u);
        return 1;
    }
    return 0;
}

static int handle_heap_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    const char *name = import->name;
    if (!strcmp(name, "GetProcessHeap")) {
        if (!process_heap) stop_guest("process heap is not initialized", import->token);
        cpu->eax = AC1_PROCESS_HEAP_HANDLE;
        return_stdcall(cpu, 0);
        return 1;
    }
    if (!strcmp(name, "GetLastError")) {
        cpu->eax = last_error;
        return_stdcall(cpu, 0);
        return 1;
    }
    if (!strcmp(name, "SetLastError")) {
        last_error = rd32(cpu->esp + 4u);
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "FreeEnvironmentStringsW")) {
        uint32_t block = rd32(cpu->esp + 4u);
        cpu->eax = block && process_heap && pcrecomp_heap_free(process_heap, block);
        if (!cpu->eax) last_error = 87u;
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "HeapCreate")) {
        uint32_t flags = rd32(cpu->esp + 4u);
        uint32_t initial_size = rd32(cpu->esp + 8u);
        uint32_t maximum_size = rd32(cpu->esp + 12u);
        if ((flags & ~1u) || maximum_size)
            stop_guest("HeapCreate requires a growable non-executable heap", import->token);
        size_t slot;
        for (slot = 0; slot < AC1_MAX_PRIVATE_HEAPS; ++slot)
            if (!private_heaps[slot].heap) break;
        if (slot == AC1_MAX_PRIVATE_HEAPS) {
            cpu->eax = 0;
            last_error = 8u;
        } else {
            pcrecomp_heap *heap = pcrecomp_heap_create(initial_size);
            if (!heap) {
                cpu->eax = 0;
                last_error = 8u;
            } else {
                private_heaps[slot].handle = AC1_PRIVATE_HEAP_BASE + (uint32_t)slot * 4u;
                private_heaps[slot].heap = heap;
                cpu->eax = private_heaps[slot].handle;
            }
        }
        return_stdcall(cpu, 12u);
        return 1;
    }
    if (!strcmp(name, "HeapDestroy")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        size_t slot;
        for (slot = 0; slot < AC1_MAX_PRIVATE_HEAPS; ++slot)
            if (private_heaps[slot].handle == handle && private_heaps[slot].heap) break;
        if (slot == AC1_MAX_PRIVATE_HEAPS) {
            cpu->eax = 0;
            last_error = handle == AC1_PROCESS_HEAP_HANDLE ? 87u : 6u;
        } else {
            pcrecomp_heap_destroy(private_heaps[slot].heap);
            private_heaps[slot].heap = NULL;
            private_heaps[slot].handle = 0;
            cpu->eax = 1;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "GetCurrentProcessId")) {
#ifdef _WIN32
        cpu->eax = (uint32_t)GetCurrentProcessId();
#else
        cpu->eax = (uint32_t)getpid();
#endif
        return_stdcall(cpu, 0);
        return 1;
    }
    if (!strcmp(name, "GetCurrentProcess")) {
        cpu->eax = UINT32_MAX; /* Current-process pseudo handle. */
        return_stdcall(cpu, 0u);
        return 1;
    }
    if (!strcmp(name, "GetProcessAffinityMask")) {
        uint32_t process = rd32(cpu->esp + 4u);
        uint32_t process_mask_address = rd32(cpu->esp + 8u);
        uint32_t system_mask_address = rd32(cpu->esp + 12u);
        uint8_t *process_mask = (uint8_t *)guest_memory_try_ptr(process_mask_address, 4u);
        uint8_t *system_mask = (uint8_t *)guest_memory_try_ptr(system_mask_address, 4u);
        if (process != UINT32_MAX || !process_mask || !system_mask) {
            cpu->eax = 0u;
            last_error = process != UINT32_MAX ? 6u : 87u;
        } else {
#ifdef _WIN32
            SYSTEM_INFO host_info;
            GetSystemInfo(&host_info);
            uint32_t count = host_info.dwNumberOfProcessors;
#else
            long host_count = sysconf(_SC_NPROCESSORS_ONLN);
            uint32_t count = host_count > 0 ? (uint32_t)host_count : 1u;
#endif
            if (!count) count = 1u;
            if (count > 32u) count = 32u;
            uint32_t mask = count == 32u ? UINT32_MAX : ((1u << count) - 1u);
            wr32(process_mask_address, mask);
            wr32(system_mask_address, mask);
            guest_affinity_mask = mask;
            guest_thread_affinity_mask = mask;
            cpu->eax = 1u;
        }
        return_stdcall(cpu, 12u);
        return 1;
    }
    if (!strcmp(name, "TlsAlloc")) {
        uint32_t index;
        for (index = 0; index < AC1_TLS_SLOTS; ++index)
            if (!tls_used[index]) break;
        if (index == AC1_TLS_SLOTS) {
            cpu->eax = UINT32_MAX;
            last_error = 8u;
        } else {
            tls_used[index] = 1u;
            tls_values[index] = 0u;
            cpu->eax = index;
        }
        return_stdcall(cpu, 0u);
        return 1;
    }
    if (!strcmp(name, "TlsFree")) {
        uint32_t index = rd32(cpu->esp + 4u);
        if (index >= AC1_TLS_SLOTS || !tls_used[index]) {
            cpu->eax = 0u;
            last_error = 87u;
        } else {
            tls_used[index] = 0u;
            tls_values[index] = 0u;
            cpu->eax = 1u;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "TlsGetValue")) {
        uint32_t index = rd32(cpu->esp + 4u);
        if (index >= AC1_TLS_SLOTS || !tls_used[index]) {
            cpu->eax = 0u;
            last_error = 87u;
        } else {
            cpu->eax = tls_values[index];
            last_error = 0u;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "TlsSetValue")) {
        uint32_t index = rd32(cpu->esp + 4u);
        if (index >= AC1_TLS_SLOTS || !tls_used[index]) {
            cpu->eax = 0u;
            last_error = 87u;
        } else {
            tls_values[index] = rd32(cpu->esp + 8u);
            cpu->eax = 1u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(name, "GetCurrentThreadId")) {
        cpu->eax = current_host_thread_id();
        return_stdcall(cpu, 0);
        return 1;
    }
    if (!strcmp(name, "GetCurrentThread")) {
        cpu->eax = UINT32_MAX - 1u; /* Current-thread pseudo handle. */
        return_stdcall(cpu, 0u);
        return 1;
    }
    if (!strcmp(name, "SetThreadAffinityMask")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        uint32_t mask = rd32(cpu->esp + 8u);
        ac1_guest_thread *thread = thread_for_handle(handle);
        if (handle == UINT32_MAX - 1u) thread = active_guest_thread;
        if (!thread || !mask || (guest_affinity_mask && (mask & ~guest_affinity_mask))) {
            cpu->eax = 0u;
            last_error = !thread ? 6u : 87u;
        } else {
            uint32_t previous = thread->affinity_mask ? thread->affinity_mask : guest_affinity_mask;
            thread->affinity_mask = mask;
            if (thread == active_guest_thread) guest_thread_affinity_mask = mask;
            cpu->eax = previous;
            last_error = 0u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(name, "GetTickCount")) {
        cpu->eax = (uint32_t)(guest_monotonic_ns() / UINT64_C(1000000));
        return_stdcall(cpu, 0);
        return 1;
    }
    int allocate = !strcmp(name, "HeapAlloc");
    int release = !strcmp(name, "HeapFree");
    int resize = !strcmp(name, "HeapReAlloc");
    int size = !strcmp(name, "HeapSize");
    if (!allocate && !release && !resize && !size) return 0;
    pcrecomp_heap *heap = heap_by_handle(rd32(cpu->esp + 4u));
    if (!heap) {
        cpu->eax = size ? UINT32_MAX : 0;
        last_error = 6u; /* ERROR_INVALID_HANDLE */
        return_stdcall(cpu, resize ? 16u : 12u);
        return 1;
    }

    uint32_t flags = rd32(cpu->esp + 8u);
    if (allocate) {
        if (flags & ~9u) stop_guest("HeapAlloc flags are unsupported", import->token);
        cpu->eax = pcrecomp_heap_alloc(heap, rd32(cpu->esp + 12u), !!(flags & 8u));
        return_stdcall(cpu, 12u);
    } else if (release) {
        if (flags & ~1u) stop_guest("HeapFree flags are unsupported", import->token);
        uint32_t address = rd32(cpu->esp + 12u);
        cpu->eax = address && pcrecomp_heap_free(heap, address);
        if (!cpu->eax) last_error = 87u; /* ERROR_INVALID_PARAMETER */
        return_stdcall(cpu, 12u);
    } else if (resize) {
        if (flags & ~25u) stop_guest("HeapReAlloc flags are unsupported", import->token);
        cpu->eax = pcrecomp_heap_realloc(heap, rd32(cpu->esp + 12u),
                                         rd32(cpu->esp + 16u), !!(flags & 8u),
                                         !!(flags & 0x10u));
        return_stdcall(cpu, 16u);
    } else {
        if (flags & ~1u) stop_guest("HeapSize flags are unsupported", import->token);
        cpu->eax = pcrecomp_heap_size(heap, rd32(cpu->esp + 12u));
        if (cpu->eax == UINT32_MAX) last_error = 87u;
        return_stdcall(cpu, 12u);
    }
    return 1;
}

static int handle_critical_section_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    const char *name = import->name;
    int init_spin = !strcmp(name, "InitializeCriticalSectionAndSpinCount");
    int init = !strcmp(name, "InitializeCriticalSection");
    int destroy = !strcmp(name, "DeleteCriticalSection");
    int enter = !strcmp(name, "EnterCriticalSection");
    int leave = !strcmp(name, "LeaveCriticalSection");
    int try_enter = !strcmp(name, "TryEnterCriticalSection");
    if (!init_spin && !init && !destroy && !enter && !leave && !try_enter) return 0;

    uint32_t address = rd32(cpu->esp + 4u);
    uint8_t *section = (uint8_t *)guest_memory_try_ptr(address, 24u);
    if (!section) stop_guest("critical-section object is unmapped", import->token);
    if (init_spin || init) {
        uint32_t spin = init_spin ? rd32(cpu->esp + 8u) : 0u;
        memset(section, 0, 24u);
        wr32(address + 4u, UINT32_MAX);
        wr32(address + 20u, spin);
        cpu->eax = 1u;
        return_stdcall(cpu, init_spin ? 8u : 4u);
        return 1;
    }
    if (destroy) {
        memset(section, 0, 24u);
        wr32(address + 4u, UINT32_MAX);
        return_stdcall(cpu, 4u);
        return 1;
    }

    uint32_t owner = rd32(address + 12u);
    uint32_t thread = current_host_thread_id();
    if (enter || try_enter) {
        if (owner == thread) {
            (void)guest_atomic_xadd32(address + 4u, 1u);
            wr32(address + 8u, rd32(address + 8u) + 1u);
            cpu->eax = 1u;
        } else if (!owner) {
            uint32_t prior = guest_atomic_xadd32(address + 4u, 1u);
            if (prior == UINT32_MAX) {
                wr32(address + 8u, 1u);
                wr32(address + 12u, thread);
                cpu->eax = 1u;
            } else {
                (void)guest_atomic_xadd32(address + 4u, UINT32_MAX);
                cpu->eax = 0u;
                if (enter) {
                    if (!active_guest_thread || !active_scheduler)
                        stop_guest("contended EnterCriticalSection has no guest scheduler", import->token);
                    if (trace_imports)
                        fprintf(stderr,
                                "AC1 CS wait: thread=%u section=0x%08x owner=%u lockcount=%u\n",
                                active_guest_thread->id, address, owner,
                                rd32(address + 4u));
                    active_guest_thread->blocked = 1;
                    active_guest_thread->wait_kind = AC1_WAIT_CRITICAL_SECTION;
                    active_guest_thread->wait_handle = address;
                    active_guest_thread->wait_deadline = 0u;
                }
            }
        } else {
            cpu->eax = 0u;
            if (enter) {
                if (!active_guest_thread || !active_scheduler)
                    stop_guest("contended EnterCriticalSection has no guest scheduler", import->token);
                if (trace_imports)
                    fprintf(stderr,
                            "AC1 CS wait: thread=%u section=0x%08x owner=%u lockcount=%u\n",
                            active_guest_thread->id, address, owner,
                            rd32(address + 4u));
                active_guest_thread->blocked = 1;
                active_guest_thread->wait_kind = AC1_WAIT_CRITICAL_SECTION;
                active_guest_thread->wait_handle = address;
                active_guest_thread->wait_deadline = 0u;
            }
        }
        return_stdcall(cpu, 4u);
        return 1;
    }

    if (owner != thread || !rd32(address + 8u))
        stop_guest("LeaveCriticalSection called by a non-owner", import->token);
    uint32_t recursion = rd32(address + 8u) - 1u;
    wr32(address + 8u, recursion);
    if (!recursion) {
        wr32(address + 12u, 0u);
    }
    (void)guest_atomic_xadd32(address + 4u, UINT32_MAX);
    if (!recursion) {
        if (active_scheduler) {
            scheduler_lock(active_scheduler);
            for (unsigned i = 0; i < active_scheduler->count; ++i) {
                ac1_guest_thread *waiter = &active_scheduler->threads[i];
                if (!waiter->blocked || waiter->finished ||
                    waiter->wait_kind != AC1_WAIT_CRITICAL_SECTION ||
                    waiter->wait_handle != address)
                    continue;
                uint32_t previous = guest_atomic_xadd32(address + 4u, 1u);
                if (previous != UINT32_MAX) {
                    (void)guest_atomic_xadd32(address + 4u, UINT32_MAX);
                    break;
                }
                wr32(address + 8u, 1u);
                wr32(address + 12u, waiter->id);
                waiter->blocked = 0;
                waiter->wait_result = 0u;
                if (trace_imports)
                    fprintf(stderr,
                            "AC1 CS handoff: section=0x%08x waiter=%u lockcount=%u recursion=%u\n",
                            address, waiter->id, rd32(address + 4u),
                            rd32(address + 8u));
                scheduler_notify(active_scheduler);
                break;
            }
            scheduler_unlock(active_scheduler);
        }
    }
    return_stdcall(cpu, 4u);
    return 1;
}

static int handle_interlocked_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    const char *name = import->name;
    uint32_t address = rd32(cpu->esp + 4u);
    if (!strcmp(name, "InterlockedIncrement")) {
        cpu->eax = guest_atomic_xadd32(address, 1u) + 1u;
        return_stdcall(cpu, 4u);
    } else if (!strcmp(name, "InterlockedDecrement")) {
        cpu->eax = guest_atomic_xadd32(address, UINT32_MAX) - 1u;
        return_stdcall(cpu, 4u);
    } else if (!strcmp(name, "InterlockedExchange")) {
        cpu->eax = guest_atomic_xchg32(address, rd32(cpu->esp + 8u));
        return_stdcall(cpu, 8u);
    } else if (!strcmp(name, "InterlockedExchangeAdd")) {
        cpu->eax = guest_atomic_xadd32(address, rd32(cpu->esp + 8u));
        return_stdcall(cpu, 8u);
    } else {
        return 0;
    }
    return 1;
}

static int handle_stdio_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    const char *name = import->name;
    if (!strcmp(name, "GetStdHandle")) {
        int32_t which = (int32_t)rd32(cpu->esp + 4u);
        if (which == -10) cpu->eax = AC1_STD_INPUT_HANDLE;
        else if (which == -11) cpu->eax = AC1_STD_OUTPUT_HANDLE;
        else if (which == -12) cpu->eax = AC1_STD_ERROR_HANDLE;
        else { cpu->eax = 0u; last_error = 87u; }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "SetHandleCount")) {
        uint32_t requested = rd32(cpu->esp + 4u);
        if (requested < 3u) requested = 3u;
        if (requested > 2048u) requested = 2048u;
        max_guest_handles = requested;
        cpu->eax = requested;
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "GetFileType")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        cpu->eax = (handle == AC1_STD_INPUT_HANDLE || handle == AC1_STD_OUTPUT_HANDLE ||
                    handle == AC1_STD_ERROR_HANDLE) ? 2u : 0u; /* FILE_TYPE_CHAR */
        if (!cpu->eax) last_error = 6u;
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "GetConsoleMode")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        uint32_t output = rd32(cpu->esp + 8u);
        if (!guest_memory_try_ptr(output, 4u))
            stop_guest("GetConsoleMode output is unmapped", import->token);
        if (handle == AC1_STD_INPUT_HANDLE) {
            wr32(output, 0x0007u); /* processed, line and echo input */
            cpu->eax = 1u;
        } else if (handle == AC1_STD_OUTPUT_HANDLE || handle == AC1_STD_ERROR_HANDLE) {
            wr32(output, 0x0003u); /* processed output and wrap at end of line */
            cpu->eax = 1u;
        } else {
            cpu->eax = 0u;
            last_error = 6u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(name, "SetConsoleMode")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        (void)rd32(cpu->esp + 8u);
        cpu->eax = (handle == AC1_STD_INPUT_HANDLE || handle == AC1_STD_OUTPUT_HANDLE ||
                    handle == AC1_STD_ERROR_HANDLE);
        if (!cpu->eax) last_error = 6u;
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(name, "ReadFile") || !strcmp(name, "WriteFile")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        uint32_t buffer = rd32(cpu->esp + 8u);
        uint32_t count = rd32(cpu->esp + 12u);
        uint32_t transferred = rd32(cpu->esp + 16u);
        uint32_t overlapped = rd32(cpu->esp + 20u);
        FILE *stream = handle == AC1_STD_INPUT_HANDLE ? stdin :
                       handle == AC1_STD_OUTPUT_HANDLE ? stdout :
                       handle == AC1_STD_ERROR_HANDLE ? stderr : NULL;
        /* Console stdio owns only the synthetic standard handles. Let file
         * service tokens continue to handle ReadFile/WriteFile themselves. */
        if (!stream) return 0;
        void *guest_buffer = count ? guest_memory_try_ptr(buffer, count) : NULL;
        if (!stream || overlapped || (count && !guest_buffer) ||
            !guest_memory_try_ptr(transferred, 4u)) {
            cpu->eax = 0u;
            last_error = 87u;
        } else {
            size_t amount;
            if (!strcmp(name, "ReadFile")) {
                amount = fread(guest_buffer, 1u, count, stream);
                if (ferror(stream)) { cpu->eax = 0u; last_error = 5u; }
                else cpu->eax = 1u;
            } else {
                amount = fwrite(guest_buffer, 1u, count, stream);
                (void)fflush(stream);
                if (amount != count) { cpu->eax = 0u; last_error = 29u; }
                else cpu->eax = 1u;
            }
            wr32(transferred, (uint32_t)amount);
        }
        return_stdcall(cpu, 20u);
        return 1;
    }
    return 0;
}

static sync_object *sync_object_for_handle(uint32_t handle)
{
    if (handle < AC1_SYNC_HANDLE_BASE || (handle - AC1_SYNC_HANDLE_BASE) % 4u)
        return NULL;
    uint32_t slot = (handle - AC1_SYNC_HANDLE_BASE) / 4u;
    if (slot >= AC1_MAX_SYNC_OBJECTS || sync_objects[slot].handle != handle)
        return NULL;
    return &sync_objects[slot];
}

static int copy_guest_string(uint32_t address, char *output, size_t capacity)
{
    if (!address || !output || !capacity) return 0;
    for (size_t i = 0; i + 1u < capacity; ++i) {
        uint8_t *character = (uint8_t *)guest_memory_try_ptr(address + (uint32_t)i, 1u);
        if (!character) return 0;
        output[i] = (char)*character;
        if (!*character) return 1;
    }
    output[capacity - 1u] = '\0';
    return 0;
}

static char *trim_profile_text(char *text)
{
    while (*text && (unsigned char)*text <= ' ') ++text;
    size_t length = strlen(text);
    while (length && (unsigned char)text[length - 1u] <= ' ')
        text[--length] = '\0';
    return text;
}

static void trim_profile_trailing_blanks(char *text)
{
    size_t length = strlen(text);
    while (length && (unsigned char)text[length - 1u] <= ' ')
        text[--length] = '\0';
}

static int profile_component_is_parent(const char *path)
{
    const char *component = path;
    for (const char *cursor = path;; ++cursor) {
        if (*cursor == '\\' || *cursor == '/' || *cursor == '\0') {
            if (cursor - component == 2 && component[0] == '.' && component[1] == '.')
                return 1;
            if (!*cursor) break;
            component = cursor + 1;
        }
    }
    return 0;
}

static int profile_path_is_absolute(const char *path)
{
    if (!path || !*path) return 0;
#ifdef _WIN32
    size_t length = strlen(path);
    return (length >= 3u &&
            ((path[0] >= 'A' && path[0] <= 'Z') ||
             (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
           (length >= 2u && path[0] == '\\' && path[1] == '\\');
#elif defined(__ANDROID__)
    size_t length = strlen(path);
    int drive_absolute = length >= 3u &&
        (((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
    return path[0] == '/' || drive_absolute;
#else
    return path[0] == '/';
#endif
}

#ifdef __ANDROID__
static int android_guest_path_is_root(const char *path)
{
    char normalized[32768];
    size_t length;
    if (!path || (length = strlen(path)) >= sizeof(normalized)) return 0;
    memcpy(normalized, path, length + 1u);
    for (size_t i = 0u; i < length; ++i)
        if (normalized[i] == '/') normalized[i] = '\\';
    return equal_ascii_fold(normalized, guest_current_directory);
}

/* Maps guest paths under C:\\AC1 to the imported host game folder. POSIX
 * absolute paths, notably the app-private userdata paths returned by the
 * special-folder shim, remain host-native. */
static int android_guest_path_to_game_relative(const char *guest_path,
                                               char *relative_path,
                                               size_t capacity)
{
    char normalized[32768];
    char root[32768];
    size_t length, guest_length, root_length;
    if (!guest_path || !relative_path || !capacity ||
        (length = strlen(guest_path)) >= sizeof(normalized)) return 0;
    memcpy(normalized, guest_path, length + 1u);
    guest_length = length;
    for (size_t i = 0u; i < length; ++i)
        if (normalized[i] == '\\') normalized[i] = '/';
    root_length = strlen(guest_current_directory);
    if (root_length >= sizeof(root)) return 0;
    memcpy(root, guest_current_directory, root_length + 1u);
    for (size_t i = 0u; i < root_length; ++i)
        if (root[i] == '\\') root[i] = '/';
    root_length = strlen(root);
    if (guest_length <= root_length || normalized[root_length] != '/') return 0;
    for (size_t i = 0u; i < root_length; ++i) {
        unsigned char a = (unsigned char)normalized[i];
        unsigned char b = (unsigned char)root[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    const char *suffix = normalized + root_length + 1u;
    if (!*suffix || profile_component_is_parent(suffix) ||
        strlen(suffix) >= capacity) return 0;
    memcpy(relative_path, suffix, strlen(suffix) + 1u);
    return 1;
}
#endif

static int resolve_profile_path(const char *guest_path, char *host_path, size_t capacity)
{
    if (!guest_path || !*guest_path || !host_path || !capacity) return 0;
#ifdef __ANDROID__
    if (guest_path[0] == '/') {
        int written = snprintf(host_path, capacity, "%s", guest_path);
        return written >= 0 && (size_t)written < capacity;
    }
    char relative[32768];
    if (!android_guest_path_to_game_relative(guest_path, relative,
                                             sizeof(relative))) return 0;
    int written = snprintf(host_path, capacity, "%s/%s",
                           host_game_directory, relative);
    return written >= 0 && (size_t)written < capacity;
#else
#ifdef _WIN32
    size_t length = strlen(guest_path);
    int absolute = (length >= 3u &&
                    ((guest_path[0] >= 'A' && guest_path[0] <= 'Z') ||
                     (guest_path[0] >= 'a' && guest_path[0] <= 'z')) &&
                    guest_path[1] == ':' &&
                    (guest_path[2] == '\\' || guest_path[2] == '/')) ||
                   (length >= 2u && guest_path[0] == '\\' && guest_path[1] == '\\');
#else
    int absolute = guest_path[0] == '/';
#endif
    if (!absolute && profile_component_is_parent(guest_path)) return 0;
    int written;
    if (absolute) {
        written = snprintf(host_path, capacity, "%s", guest_path);
    } else {
#ifdef _WIN32
        const char *separator = "\\";
#else
        const char *separator = "/";
#endif
        written = snprintf(host_path, capacity, "%s%s%s",
                           guest_current_directory, separator, guest_path);
    }
    if (written < 0 || (size_t)written >= capacity) return 0;
#ifndef _WIN32
    for (char *cursor = host_path; *cursor; ++cursor)
        if (*cursor == '\\') *cursor = '/';
#endif
    return 1;
#endif
}

static int profile_path_is_in_userdata(const char *path)
{
    char root[32768];
    char normalized_path[32768];
    char normalized_root[32768];
    if (!get_user_data_root(root, sizeof(root)) ||
        strlen(path) >= sizeof(normalized_path) ||
        strlen(root) >= sizeof(normalized_root) ||
        profile_component_is_parent(path))
        return 0;
    memcpy(normalized_path, path, strlen(path) + 1u);
    memcpy(normalized_root, root, strlen(root) + 1u);
    for (char *cursor = normalized_path; *cursor; ++cursor)
        if (*cursor == '\\') *cursor = '/';
    for (char *cursor = normalized_root; *cursor; ++cursor)
        if (*cursor == '\\') *cursor = '/';
    size_t root_length = strlen(normalized_root);
    while (root_length > 1u && normalized_root[root_length - 1u] == '/')
        normalized_root[--root_length] = '\0';
    if (strlen(normalized_path) <= root_length || normalized_path[root_length] != '/')
        return 0;
    for (size_t i = 0u; i < root_length; ++i) {
        unsigned char path_char = (unsigned char)normalized_path[i];
        unsigned char root_char = (unsigned char)normalized_root[i];
        if (path_char >= 'A' && path_char <= 'Z') path_char += 'a' - 'A';
        if (root_char >= 'A' && root_char <= 'Z') root_char += 'a' - 'A';
        if (path_char != root_char) return 0;
    }
    return 1;
}

static int profile_write_value(const char *path, const char *section,
                               const char *key, const char *value)
{
    enum { PROFILE_FILE_LIMIT = 1024 * 1024 };
    FILE *input = fopen(path, "rb");
    char *old_data = NULL;
    size_t old_length = 0u;
    if (input) {
        if (fseek(input, 0, SEEK_END) != 0) { fclose(input); return 0; }
        long file_length = ftell(input);
        if (file_length < 0 || file_length > PROFILE_FILE_LIMIT ||
            fseek(input, 0, SEEK_SET) != 0) {
            fclose(input);
            return 0;
        }
        old_length = (size_t)file_length;
        old_data = (char *)malloc(old_length + 1u);
        if (!old_data) { fclose(input); return 0; }
        size_t read_length = fread(old_data, 1u, old_length, input);
        fclose(input);
        if (read_length != old_length) { free(old_data); return 0; }
        old_data[old_length] = '\0';
    } else if (errno != ENOENT) {
        return 0;
    }

    size_t section_length = strlen(section);
    size_t key_length = strlen(key);
    size_t value_length = value ? strlen(value) : 0u;
    size_t extra = section_length + key_length + value_length + 16u;
    if (old_length > PROFILE_FILE_LIMIT - extra) { free(old_data); return 0; }
    size_t output_capacity = old_length + extra;
    char *output = (char *)malloc(output_capacity + 1u);
    if (!output) { free(old_data); return 0; }

    size_t out_length = 0u;
    size_t offset = 0u;
    int in_section = 0;
    int section_seen = 0;
    int key_written = 0;
    while (offset < old_length) {
        size_t line_start = offset;
        while (offset < old_length && old_data[offset] != '\n') ++offset;
        size_t line_end = offset;
        if (offset < old_length) ++offset;
        size_t content_end = line_end;
        if (content_end > line_start && old_data[content_end - 1u] == '\r')
            --content_end;
        size_t line_length = content_end - line_start;
        char *line = (char *)malloc(line_length + 1u);
        if (!line) { free(output); free(old_data); return 0; }
        memcpy(line, old_data + line_start, line_length);
        line[line_length] = '\0';
        char *trimmed = trim_profile_text(line);
        if (*trimmed == '[') {
            char *end = strchr(trimmed + 1u, ']');
            if (end) {
                if (in_section && !key_written && value) {
                    int amount = snprintf(output + out_length, output_capacity + 1u - out_length,
                                          "%s=%s\r\n", key, value);
                    if (amount < 0 || (size_t)amount >= output_capacity + 1u - out_length) {
                        free(line); free(output); free(old_data); return 0;
                    }
                    out_length += (size_t)amount;
                    key_written = 1;
                }
                *end = '\0';
                in_section = equal_ascii_fold(trim_profile_text(trimmed + 1u), section);
                if (in_section) section_seen = 1;
            } else {
                in_section = 0;
            }
        } else if (in_section) {
            char *equals = strchr(trimmed, '=');
            if (equals) {
                *equals = '\0';
                if (equal_ascii_fold(trim_profile_text(trimmed), key)) {
                    if (!key_written && value) {
                        int amount = snprintf(output + out_length, output_capacity + 1u - out_length,
                                              "%s=%s\r\n", key, value);
                        if (amount < 0 || (size_t)amount >= output_capacity + 1u - out_length) {
                            free(line); free(output); free(old_data); return 0;
                        }
                        out_length += (size_t)amount;
                        key_written = 1;
                    }
                    free(line);
                    continue;
                }
            }
        }
        free(line);
        size_t raw_length = offset - line_start;
        if (raw_length > output_capacity - out_length) {
            free(output); free(old_data); return 0;
        }
        memcpy(output + out_length, old_data + line_start, raw_length);
        out_length += raw_length;
    }

    if (in_section && !key_written && value) {
        if (out_length && output[out_length - 1u] != '\n') {
            if (out_length + 2u > output_capacity) {
                free(output); free(old_data); return 0;
            }
            output[out_length++] = '\r';
            output[out_length++] = '\n';
        }
        int amount = snprintf(output + out_length, output_capacity + 1u - out_length,
                              "%s=%s\r\n", key, value);
        if (amount < 0 || (size_t)amount >= output_capacity + 1u - out_length) {
            free(output); free(old_data); return 0;
        }
        out_length += (size_t)amount;
        key_written = 1;
    }
    if (!section_seen && value) {
        if (out_length && output[out_length - 1u] != '\n') {
            if (out_length + 2u > output_capacity) {
                free(output); free(old_data); return 0;
            }
            output[out_length++] = '\r';
            output[out_length++] = '\n';
        }
        int amount = snprintf(output + out_length, output_capacity + 1u - out_length,
                              "[%s]\r\n%s=%s\r\n", section, key, value);
        if (amount < 0 || (size_t)amount >= output_capacity + 1u - out_length) {
            free(output); free(old_data); return 0;
        }
        out_length += (size_t)amount;
    }

    output[out_length] = '\0';
    FILE *stream = fopen(path, "wb");
    if (!stream) { free(output); free(old_data); return 0; }
    int ok = fwrite(output, 1u, out_length, stream) == out_length && fflush(stream) == 0;
    if (fclose(stream) != 0) ok = 0;
    free(output);
    free(old_data);
    return ok;
}

static int profile_get_value(const char *path, const char *section,
                             const char *key, const char *fallback,
                             char *value, size_t value_capacity, int *file_opened)
{
    FILE *stream = fopen(path, "rb");
    if (file_opened) *file_opened = stream != NULL;
    if (!stream) {
        size_t length = strlen(fallback);
        if (length >= value_capacity) length = value_capacity - 1u;
        memcpy(value, fallback, length);
        value[length] = '\0';
        return 0;
    }

    char line[4096];
    int in_section = 0;
    int found = 0;
    while (fgets(line, sizeof(line), stream)) {
        char *entry = trim_profile_text(line);
        if (!*entry || *entry == ';' || *entry == '#') continue;
        if (*entry == '[') {
            char *end = strchr(entry + 1, ']');
            if (!end) {
                in_section = 0;
                continue;
            }
            *end = '\0';
            in_section = equal_ascii_fold(trim_profile_text(entry + 1), section);
            continue;
        }
        if (!in_section) continue;
        char *equals = strchr(entry, '=');
        if (!equals) continue;
        *equals = '\0';
        char *entry_key = trim_profile_text(entry);
        if (!equal_ascii_fold(entry_key, key)) continue;
        char *entry_value = trim_profile_text(equals + 1);
        size_t length = strlen(entry_value);
        if (length >= value_capacity) length = value_capacity - 1u;
        memcpy(value, entry_value, length);
        value[length] = '\0';
        found = 1;
        break;
    }
    fclose(stream);
    if (!found) {
        size_t length = strlen(fallback);
        if (length >= value_capacity) length = value_capacity - 1u;
        memcpy(value, fallback, length);
        value[length] = '\0';
    }
    return found;
}

static int profile_enumerate_sections(const char *path, char *output,
                                      size_t capacity, int *file_opened)
{
    if (file_opened) *file_opened = 0;
    if (!capacity) return 0;
    output[0] = '\0';
    if (capacity == 1u) return 0;

    FILE *stream = fopen(path, "rb");
    if (!stream) return 0;
    if (file_opened) *file_opened = 1;
    size_t used = 0u;
    char line[4096];
    while (fgets(line, sizeof(line), stream)) {
        char *entry = trim_profile_text(line);
        if (*entry != '[') continue;
        char *end = strchr(entry + 1u, ']');
        if (!end) continue;
        *end = '\0';
        char *name = trim_profile_text(entry + 1u);
        size_t length = strlen(name);
        if (!length) continue;

        if (capacity < 2u || used > capacity - 2u ||
            length >= capacity - 2u - used) {
            fclose(stream);
            size_t max_chars = capacity - 2u;
            if (used > max_chars) used = max_chars;
            size_t available = max_chars - used;
            size_t copied = length < available ? length : available;
            if (copied) memcpy(output + used, name, copied);
            output[used + copied] = '\0';
            output[used + copied + 1u] = '\0';
            return (int)max_chars;
        }
        memcpy(output + used, name, length);
        used += length;
        output[used++] = '\0';
        output[used] = '\0';
    }
    fclose(stream);
    return (int)used;
}

static int handle_profile_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "KERNEL32.dll") ||
        (strcmp(import->name, "GetPrivateProfileStringA") &&
         strcmp(import->name, "GetPrivateProfileSectionNamesA") &&
         strcmp(import->name, "GetPrivateProfileIntA") &&
         strcmp(import->name, "WritePrivateProfileStringA")))
        return 0;

    if (!strcmp(import->name, "GetPrivateProfileSectionNamesA")) {
        const uint32_t *arguments = (const uint32_t *)guest_memory_try_ptr(
            cpu->esp + 4u, 12u);
        if (!arguments)
            stop_guest("GetPrivateProfileSectionNamesA arguments are unmapped", import->token);
        uint32_t output_address = arguments[0];
        uint32_t capacity = arguments[1];
        uint32_t file_address = arguments[2];
        char guest_path[32768] = "";
        if (!file_address ||
            !copy_guest_string(file_address, guest_path, sizeof(guest_path)))
            stop_guest("GetPrivateProfileSectionNamesA filename is null, unmapped, or too long",
                       import->token);
        if (capacity && !guest_memory_try_ptr(output_address, capacity))
            stop_guest("GetPrivateProfileSectionNamesA output is unmapped", import->token);
        if (!profile_path_is_absolute(guest_path))
            stop_guest("GetPrivateProfileSectionNamesA relative paths require Windows-directory lookup",
                       import->token);

        char host_path[32768];
        if (!resolve_profile_path(guest_path, host_path, sizeof(host_path))) {
            if (capacity) {
                char *output = (char *)guest_memory_try_ptr(output_address, capacity);
                output[0] = '\0';
                if (capacity > 1u) output[1] = '\0';
            }
            cpu->eax = 0u;
            last_error = 87u; /* ERROR_INVALID_PARAMETER */
            if (trace_imports)
                fprintf(stderr, "AC1 profile section names: unresolved file=\"%s\" size=%u\n",
                        guest_path, capacity);
            return_stdcall(cpu, 12u);
            return 1;
        }

        int file_opened = 0;
        cpu->eax = capacity
            ? (uint32_t)profile_enumerate_sections(
                host_path, (char *)guest_memory_try_ptr(output_address, capacity),
                capacity, &file_opened)
            : 0u;
        if (file_opened) last_error = 0u;
        else if (capacity >= 2u) last_error = (errno == ENOENT) ? 2u : 5u;
        if (trace_imports)
            fprintf(stderr, "AC1 profile section names: chars=%u file_opened=%d size=%u file=\"%s\"\n",
                    cpu->eax, file_opened, capacity, guest_path);
        return_stdcall(cpu, 12u);
        return 1;
    }

    if (!strcmp(import->name, "GetPrivateProfileIntA")) {
        const uint32_t *arguments = (const uint32_t *)guest_memory_try_ptr(cpu->esp + 4u, 16u);
        if (!arguments)
            stop_guest("GetPrivateProfileIntA arguments are unmapped", import->token);
        char section[512] = "";
        char key[512] = "";
        char guest_path[32768] = "";
        if (!arguments[0] || !arguments[1] || !arguments[3] ||
            !copy_guest_string(arguments[0], section, sizeof(section)) ||
            !copy_guest_string(arguments[1], key, sizeof(key)) ||
            !copy_guest_string(arguments[3], guest_path, sizeof(guest_path)))
            stop_guest("GetPrivateProfileIntA requires mapped section, key, and filename strings",
                       import->token);
        if (!profile_path_is_absolute(guest_path))
            stop_guest("GetPrivateProfileIntA relative paths require Windows-directory lookup",
                       import->token);

        char host_path[32768];
        if (!resolve_profile_path(guest_path, host_path, sizeof(host_path))) {
            cpu->eax = arguments[2];
            last_error = 87u; /* ERROR_INVALID_PARAMETER */
            return_stdcall(cpu, 16u);
            return 1;
        }

        char value[256];
        int file_opened = 0;
        int found = profile_get_value(host_path, section, key, "",
                                      value, sizeof(value), &file_opened);
        uint32_t result = ac1_profile_int_parse(value, found, arguments[2]);
        cpu->eax = result;
        last_error = file_opened ? 0u : ((errno == ENOENT) ? 2u : 5u);
        if (trace_imports)
            fprintf(stderr, "AC1 profile integer: section=\"%s\" key=\"%s\" value=\"%s\" found=%d file_opened=%d result=0x%08x file=\"%s\"\n",
                    section, key, found ? value : "<default>", found, file_opened,
                    result, guest_path);
        return_stdcall(cpu, 16u);
        return 1;
    }

    if (!strcmp(import->name, "WritePrivateProfileStringA")) {
        const uint32_t *arguments = (const uint32_t *)guest_memory_try_ptr(cpu->esp + 4u, 16u);
        if (!arguments)
            stop_guest("WritePrivateProfileStringA arguments are unmapped", import->token);
        char section[512] = "";
        char key[512] = "";
        char value[4096] = "";
        char guest_path[32768] = "";
        if ((arguments[0] && !copy_guest_string(arguments[0], section, sizeof(section))) ||
            (arguments[1] && !copy_guest_string(arguments[1], key, sizeof(key))) ||
            (arguments[2] && !copy_guest_string(arguments[2], value, sizeof(value))) ||
            (arguments[3] && !copy_guest_string(arguments[3], guest_path, sizeof(guest_path))))
            stop_guest("WritePrivateProfileStringA string argument is unmapped or too long", import->token);
        if (trace_imports)
            fprintf(stderr, "AC1 profile write args: section=%s key=%s value=\"%s\" file=%s\n",
                    arguments[0] ? section : "<null>", arguments[1] ? key : "<null>",
                    arguments[2] ? value : "<null>", arguments[3] ? guest_path : "<null>");
        if (!arguments[0] || !arguments[1] || !arguments[3])
            stop_guest("WritePrivateProfileStringA section/key/file form is unsupported", import->token);
        if (!profile_path_is_absolute(guest_path))
            stop_guest("WritePrivateProfileStringA relative paths require Windows-directory lookup",
                       import->token);
        char host_path[32768];
        if (!resolve_profile_path(guest_path, host_path, sizeof(host_path)) ||
            !profile_path_is_in_userdata(host_path))
            stop_guest("WritePrivateProfileStringA path is outside the userdata root", import->token);
        char parent_path[32768];
        size_t path_length = strlen(host_path);
        if (path_length >= sizeof(parent_path))
            stop_guest("WritePrivateProfileStringA path is too long", import->token);
        memcpy(parent_path, host_path, path_length + 1u);
        char *slash = strrchr(parent_path, '/');
        char *backslash = strrchr(parent_path, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        if (!slash) stop_guest("WritePrivateProfileStringA path has no parent directory", import->token);
        *slash = '\0';
        if (!ensure_directory_tree(parent_path)) {
            cpu->eax = 0u;
            last_error = 5u;
        } else {
            cpu->eax = profile_write_value(host_path, section, key,
                                           arguments[2] ? value : NULL) ? 1u : 0u;
            last_error = cpu->eax ? 0u : 5u;
        }
        return_stdcall(cpu, 16u);
        return 1;
    }

    uint32_t argument_address = cpu->esp + 4u;
    const uint32_t *arguments = (const uint32_t *)guest_memory_try_ptr(argument_address, 24u);
    if (!arguments) stop_guest("GetPrivateProfileStringA arguments are unmapped", import->token);
    uint32_t app_address = arguments[0];
    uint32_t key_address = arguments[1];
    uint32_t fallback_address = arguments[2];
    uint32_t output_address = arguments[3];
    uint32_t capacity = arguments[4];
    uint32_t file_address = arguments[5];
    char section[512] = "";
    char key[512] = "";
    char fallback[4096] = "";
    char guest_path[32768] = "";
    if ((app_address && !copy_guest_string(app_address, section, sizeof(section))) ||
        (key_address && !copy_guest_string(key_address, key, sizeof(key))) ||
        (fallback_address && !copy_guest_string(fallback_address, fallback, sizeof(fallback))) ||
        (file_address && !copy_guest_string(file_address, guest_path, sizeof(guest_path))))
        stop_guest("GetPrivateProfileStringA string argument is unmapped or too long", import->token);
    trim_profile_trailing_blanks(fallback);
    if (trace_imports)
        fprintf(stderr, "AC1 profile args: section=%s key=%s default=\"%s\" file=%s output=0x%08x size=%u\n",
                app_address ? section : "<null>", key_address ? key : "<null>",
                fallback, file_address ? guest_path : "<null>", output_address, capacity);
    if (!file_address)
        stop_guest("GetPrivateProfileStringA null filename is unsupported", import->token);
    if (!profile_path_is_absolute(guest_path))
        stop_guest("GetPrivateProfileStringA relative paths require Windows-directory lookup",
                   import->token);

    char host_path[32768];
    if (!resolve_profile_path(guest_path, host_path, sizeof(host_path))) {
        cpu->eax = 0u;
        last_error = 87u; /* ERROR_INVALID_PARAMETER */
        if (capacity && guest_memory_try_ptr(output_address, capacity))
            ((char *)guest_memory_try_ptr(output_address, capacity))[0] = '\0';
        return_stdcall(cpu, 24u);
        return 1;
    }
    if (trace_imports)
        fprintf(stderr, "AC1 profile read: section=\"%s\" key=\"%s\" default=\"%s\" file=\"%s\" host=\"%s\"\n",
                app_address ? section : "<null>", key_address ? key : "<null>",
                fallback, guest_path, host_path);

    if (!app_address && !key_address) {
        char *output = capacity
            ? (char *)guest_memory_try_ptr(output_address, capacity) : NULL;
        if (capacity && !output)
            stop_guest("GetPrivateProfileStringA section-list output is unmapped", import->token);
        int file_opened = 0;
        cpu->eax = capacity
            ? (uint32_t)profile_enumerate_sections(host_path, output, capacity, &file_opened)
            : 0u;
        if (file_opened) last_error = 0u;
        else if (capacity >= 2u) last_error = (errno == ENOENT) ? 2u : 5u;
        if (trace_imports)
            fprintf(stderr, "AC1 profile section enumeration: chars=%u file_opened=%d\n",
                    cpu->eax, file_opened);
        return_stdcall(cpu, 24u);
        return 1;
    }
    if (!app_address || !key_address)
        stop_guest("GetPrivateProfileStringA key enumeration form is unsupported", import->token);

    char value[4096];
    int file_opened = 0;
    int value_found = profile_get_value(host_path, section, key, fallback,
                                        value, sizeof(value), &file_opened);
    if (!file_opened) last_error = (errno == ENOENT) ? 2u : 5u;
    else last_error = 0u;
    size_t value_length = strlen(value);
    if (value_found && value_length >= 2u &&
        ((value[0] == '"' && value[value_length - 1u] == '"') ||
         (value[0] == '\'' && value[value_length - 1u] == '\''))) {
        memmove(value, value + 1u, value_length - 2u);
        value[value_length - 2u] = '\0';
    }
    if (capacity) {
        char *output = (char *)guest_memory_try_ptr(output_address, capacity);
        if (!output) stop_guest("GetPrivateProfileStringA output is unmapped", import->token);
        size_t length = strlen(value);
        if (length >= capacity) length = (size_t)capacity - 1u;
        memcpy(output, value, length);
        output[length] = '\0';
        cpu->eax = (uint32_t)length;
    } else {
        cpu->eax = 0u;
    }
    if (trace_imports)
        fprintf(stderr, "AC1 profile result: chars=%u file_opened=%d\n",
                cpu->eax, file_opened);
    return_stdcall(cpu, 24u);
    return 1;
}

static sync_object *sync_object_named(const char *name)
{
    if (!name || !*name) return NULL;
    for (uint32_t slot = 0; slot < AC1_MAX_SYNC_OBJECTS; ++slot)
        if (sync_objects[slot].handle && !strcmp(sync_objects[slot].name, name))
            return &sync_objects[slot];
    return NULL;
}

static int sync_object_is_signaled(sync_object *object)
{
    if (!object) return 0;
#ifdef _WIN32
    if (object->kind == 1u && object->host_event)
        return WaitForSingleObject(object->host_event, 0u) == WAIT_OBJECT_0;
#endif
    return object->signaled != 0u;
}

static void sync_object_consume_event(sync_object *object)
{
    if (!object || object->kind != 1u || object->manual_reset) return;
#ifdef _WIN32
    if (object->host_event) return; /* The zero-time native wait consumed it. */
#endif
    object->signaled = 0u;
}

static void scheduler_refresh_waits(ac1_scheduler *scheduler)
{
    uint64_t now = guest_monotonic_ns();
    for (unsigned i = 0; i < scheduler->count; ++i) {
        ac1_guest_thread *thread = &scheduler->threads[i];
        if (!thread->blocked) continue;
        if (thread->wait_kind == 2) {
            if (thread->wait_deadline && now >= thread->wait_deadline) {
                thread->blocked = 0;
                thread->wait_result = AC1_WAIT_TIMEOUT;
                thread->wait_kind = 0;
            }
            continue;
        }
        if (thread->wait_kind != 1) continue;
        ac1_guest_thread *waited_thread = thread_for_handle(thread->wait_handle);
        if (waited_thread && waited_thread->finished) {
            thread->blocked = 0;
            thread->wait_result = AC1_WAIT_OBJECT_0;
            thread->cpu.eax = AC1_WAIT_OBJECT_0;
            thread->wait_kind = 0;
            continue;
        }
        sync_object *object = sync_object_for_handle(thread->wait_handle);
        if (object) {
            int signaled = object->kind == 1u
                ? sync_object_is_signaled(object) : object->count != 0u;
            if (signaled) {
                if (object->kind == 1u) sync_object_consume_event(object);
                if (object->kind == 2u) --object->count;
                thread->blocked = 0;
                thread->wait_result = AC1_WAIT_OBJECT_0;
                thread->cpu.eax = AC1_WAIT_OBJECT_0;
                thread->wait_kind = 0;
                continue;
            }
        }
        if (thread->wait_deadline && now >= thread->wait_deadline) {
            thread->blocked = 0;
            thread->wait_result = AC1_WAIT_TIMEOUT;
            thread->cpu.eax = AC1_WAIT_TIMEOUT;
            thread->wait_kind = 0;
        }
    }
}

static uint32_t scheduler_next_timeout(ac1_scheduler *scheduler)
{
    uint64_t now = guest_monotonic_ns();
    uint64_t nearest = UINT64_MAX;
#ifdef _WIN32
    int poll_native_events = 0;
    if (guest_files && ac1_files_has_pending(guest_files)) {
        for (unsigned i = 0; i < scheduler->count; ++i) {
            ac1_guest_thread *thread = &scheduler->threads[i];
            if (!thread->blocked || thread->wait_kind != 1) continue;
            sync_object *object = sync_object_for_handle(thread->wait_handle);
            if (object && object->kind == 1u && object->host_event &&
                object->io_backed) {
                poll_native_events = 1;
                break;
            }
        }
    }
#endif
    for (unsigned i = 0; i < scheduler->count; ++i) {
        ac1_guest_thread *thread = &scheduler->threads[i];
        if (thread->blocked && thread->wait_deadline && thread->wait_deadline < nearest)
            nearest = thread->wait_deadline;
    }
    if (nearest == UINT64_MAX) {
#ifdef _WIN32
        return poll_native_events ? 10u : 0u;
#else
        return 0u;
#endif
    }
    if (nearest <= now) return 1u;
    uint64_t ms = (nearest - now + 999999u) / 1000000u;
#ifdef _WIN32
    if (poll_native_events && ms > 10u) return 10u;
#endif
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)(ms ? ms : 1u);
}

static sync_object *sync_object_create(uint8_t kind)
{
    for (uint32_t slot = 0; slot < AC1_MAX_SYNC_OBJECTS; ++slot) {
        if (!sync_objects[slot].handle) {
            memset(&sync_objects[slot], 0, sizeof(sync_objects[slot]));
            sync_objects[slot].handle = AC1_SYNC_HANDLE_BASE + slot * 4u;
            sync_objects[slot].kind = kind;
            return &sync_objects[slot];
        }
    }
    return NULL;
}

static void sync_objects_destroy(void)
{
#ifdef _WIN32
    for (uint32_t slot = 0; slot < AC1_MAX_SYNC_OBJECTS; ++slot) {
        if (sync_objects[slot].host_event) {
            CloseHandle(sync_objects[slot].host_event);
            sync_objects[slot].host_event = NULL;
        }
    }
#endif
    memset(sync_objects, 0, sizeof(sync_objects));
}

static int handle_sync_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    const char *name = import->name;
    if (!strcmp(name, "CreateEventA")) {
        uint32_t attributes = rd32(cpu->esp + 4u);
        uint32_t manual_reset = rd32(cpu->esp + 8u);
        uint32_t initial_state = rd32(cpu->esp + 12u);
        uint32_t name_address = rd32(cpu->esp + 16u);
        if (attributes)
            stop_guest("CreateEventA security attributes are unsupported", import->token);
        char object_name[256] = {0};
        if (name_address && !copy_guest_string(name_address, object_name, sizeof(object_name)))
            stop_guest("CreateEventA object name is unmapped or too long", import->token);
        sync_object *object = sync_object_named(object_name);
        if (object && object->kind != 1u) { cpu->eax = 0u; last_error = 6u; }
        else if (object) { cpu->eax = object->handle; last_error = 183u; }
        else if (!(object = sync_object_create(1u))) { cpu->eax = 0u; last_error = 8u; }
        else {
            object->manual_reset = manual_reset != 0u;
#ifndef _WIN32
            object->signaled = initial_state != 0u;
#endif
            if (object_name[0]) memcpy(object->name, object_name, strlen(object_name) + 1u);
#ifdef _WIN32
            object->host_event = CreateEventA(NULL, object->manual_reset,
                                               initial_state != 0u, NULL);
            if (!object->host_event) {
                last_error = (uint32_t)GetLastError();
                memset(object, 0, sizeof(*object));
                cpu->eax = 0u;
                return_stdcall(cpu, 16u);
                return 1;
            }
#endif
            cpu->eax = object->handle;
        }
        return_stdcall(cpu, 16u);
        return 1;
    }
    if (!strcmp(name, "CreateSemaphoreA")) {
        uint32_t attributes = rd32(cpu->esp + 4u);
        uint32_t initial = rd32(cpu->esp + 8u);
        uint32_t maximum = rd32(cpu->esp + 12u);
        uint32_t name_address = rd32(cpu->esp + 16u);
        if (attributes)
            stop_guest("CreateSemaphoreA security attributes are unsupported", import->token);
        char object_name[256] = {0};
        if (name_address && !copy_guest_string(name_address, object_name, sizeof(object_name)))
            stop_guest("CreateSemaphoreA object name is unmapped or too long", import->token);
        if (!maximum || initial > maximum) {
            cpu->eax = 0u; last_error = 87u;
        } else {
            sync_object *object = sync_object_named(object_name);
            if (object && object->kind != 2u) { cpu->eax = 0u; last_error = 6u; }
            else if (object) { cpu->eax = object->handle; last_error = 183u; }
            else if (!(object = sync_object_create(2u))) { cpu->eax = 0u; last_error = 8u; }
            else {
                if (object_name[0]) memcpy(object->name, object_name, strlen(object_name) + 1u);
                object->count = initial;
                object->maximum = maximum;
                cpu->eax = object->handle;
            }
        }
        return_stdcall(cpu, 16u);
        return 1;
    }
    if (!strcmp(name, "SetEvent") || !strcmp(name, "ResetEvent")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        sync_object *object = sync_object_for_handle(handle);
        if (!object || object->kind != 1u) {
            cpu->eax = 0u; last_error = 6u;
        } else {
            int native_success = 1;
#ifdef _WIN32
            if (object->host_event)
                native_success = !strcmp(name, "SetEvent")
                    ? SetEvent(object->host_event)
                    : ResetEvent(object->host_event);
#endif
            if (!native_success) {
                cpu->eax = 0u;
#ifdef _WIN32
                last_error = (uint32_t)GetLastError();
#else
                last_error = 6u;
#endif
            } else {
#ifndef _WIN32
                object->signaled = !strcmp(name, "SetEvent");
#endif
                cpu->eax = 1u;
            }
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(name, "ReleaseSemaphore")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        uint32_t release_count = rd32(cpu->esp + 8u);
        uint32_t previous_address = rd32(cpu->esp + 12u);
        sync_object *object = sync_object_for_handle(handle);
        if (!object || object->kind != 2u) {
            cpu->eax = 0u; last_error = 6u;
        } else if (!release_count || release_count > object->maximum - object->count) {
            cpu->eax = 0u; last_error = 298u; /* ERROR_TOO_MANY_POSTS */
        } else {
            if (previous_address) {
                if (!guest_memory_try_ptr(previous_address, 4u))
                    stop_guest("ReleaseSemaphore previous-count output is unmapped", import->token);
                wr32(previous_address, object->count);
            }
            object->count += release_count;
            cpu->eax = 1u;
        }
        return_stdcall(cpu, 12u);
        return 1;
    }
    if (!strcmp(name, "WaitForSingleObject")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        uint32_t timeout = rd32(cpu->esp + 8u);
        ac1_guest_thread *thread = thread_for_handle(handle);
        sync_object *object = sync_object_for_handle(handle);
        if (thread) {
            if (thread->finished) cpu->eax = AC1_WAIT_OBJECT_0;
            else if (!timeout) cpu->eax = AC1_WAIT_TIMEOUT;
            else if (!active_scheduler || !active_guest_thread)
                stop_guest("WaitForSingleObject thread wait lacks scheduler state", import->token);
            else {
                active_guest_thread->blocked = 1;
                active_guest_thread->wait_kind = 1;
                active_guest_thread->wait_handle = handle;
                active_guest_thread->wait_deadline = timeout == UINT32_MAX ? 0u :
                    guest_monotonic_ns() + (uint64_t)timeout * 1000000u;
            }
        } else if (!object) {
            cpu->eax = UINT32_MAX; last_error = 6u;
        } else {
            int signaled = object->kind == 1u
                ? sync_object_is_signaled(object) : object->count != 0u;
            if (signaled) {
                if (object->kind == 1u) sync_object_consume_event(object);
                if (object->kind == 2u) --object->count;
                cpu->eax = AC1_WAIT_OBJECT_0;
            } else if (!timeout) cpu->eax = AC1_WAIT_TIMEOUT;
            else if (!active_scheduler || !active_guest_thread)
                stop_guest("WaitForSingleObject wait lacks scheduler state", import->token);
            else {
                active_guest_thread->blocked = 1;
                active_guest_thread->wait_kind = 1;
                active_guest_thread->wait_handle = handle;
                active_guest_thread->wait_deadline = timeout == UINT32_MAX ? 0u :
                    guest_monotonic_ns() + (uint64_t)timeout * 1000000u;
            }
        }
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!strcmp(name, "CloseHandle")) {
        uint32_t handle = rd32(cpu->esp + 4u);
        ac1_guest_thread *thread = thread_for_handle(handle);
        sync_object *object = sync_object_for_handle(handle);
        if (thread) { thread->closed = 1; cpu->eax = 1u; }
        else if (!object) { cpu->eax = 0u; last_error = 6u; }
        else {
#ifdef _WIN32
            if (object->host_event && !CloseHandle(object->host_event)) {
                cpu->eax = 0u;
                last_error = (uint32_t)GetLastError();
                return_stdcall(cpu, 4u);
                return 1;
            }
#endif
            memset(object, 0, sizeof(*object));
            cpu->eax = 1u;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }
    return 0;
}

static int handle_shell_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "SHELL32.dll"))
        return 0;
    if (!strcmp(import->name, "SHCreateDirectoryExA")) {
        uint32_t path_address = rd32(cpu->esp + 8u);
        uint32_t security_attributes = rd32(cpu->esp + 12u);
        if (security_attributes)
            stop_guest("SHCreateDirectoryExA security attributes are unsupported", import->token);
        char *guest_path = (char *)guest_memory_try_ptr(path_address, 1u);
        if (!guest_path) stop_guest("SHCreateDirectoryExA path is unmapped", import->token);
        char path[32768];
        size_t length = 0;
        while (length + 1u < sizeof(path)) {
            char *ch = (char *)guest_memory_try_ptr(path_address + (uint32_t)length, 1u);
            if (!ch) stop_guest("SHCreateDirectoryExA path is unmapped", import->token);
            path[length] = *ch;
            if (!*ch) break;
            ++length;
        }
        if (length + 1u >= sizeof(path))
            stop_guest("SHCreateDirectoryExA path is too long", import->token);
        char root[32768];
        if (!get_user_data_root(root, sizeof(root))) {
            cpu->eax = 3u;
            return_stdcall(cpu, 12u);
            return 1;
        }
        size_t root_length = strlen(root);
        int in_root = length >= root_length;
        for (size_t i = 0; in_root && i < root_length; ++i) {
            unsigned char a = (unsigned char)path[i], b = (unsigned char)root[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) in_root = 0;
        }
        in_root = in_root && (length == root_length || path[root_length] == '/' || path[root_length] == '\\');
        for (size_t i = 0; in_root && i + 1u < length; ++i) {
            if ((path[i] == '.' && path[i + 1u] == '.') &&
                (i == 0u || path[i - 1u] == '/' || path[i - 1u] == '\\') &&
                (i + 2u == length || path[i + 2u] == '/' || path[i + 2u] == '\\'))
                in_root = 0;
        }
        if (!in_root) cpu->eax = 5u; /* ERROR_ACCESS_DENIED: never create beside the game. */
        else if (directory_exists(path)) cpu->eax = 183u; /* ERROR_ALREADY_EXISTS */
        else if (ensure_directory_tree(path)) cpu->eax = 0u;
        else cpu->eax = 5u;
        return_stdcall(cpu, 12u);
        return 1;
    }
    if (strcmp(import->name, "SHGetSpecialFolderPathA")) return 0;
    uint32_t output_address = rd32(cpu->esp + 8u);
    uint32_t csidl = rd32(cpu->esp + 12u) & 0x7fffu;
    uint32_t create = rd32(cpu->esp + 16u);
    const char *subdirectory = NULL;
    switch (csidl) {
    case 0x05u: subdirectory = "documents"; break;       /* CSIDL_PERSONAL */
    case 0x1au: subdirectory = "roaming"; break;        /* CSIDL_APPDATA */
    case 0x1cu: subdirectory = "local"; break;          /* CSIDL_LOCAL_APPDATA */
    case 0x23u: subdirectory = "common"; break;         /* CSIDL_COMMON_APPDATA */
    default:
        cpu->eax = 0u;
        last_error = 50u; /* ERROR_NOT_SUPPORTED */
        return_stdcall(cpu, 16u);
        return 1;
    }
    char root[32768];
    char path[32768];
    if (!get_user_data_root(root, sizeof(root))) {
        cpu->eax = 0u;
        last_error = 3u;
        return_stdcall(cpu, 16u);
        return 1;
    }
    size_t root_length = strlen(root);
    int separator = root_length && root[root_length - 1u] != '/' && root[root_length - 1u] != '\\';
    int written = snprintf(path, sizeof(path), "%s%s%s", root, separator ? "/" : "", subdirectory);
    if (written < 0 || (size_t)written >= sizeof(path) || (size_t)written >= 260u) {
        cpu->eax = 0u;
        last_error = 206u; /* ERROR_FILENAME_EXCED_RANGE */
        return_stdcall(cpu, 16u);
        return 1;
    }
    char *output = (char *)guest_memory_try_ptr(output_address, 260u);
    if (!output) stop_guest("SHGetSpecialFolderPathA output is unmapped", import->token);
    if (create && !ensure_directory_tree(path)) {
        cpu->eax = 0u;
        last_error = 5u;
    } else {
        memcpy(output, path, (size_t)written + 1u);
        cpu->eax = 1u;
    }
    return_stdcall(cpu, 16u);
    return 1;
}

static int handle_ole_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "ole32.dll"))
        return 0;
    if (!strcmp(import->name, "CoCreateInstance")) {
        static const uint8_t game_explorer_clsid[16] = {
            0x90,0xa9,0x5e,0x9a,0x34,0x30,0x6f,0x4d,
            0x91,0x28,0x01,0xf3,0xc6,0x10,0x22,0xbc
        };
        static const uint8_t game_explorer_iid[16] = {
            0x72,0xfb,0xb2,0xe7,0x28,0xd7,0xb3,0x49,
            0xa5,0xf2,0x18,0xeb,0xf5,0xf1,0x34,0x9e
        };
        uint32_t clsid_address = rd32(cpu->esp + 4u);
        uint32_t outer = rd32(cpu->esp + 8u);
        uint32_t context = rd32(cpu->esp + 12u);
        uint32_t iid_address = rd32(cpu->esp + 16u);
        uint32_t output_address = rd32(cpu->esp + 20u);
        uint8_t *clsid = (uint8_t *)guest_memory_try_ptr(clsid_address, 16u);
        uint8_t *iid = (uint8_t *)guest_memory_try_ptr(iid_address, 16u);
        if (!clsid || !iid)
            stop_guest("CoCreateInstance GUID is unmapped", import->token);
        if (outer || context != 1u || memcmp(clsid, game_explorer_clsid, 16u) ||
            memcmp(iid, game_explorer_iid, 16u))
            stop_guest("unsupported CoCreateInstance class/interface/context", import->token);
        if (!guest_memory_try_ptr(output_address, 4u))
            stop_guest("CoCreateInstance output pointer is unmapped", import->token);
        /* Game Explorer is absent from the Android runtime and current host
         * profile. Windows callers are documented to handle this optional
         * parental-controls component being unregistered. */
        wr32(output_address, 0u);
        cpu->eax = 0x80040154u; /* REGDB_E_CLASSNOTREG */
        return_stdcall(cpu, 20u);
        return 1;
    }
    if (!strcmp(import->name, "CoInitialize") || !strcmp(import->name, "CoInitializeEx")) {
        int initialize_ex = !strcmp(import->name, "CoInitializeEx");
        uint32_t reserved = rd32(cpu->esp + 4u);
        uint32_t flags = initialize_ex ? rd32(cpu->esp + 8u) : 2u; /* CoInitialize is STA. */
        uint32_t model = flags & 3u;
        if (reserved || (model != 0u && model != 2u) || (flags & ~0x0au)) {
            cpu->eax = 0x80070057u; /* E_INVALIDARG */
        } else if (com_init_count && com_apartment_model != model) {
            cpu->eax = 0x80010106u; /* RPC_E_CHANGED_MODE */
        } else {
            if (!com_init_count) com_apartment_model = model;
            ++com_init_count;
            cpu->eax = com_init_count == 1u ? 0u : 1u; /* S_OK / S_FALSE */
        }
        return_stdcall(cpu, initialize_ex ? 8u : 4u);
        return 1;
    }
    if (!strcmp(import->name, "CoUninitialize")) {
        if (com_init_count && !--com_init_count) com_apartment_model = UINT32_MAX;
        return_stdcall(cpu, 0u);
        return 1;
    }
    return 0;
}

static int handle_registry_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "ADVAPI32.dll"))
        return 0;
    if (strcmp(import->name, "RegOpenKeyExA")) return 0;
    uint32_t root = rd32(cpu->esp + 4u);
    uint32_t subkey = rd32(cpu->esp + 8u);
    uint32_t options = rd32(cpu->esp + 12u);
    uint32_t access = rd32(cpu->esp + 16u);
    uint32_t output = rd32(cpu->esp + 20u);
    if (root == 0x80000002u && options == 0u && access == 0x00020019u &&
        guest_string_equal(subkey, "SOFTWARE\\Ubisoft\\Assassin's Creed")) {
        if (!guest_memory_try_ptr(output, 4u))
            stop_guest("RegOpenKeyExA output is unmapped", import->token);
        wr32(output, 0u);
        cpu->eax = 2u; /* ERROR_FILE_NOT_FOUND: no registry-backed install data on this runtime. */
        return_stdcall(cpu, 20u);
        return 1;
    }
    stop_guest("unsupported registry key access", import->token);
    return 1;
}

static int invoke_guest_stdcall_callback(CPU *cpu, uint32_t procedure,
                                         const uint32_t *arguments,
                                         uint32_t argument_count,
                                         uint32_t *result)
{
    if (!cpu || !procedure || !result ||
        (argument_count && !arguments) || argument_count >= UINT32_MAX / 4u)
        return 0;
    uint32_t frame_bytes = (argument_count + 1u) * (uint32_t)sizeof(uint32_t);
    if (cpu->esp < frame_bytes) return 0;
    uint32_t frame_address = cpu->esp - frame_bytes;
    uint32_t *frame = (uint32_t *)guest_memory_try_ptr(frame_address,
                                                       frame_bytes);
    if (!frame) return 0;

    CPU saved = *cpu;
    frame[0] = AC1_RETURN_SENTINEL;
    for (uint32_t i = 0u; i < argument_count; ++i)
        frame[i + 1u] = arguments[i];
    cpu->esp = frame_address;
    dispatch(cpu, procedure);
    *result = cpu->eax;
    *cpu = saved;
    return 1;
}

#ifdef _WIN32
static int window_read_guest(void *context, uint32_t address,
                             void *output, size_t bytes)
{
    (void)context;
    const void *source = guest_memory_try_ptr(address, bytes);
    if (!source || (!output && bytes)) return 0;
    if (bytes) memcpy(output, source, bytes);
    return 1;
}

static int window_write_guest(void *context, uint32_t address,
                              const void *input, size_t bytes)
{
    (void)context;
    void *destination = guest_memory_try_ptr(address, bytes);
    if (!destination || (!input && bytes)) return 0;
    if (bytes) memcpy(destination, input, bytes);
    return 1;
}

static int window_allocate_guest(void *context, size_t bytes,
                                 uint32_t *guest_address)
{
    (void)context;
    if (!process_heap || !guest_address || !bytes || bytes > UINT32_MAX) return 0;
    uint32_t address = pcrecomp_heap_alloc(process_heap, (uint32_t)bytes, 0);
    if (!address) return 0;
    *guest_address = address;
    return 1;
}

static void window_free_guest(void *context, uint32_t guest_address)
{
    (void)context;
    if (process_heap && guest_address) pcrecomp_heap_free(process_heap, guest_address);
}

static int window_invoke_wndproc(void *context, CPU *cpu, uint32_t procedure,
    uint32_t guest_hwnd, uint32_t message, uint32_t wparam, uint32_t lparam,
    uint32_t *result)
{
    (void)context;
    uint32_t arguments[4] = { guest_hwnd, message, wparam, lparam };
    return invoke_guest_stdcall_callback(cpu, procedure, arguments, 4u, result);
}

static uint32_t window_current_guest_thread_id(void *context)
{
    (void)context;
    return active_guest_thread ? active_guest_thread->id : 0u;
}

static int ensure_window_bridge(void)
{
    if (window_bridge) return 1;
    ac1_window_win32_callbacks callbacks = {0};
    callbacks.read_guest = window_read_guest;
    callbacks.write_guest = window_write_guest;
    callbacks.allocate_guest = window_allocate_guest;
    callbacks.free_guest = window_free_guest;
    callbacks.invoke_wndproc = window_invoke_wndproc;
    callbacks.current_guest_thread_id = window_current_guest_thread_id;
    char error[256] = {0};
    window_bridge = ac1_window_win32_create(&callbacks, error, sizeof(error));
    if (!window_bridge)
        stop_guest(error[0] ? error : "cannot initialize Win32 USER32 bridge",
                   last_guest_target);
    return 1;
}

typedef struct {
    CPU *cpu;
    uint32_t procedure;
    uint32_t guest_data;
    uint32_t callbacks_delivered;
    int failed;
    int guest_stopped;
} ac1_monitor_enum_context;

static BOOL CALLBACK invoke_guest_monitor_enum(HMONITOR monitor, HDC dc,
                                               LPRECT rect, LPARAM opaque)
{
    ac1_monitor_enum_context *context =
        (ac1_monitor_enum_context *)(uintptr_t)opaque;
    if (!context || context->failed) return FALSE;
    if (dc) {
        context->failed = 1;
        return FALSE;
    }

    uint32_t guest_monitor = guest_monitor_for_host((uintptr_t)monitor);
    if (!guest_monitor) {
        context->failed = 1;
        return FALSE;
    }

    uint32_t guest_rect = 0u;
    if (rect) {
        if (!process_heap) {
            context->failed = 1;
            return FALSE;
        }
        guest_rect = pcrecomp_heap_alloc(process_heap, sizeof(*rect), 0);
        if (!guest_rect) {
            context->failed = 1;
            return FALSE;
        }
        void *destination = guest_memory_try_ptr(guest_rect, sizeof(*rect));
        if (!destination) {
            pcrecomp_heap_free(process_heap, guest_rect);
            context->failed = 1;
            return FALSE;
        }
        memcpy(destination, rect, sizeof(*rect));
    }

    uint32_t callback_arguments[4] = {
        guest_monitor, 0u, guest_rect, context->guest_data
    };
    uint32_t callback_result = 0u;
    int invoked = invoke_guest_stdcall_callback(
        context->cpu, context->procedure, callback_arguments, 4u,
        &callback_result);
    if (guest_rect) pcrecomp_heap_free(process_heap, guest_rect);
    if (!invoked) {
        context->failed = 1;
        return FALSE;
    }
    ++context->callbacks_delivered;
    if (!callback_result) {
        context->guest_stopped = 1;
        return FALSE;
    }
    return TRUE;
}

static int handle_enum_display_monitors_import(
    CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "USER32.dll") ||
        strcmp(import->name, "EnumDisplayMonitors"))
        return 0;
    if (!guest_memory_try_ptr(cpu->esp + 4u, 4u * sizeof(uint32_t)))
        stop_guest("EnumDisplayMonitors arguments are unmapped", import->token);
    const uint32_t guest_dc = rd32(cpu->esp + 4u);
    const uint32_t guest_rect = rd32(cpu->esp + 8u);
    const uint32_t procedure = rd32(cpu->esp + 12u);
    const uint32_t guest_data = rd32(cpu->esp + 16u);
    if (guest_dc)
        stop_guest("EnumDisplayMonitors received an unsupported non-null HDC",
                   import->token);
    if (!procedure)
        stop_guest("EnumDisplayMonitors received a null guest callback",
                   import->token);

    RECT clip_rect;
    RECT *native_clip_rect = NULL;
    if (guest_rect) {
        const void *source = guest_memory_try_ptr(guest_rect, sizeof(clip_rect));
        if (!source)
            stop_guest("EnumDisplayMonitors clip RECT is unmapped", import->token);
        memcpy(&clip_rect, source, sizeof(clip_rect));
        native_clip_rect = &clip_rect;
    }

    ac1_monitor_enum_context context;
    memset(&context, 0, sizeof(context));
    context.cpu = cpu;
    context.procedure = procedure;
    context.guest_data = guest_data;
    SetLastError(ERROR_SUCCESS);
    BOOL enumerated = EnumDisplayMonitors(
        NULL, native_clip_rect, invoke_guest_monitor_enum,
        (LPARAM)(uintptr_t)&context);
    DWORD native_error = enumerated ? ERROR_SUCCESS : GetLastError();
    if (context.failed)
        stop_guest("EnumDisplayMonitors could not marshal a guest callback",
                   procedure);
    cpu->eax = enumerated ? 1u : 0u;
    if (!enumerated && native_error) last_error = native_error;
    if (trace_imports)
        fprintf(stderr,
                "AC1 EnumDisplayMonitors: clip=0x%08x callback=0x%08x data=0x%08x delivered=%u stopped=%u result=%u error=%lu\n",
                guest_rect, procedure, guest_data, context.callbacks_delivered,
                context.guest_stopped, cpu->eax, (unsigned long)native_error);
    return_stdcall(cpu, 4u * sizeof(uint32_t));
    scheduler_checkpoint();
    return 1;
}
#endif

static int handle_window_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "USER32.dll"))
        return 0;
    if (!strcmp(import->name, "SetWindowsHookExA")) {
        uint32_t hook_type = rd32(cpu->esp + 4u);
        uint32_t callback = rd32(cpu->esp + 8u);
        uint32_t module = rd32(cpu->esp + 12u);
        uint32_t thread_id = rd32(cpu->esp + 16u);
        if (hook_type != 13u || callback != 0x00401145u || module != image_base || thread_id)
            stop_guest("unsupported SetWindowsHookExA hook/callback", import->token);
        /* WH_KEYBOARD_LL is a system-wide Win32 facility. The callback is a
         * 32-bit guest VA and cannot be registered with the host OS or Android. */
        cpu->eax = 0u;
        last_error = 120u; /* ERROR_CALL_NOT_IMPLEMENTED */
        return_stdcall(cpu, 16u);
        return 1;
    }
#ifdef _WIN32
    if (!strcmp(import->name, "EnumDisplayMonitors"))
        return handle_enum_display_monitors_import(cpu, import);
    if (!ensure_window_bridge()) return 0;
    char error[256] = {0};
    int status = ac1_window_win32_call(window_bridge, cpu, import->name,
                                       &last_error, error, sizeof(error));
    if (status == AC1_WINDOW_WIN32_HANDLED) {
        if (trace_imports &&
            (!strcmp(import->name, "RegisterClassExA") ||
             !strcmp(import->name, "CreateWindowExA")))
            fprintf(stderr, "AC1 milestone: %s result=0x%08x guest_call=0x%08x\n",
                    import->name, cpu->eax, last_guest_target);
        return 1;
    }
    if (status == AC1_WINDOW_WIN32_ERROR)
        stop_guest(error[0] ? error : "Win32 USER32 bridge rejected call state",
                   import->token);
#endif
    return 0;
}

static int handle_clock_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    uint64_t value;
    if (!strcmp(import->name, "QueryPerformanceFrequency")) {
#ifdef _WIN32
        LARGE_INTEGER frequency;
        if (!QueryPerformanceFrequency(&frequency)) {
            cpu->eax = 0;
            last_error = GetLastError();
            return_stdcall(cpu, 4u);
            return 1;
        }
        value = (uint64_t)frequency.QuadPart;
#else
        value = UINT64_C(1000000000);
#endif
    } else if (!strcmp(import->name, "QueryPerformanceCounter")) {
#ifdef _WIN32
        LARGE_INTEGER counter;
        if (!QueryPerformanceCounter(&counter)) {
            cpu->eax = 0;
            last_error = GetLastError();
            return_stdcall(cpu, 4u);
            return 1;
        }
        value = (uint64_t)counter.QuadPart;
#else
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            stop_guest("monotonic performance counter failed", import->token);
        value = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
    } else {
        return 0;
    }
    uint32_t output = rd32(cpu->esp + 4u);
    if (!guest_memory_try_ptr(output, 8))
        stop_guest("performance counter output is unmapped", import->token);
    wr32(output, (uint32_t)value);
    wr32(output + 4u, (uint32_t)(value >> 32));
    cpu->eax = 1;
    return_stdcall(cpu, 4u);
    return 1;
}

static int handle_version_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;
    if (!strcmp(import->name, "GetVersion")) {
        /* Match the Windows 7 compatibility profile used by GetVersionExA. */
        cpu->eax = 0x1db10106u; /* 6.1.7601, NT platform */
        return_stdcall(cpu, 0u);
        return 1;
    }
    if (strcmp(import->name, "GetVersionExA")) return 0;
    uint32_t output = rd32(cpu->esp + 4u);
    if (!guest_memory_try_ptr(output, 4u))
        stop_guest("GetVersionExA output is unmapped", import->token);
    uint32_t structure_size = rd32(output);
    if ((structure_size != 148u && structure_size != 156u) ||
        !guest_memory_try_ptr(output, structure_size)) {
        cpu->eax = 0;
        last_error = 87u; /* ERROR_INVALID_PARAMETER */
        return_stdcall(cpu, 4u);
        return 1;
    }
    uint8_t *version = (uint8_t *)guest_memory_try_ptr(output, structure_size);
    memset(version, 0, structure_size);
    wr32(output, structure_size);
    wr32(output + 4u, 6u);       /* Windows 7 compatibility profile. */
    wr32(output + 8u, 1u);
    wr32(output + 12u, 7601u);
    wr32(output + 16u, 2u);      /* VER_PLATFORM_WIN32_NT */
    memcpy(version + 20u, "Service Pack 1", sizeof("Service Pack 1"));
    if (structure_size == 156u) {
        version[148] = 1u;       /* wServicePackMajor */
        version[154] = 1u;       /* VER_NT_WORKSTATION */
    }
    cpu->eax = 1;
    return_stdcall(cpu, 4u);
    return 1;
}

static int handle_file_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;

    if (!strcmp(import->name, "CreateFileA")) {
        uint32_t arguments[7];
        if (!guest_memory_try_ptr(cpu->esp + 4u, sizeof(arguments)))
            stop_guest("CreateFileA arguments are unmapped", import->token);
        for (unsigned i = 0; i < 7u; ++i)
            arguments[i] = rd32(cpu->esp + 4u + i * 4u);

        /* The current file service accepts a bounded single path component. */
        char guest_path[260];
        if (!copy_guest_string(arguments[0], guest_path, sizeof(guest_path)))
            stop_guest("CreateFileA path is unmapped or too long", import->token);
        if (!guest_files)
            stop_guest("CreateFileA called before file service initialization", import->token);

        uint32_t handle = UINT32_MAX;
        uint32_t error = 0u;
        int file_result = ac1_files_open(
            guest_files, guest_path, arguments[1], arguments[2],
            arguments[3] != 0u, arguments[4], arguments[5], arguments[6],
            &handle, &error);
        if (trace_imports)
            fprintf(stderr,
                    "AC1 file probe: path=\"%s\" service=%d handle=0x%08x error=%u\n",
                    guest_path, file_result, handle, error);
        if (file_result == AC1_FILES_UNSUPPORTED) {
            char message[384];
            snprintf(message, sizeof(message),
                     "CreateFileA unsupported form path=\"%s\" access=0x%08x share=0x%08x security=0x%08x disposition=%u flags=0x%08x template=0x%08x",
                     guest_path, arguments[1], arguments[2], arguments[3],
                     arguments[4], arguments[5], arguments[6]);
            stop_guest(message, import->token);
        }
        if (file_result != AC1_FILES_HANDLED)
            stop_guest("CreateFileA file service rejected the call shape", import->token);
        cpu->eax = handle;
        if (handle == UINT32_MAX) last_error = error;
        return_stdcall(cpu, 28u);
        return 1;
    }

    if (!strcmp(import->name, "ReadFile")) {
        if (!guest_files) return 0;
        if (!guest_memory_try_ptr(cpu->esp + 4u, 20u))
            stop_guest("ReadFile arguments are unmapped", import->token);
        const uint32_t handle = rd32(cpu->esp + 4u);
        const uint32_t buffer_address = rd32(cpu->esp + 8u);
        const uint32_t requested = rd32(cpu->esp + 12u);
        const uint32_t transferred_address = rd32(cpu->esp + 16u);
        const uint32_t overlapped_address = rd32(cpu->esp + 20u);
        void *buffer = requested
            ? guest_memory_try_ptr(buffer_address, requested)
            : NULL;
        if (requested && !buffer)
            stop_guest("ReadFile buffer is unmapped", import->token);
        uint32_t bytes_read = 0u;
        uint32_t error = 0u;
        ac1_files_overlapped_snapshot snapshot = {0};
        int file_result;
        if (overlapped_address) {
            if (transferred_address)
                stop_guest("ReadFile overlapped call supplied a byte-count pointer",
                           import->token);
            if (!guest_memory_try_ptr(overlapped_address, 20u))
                stop_guest("ReadFile OVERLAPPED is unmapped", import->token);
            if (trace_imports)
                fprintf(stderr,
                        "AC1 guest OVERLAPPED: address=0x%08x internal=0x%08x internal_high=0x%08x offset=0x%08x:%08x hEvent=0x%08x\n",
                        overlapped_address, rd32(overlapped_address),
                        rd32(overlapped_address + 4u),
                        rd32(overlapped_address + 12u),
                        rd32(overlapped_address + 8u),
                        rd32(overlapped_address + 16u));
            const uint32_t event_handle = rd32(overlapped_address + 16u);
            sync_object *event_object = NULL;
            void *host_event = NULL;
            if (event_handle) {
                event_object = sync_object_for_handle(event_handle);
                if (!event_object || event_object->kind != 1u)
                    stop_guest("ReadFile OVERLAPPED.hEvent is not a guest event handle",
                               import->token);
#ifdef _WIN32
                host_event = event_object->host_event;
                if (!host_event)
                    stop_guest("ReadFile guest event lacks a native event", import->token);
#endif
            }
            file_result = ac1_files_read_overlapped(
                guest_files, handle, buffer, requested, overlapped_address,
                rd32(overlapped_address + 8u),
                rd32(overlapped_address + 12u), host_event, &error, &snapshot);
            if (event_object && file_result == AC1_FILES_HANDLED) {
                event_object->io_backed = error == 997u;
#ifndef _WIN32
                if (error == 0u)
                    event_object->signaled = 1u;
#endif
            }
        } else {
            uint32_t *transferred = (uint32_t *)guest_memory_try_ptr(
                transferred_address, sizeof(uint32_t));
            if (!transferred)
                stop_guest("ReadFile byte-count output is unmapped", import->token);
            file_result = ac1_files_read(
                guest_files, handle, buffer, requested, &bytes_read, &error);
            if (file_result == AC1_FILES_HANDLED)
                *transferred = bytes_read;
        }
        if (file_result == AC1_FILES_NOT_HANDLED) return 0;
        if (file_result != AC1_FILES_HANDLED)
            stop_guest("ReadFile file service rejected the call", import->token);
        if (overlapped_address && snapshot.valid) {
            wr32(overlapped_address, snapshot.internal);
            wr32(overlapped_address + 4u, snapshot.internal_high);
        }
        cpu->eax = error == 0u;
        if (error) last_error = error;
        if (trace_imports && trace_file_read_count++ < 16u) {
            const uint8_t *bytes = (const uint8_t *)buffer;
            fprintf(stderr,
                    "AC1 file read: handle=0x%08x request=%u ovl=0x%08x offset=0x%08x:%08x success=%u bytes=%u error=%u first=",
                    handle, requested, overlapped_address,
                    overlapped_address ? rd32(overlapped_address + 12u) : 0u,
                    overlapped_address ? rd32(overlapped_address + 8u) : 0u,
                    cpu->eax, bytes_read, error);
            for (uint32_t i = 0; !overlapped_address && bytes &&
                                  i < bytes_read && i < 8u; ++i)
                fprintf(stderr, "%02x", bytes[i]);
            fputc('\n', stderr);
        }
        return_stdcall(cpu, 20u);
        return 1;
    }

    if (!strcmp(import->name, "GetOverlappedResult")) {
        if (!guest_files) return 0;
        if (!guest_memory_try_ptr(cpu->esp + 4u, 16u))
            stop_guest("GetOverlappedResult arguments are unmapped", import->token);
        const uint32_t handle = rd32(cpu->esp + 4u);
        const uint32_t overlapped_address = rd32(cpu->esp + 8u);
        const uint32_t transferred_address = rd32(cpu->esp + 12u);
        const uint32_t wait = rd32(cpu->esp + 16u);
        if (!guest_memory_try_ptr(overlapped_address, 20u))
            stop_guest("GetOverlappedResult OVERLAPPED is unmapped", import->token);
        uint32_t *transferred = (uint32_t *)guest_memory_try_ptr(
            transferred_address, sizeof(uint32_t));
        if (!transferred)
            stop_guest("GetOverlappedResult byte-count output is unmapped",
                       import->token);
        uint32_t bytes_read = 0u;
        uint32_t error = 0u;
        ac1_files_overlapped_snapshot snapshot = {0};
        const int file_result = ac1_files_get_overlapped_result(
            guest_files, handle, overlapped_address, wait,
            &bytes_read, &error, &snapshot);
        if (file_result == AC1_FILES_NOT_HANDLED) return 0;
        if (file_result != AC1_FILES_HANDLED)
            stop_guest("GetOverlappedResult file service rejected the call",
                       import->token);
        if (snapshot.valid) {
            wr32(overlapped_address, snapshot.internal);
            wr32(overlapped_address + 4u, snapshot.internal_high);
        }
        cpu->eax = error == 0u;
        if (cpu->eax) *transferred = bytes_read;
        if (error) last_error = error;
        if (trace_imports && trace_file_read_count++ < 16u)
            fprintf(stderr,
                    "AC1 overlapped result: handle=0x%08x ovl=0x%08x wait=%u success=%u bytes=%u error=%u\n",
                    handle, overlapped_address, wait, cpu->eax, bytes_read, error);
        return_stdcall(cpu, 16u);
        return 1;
    }

    if (!strcmp(import->name, "CloseHandle")) {
        if (!guest_files) return 0;
        uint32_t error = 0u;
        uint32_t handle = rd32(cpu->esp + 4u);
        if (!ac1_files_close(guest_files, handle, &error)) return 0;
        cpu->eax = error == 0u;
        if (trace_imports)
            fprintf(stderr,
                    "AC1 file close: handle=0x%08x success=%u error=%u\n",
                    handle, cpu->eax, error);
        if (error) last_error = error;
        return_stdcall(cpu, 4u);
        return 1;
    }
    if (!strcmp(import->name, "SetFilePointer")) {
        if (!guest_files) return 0;
        if (!guest_memory_try_ptr(cpu->esp + 4u, 16u))
            stop_guest("SetFilePointer arguments are unmapped", import->token);
        const uint32_t handle = rd32(cpu->esp + 4u);
        const uint32_t distance_low = rd32(cpu->esp + 8u);
        const uint32_t distance_high_address = rd32(cpu->esp + 12u);
        const uint32_t move_method = rd32(cpu->esp + 16u);
        const int has_distance_high = distance_high_address != 0u;
        uint32_t distance_high = 0u;
        if (has_distance_high) {
            if (!guest_memory_try_ptr(distance_high_address, 4u))
                stop_guest("SetFilePointer high-distance pointer is unmapped",
                           import->token);
            distance_high = rd32(distance_high_address);
        }
        uint32_t position_low = UINT32_MAX;
        uint32_t position_high = 0u;
        uint32_t error = 0u;
        int file_result = ac1_files_seek(
            guest_files, handle, distance_low, has_distance_high,
            distance_high, move_method, &position_low,
            has_distance_high ? &position_high : NULL, &error);
        if (file_result == AC1_FILES_NOT_HANDLED) return 0;
        if (file_result != AC1_FILES_HANDLED)
            stop_guest("SetFilePointer file service rejected the call", import->token);
        if (!error && has_distance_high)
            wr32(distance_high_address, position_high);
        cpu->eax = error ? UINT32_MAX : position_low;
        if (error) last_error = error;
        else if (position_low == UINT32_MAX) last_error = 0u;
        if (trace_imports)
            fprintf(stderr,
                    "AC1 file seek: handle=0x%08x distance=0x%08x:%08x method=%u high_out=0x%08x position=0x%08x:%08x error=%u\n",
                    handle, distance_high, distance_low, move_method,
                    distance_high_address, position_high, position_low, error);
        return_stdcall(cpu, 16u);
        return 1;
    }
    if (!strcmp(import->name, "GetFileSizeEx")) {
        if (!guest_files) return 0;
        if (!guest_memory_try_ptr(cpu->esp + 4u, 8u))
            stop_guest("GetFileSizeEx arguments are unmapped", import->token);
        const uint32_t handle = rd32(cpu->esp + 4u);
        const uint32_t size_address = rd32(cpu->esp + 8u);
        if (!guest_memory_try_ptr(size_address, 8u))
            stop_guest("GetFileSizeEx size output is unmapped", import->token);
        uint64_t size = 0u;
        uint32_t error = 0u;
        int file_result = ac1_files_get_size(guest_files, handle, &size, &error);
        if (file_result == AC1_FILES_NOT_HANDLED) return 0;
        if (file_result != AC1_FILES_HANDLED)
            stop_guest("GetFileSizeEx file service rejected the call", import->token);
        cpu->eax = error == 0u;
        if (!error) {
            wr32(size_address, (uint32_t)size);
            wr32(size_address + 4u, (uint32_t)(size >> 32));
        } else {
            last_error = error;
        }
        if (trace_imports)
            fprintf(stderr,
                    "AC1 file size: handle=0x%08x success=%u size=%llu error=%u\n",
                    handle, cpu->eax, (unsigned long long)size, error);
        return_stdcall(cpu, 8u);
        return 1;
    }
    return 0;
}

static int handle_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal || !equal_ascii_fold(import->dll, "KERNEL32.dll"))
        return 0;

    if (!strcmp(import->name, "GetStartupInfoA")) {
        uint32_t output = rd32(cpu->esp + 4u);
        uint8_t *startup = (uint8_t *)guest_memory_try_ptr(output, 68u);
        if (!startup) stop_guest("GetStartupInfoA output is unmapped", import->token);
        memset(startup, 0, 68u);
        wr32(output, 68u); /* STARTUPINFOA.cb; remaining default fields are zero. */
        return_stdcall(cpu, 4u);
        return 1;
    }

    if (!strcmp(import->name, "GetWindowsDirectoryA")) {
        uint32_t output_address = rd32(cpu->esp + 4u);
        uint32_t capacity = rd32(cpu->esp + 8u);
#ifdef _WIN32
        char *output = capacity
            ? (char *)guest_memory_try_ptr(output_address, capacity)
            : NULL;
        if (capacity && !output)
            stop_guest("GetWindowsDirectoryA output is unmapped", import->token);
        cpu->eax = (uint32_t)GetWindowsDirectoryA(output, capacity);
        if (!cpu->eax) last_error = (uint32_t)GetLastError();
#else
        (void)output_address;
        (void)capacity;
        stop_guest("GetWindowsDirectoryA is unavailable without a Windows host", import->token);
#endif
        return_stdcall(cpu, 8u);
        return 1;
    }

    if (!strcmp(import->name, "GetSystemDirectoryA")) {
        uint32_t output_address = rd32(cpu->esp + 4u);
        uint32_t capacity = rd32(cpu->esp + 8u);
#ifdef _WIN32
        char *output = capacity
            ? (char *)guest_memory_try_ptr(output_address, capacity)
            : NULL;
        if (capacity && !output)
            stop_guest("GetSystemDirectoryA output is unmapped", import->token);
        cpu->eax = (uint32_t)GetSystemDirectoryA(output, capacity);
        if (!cpu->eax) last_error = (uint32_t)GetLastError();
#else
        (void)output_address;
        (void)capacity;
        stop_guest("GetSystemDirectoryA is unavailable without a Windows host", import->token);
#endif
        return_stdcall(cpu, 8u);
        return 1;
    }

    if (!strcmp(import->name, "GetSystemInfo")) {
        uint32_t output = rd32(cpu->esp + 4u);
        uint8_t *info = (uint8_t *)guest_memory_try_ptr(output, 36u);
        if (!info) stop_guest("GetSystemInfo output is unmapped", import->token);
        memset(info, 0, 36u);
#ifdef _WIN32
        SYSTEM_INFO host_info;
        GetSystemInfo(&host_info);
        uint32_t processor_count = host_info.dwNumberOfProcessors;
#else
        long host_count = sysconf(_SC_NPROCESSORS_ONLN);
        uint32_t processor_count = host_count > 0 ? (uint32_t)host_count : 1u;
#endif
        if (!processor_count) processor_count = 1u;
        if (processor_count > 32u) processor_count = 32u;
        wr32(output + 4u, 4096u);             /* guest page size */
        wr32(output + 8u, 0x00010000u);       /* 32-bit Windows user address range */
        wr32(output + 12u, 0x7ffeffffu);
        wr32(output + 16u, processor_count == 32u ? UINT32_MAX : ((1u << processor_count) - 1u));
        wr32(output + 20u, processor_count);
        wr32(output + 24u, 586u);             /* PROCESSOR_INTEL_PENTIUM */
        wr32(output + 28u, 65536u);           /* allocation granularity */
        wr16(output + 32u, 6u);               /* guest x86 processor level */
        wr16(output + 34u, 0u);
        return_stdcall(cpu, 4u);
        return 1;
    }

    if (!strcmp(import->name, "RaiseException")) {
        uint32_t code = rd32(cpu->esp + 4u);
        uint32_t flags = rd32(cpu->esp + 8u);
        uint32_t argument_count = rd32(cpu->esp + 12u);
        uint32_t arguments = rd32(cpu->esp + 16u);
        if (code != 0x406D1388u || flags != 0u || argument_count != 4u ||
            !guest_memory_try_ptr(arguments, 4u * sizeof(uint32_t)) ||
            rd32(arguments) != 0x1000u || !rd32(arguments + 4u) ||
            rd32(arguments + 12u) != 0u)
            stop_guest("unsupported RaiseException (only the MSVC thread-name protocol is recognized)",
                       import->token);
        if (trace_imports)
            fprintf(stderr, "AC1 ignored MSVC thread-name debugger exception on guest thread %u\n",
                    active_guest_thread ? active_guest_thread->id : 0u);
        /* 0x406D1388 is the debugger-only MSVC thread naming protocol. It has
         * no guest program effect once no debugger is attached. */
        return_stdcall(cpu, 16u);
        return 1;
    }

    if (!strcmp(import->name, "VirtualQuery")) {
        if (!guest_memory_try_ptr(cpu->esp + 4u, 12u))
            stop_guest("VirtualQuery arguments are unmapped", import->token);
        const uint32_t query_address = rd32(cpu->esp + 4u);
        const uint32_t information_address = rd32(cpu->esp + 8u);
        const uint32_t information_length = rd32(cpu->esp + 12u);
        ac1_virtual_query_region query_region = {0};
        int tracked_allocation = 0;
        for (size_t i = 0; i < GUEST_MEMORY_MAX_REGIONS; ++i) {
            const uint32_t allocation_base = virtual_alloc_bases[i];
            if (!allocation_base) continue;
            const uint64_t allocation_size =
                guest_memory_region_size(allocation_base);
            if (ac1_virtual_query_region_for_allocation(
                    query_address, allocation_base, allocation_size,
                    &query_region)) {
                tracked_allocation = 1;
                break;
            }
        }
        uint8_t *information = (uint8_t *)guest_memory_try_ptr(
            information_address, 28u);
        if (!information || information_length != 28u || !tracked_allocation)
            stop_guest("VirtualQuery address is not inside a tracked guest private allocation",
                       import->token);
        /* Each tracked VirtualAlloc is one committed, private RW region. */
        wr32(information_address + 0u, query_region.base_address); /* BaseAddress */
        wr32(information_address + 4u, query_region.allocation_base); /* AllocationBase */
        wr32(information_address + 8u, 4u);            /* AllocationProtect */
        wr32(information_address + 12u, query_region.region_size); /* RegionSize */
        wr32(information_address + 16u, 0x1000u);      /* MEM_COMMIT */
        wr32(information_address + 20u, 4u);            /* PAGE_READWRITE */
        wr32(information_address + 24u, 0x20000u);     /* MEM_PRIVATE */
        cpu->eax = 28u;
        return_stdcall(cpu, 12u);
        return 1;
    }

    if (!strcmp(import->name, "VirtualAlloc")) {
        uint32_t requested = rd32(cpu->esp + 4u);
        uint32_t size = rd32(cpu->esp + 8u);
        uint32_t allocation_type = rd32(cpu->esp + 12u);
        uint32_t protection = rd32(cpu->esp + 16u);
        const uint32_t supported_type = 0x1000u | 0x2000u; /* MEM_COMMIT | MEM_RESERVE */
        if (!size || (allocation_type & supported_type) != supported_type ||
            (allocation_type & ~(supported_type | 0x100000u)) ||
            protection != 4u) {
            cpu->eax = 0u;
            last_error = 87u; /* ERROR_INVALID_PARAMETER */
            return_stdcall(cpu, 16u);
            return 1;
        }
        uint64_t rounded = ((uint64_t)size + 0xfffu) & ~UINT64_C(0xfff);
        uint32_t base = requested;
        if (base) {
            base &= ~0xffffu; /* MEM_RESERVE rounds an explicit address to 64 KiB. */
        } else {
            if (rounded > UINT32_MAX) {
                cpu->eax = 0u;
                last_error = 8u; /* ERROR_NOT_ENOUGH_MEMORY */
                return_stdcall(cpu, 16u);
                return 1;
            }
            base = guest_memory_find_gap(0x10000000u, (uint32_t)rounded,
                                         0x10000u, 0x70000000u);
        }
        if (!base || !guest_memory_allocate(base, rounded)) {
            cpu->eax = 0u;
            last_error = 8u; /* ERROR_NOT_ENOUGH_MEMORY */
        } else if (!virtual_allocation_add(base)) {
            guest_memory_release(base);
            cpu->eax = 0u;
            last_error = 8u; /* ERROR_NOT_ENOUGH_MEMORY */
        } else {
            cpu->eax = base;
            last_error = 0u;
        }
        return_stdcall(cpu, 16u);
        return 1;
    }

    if (!strcmp(import->name, "VirtualFree")) {
        uint32_t address = rd32(cpu->esp + 4u);
        uint32_t size = rd32(cpu->esp + 8u);
        uint32_t free_type = rd32(cpu->esp + 12u);
        if (trace_imports)
            fprintf(stderr, "AC1 VirtualFree: eip=0x%08x address=0x%08x size=0x%08x type=0x%08x\n",
                    cpu->eip, address, size, free_type);
        if (free_type == 0x8000u && address && size == 0u &&
            virtual_allocation_contains(address) &&
            guest_memory_region_size(address) != 0u) {
            guest_memory_release(address);
            virtual_allocation_remove(address);
            cpu->eax = 1u;
            last_error = 0u;
        } else if (free_type == 0x4000u) {
            stop_guest("VirtualFree MEM_DECOMMIT is unsupported by the current guest map model",
                       import->token);
        } else {
            cpu->eax = 0u;
            last_error = 87u; /* ERROR_INVALID_PARAMETER */
        }
        return_stdcall(cpu, 12u);
        return 1;
    }

    if (!strcmp(import->name, "SetCurrentDirectoryA")) {
        uint32_t path_address = rd32(cpu->esp + 4u);
        char path[32768];
        size_t length = 0u;
        for (; length + 1u < sizeof(path); ++length) {
            const char *byte = (const char *)guest_memory_try_ptr(
                path_address + (uint32_t)length, 1u);
            if (!byte) stop_guest("SetCurrentDirectoryA path is unmapped", import->token);
            path[length] = *byte;
            if (!path[length]) break;
        }
        if (length + 1u >= sizeof(path) || path[length])
            stop_guest("SetCurrentDirectoryA path is unterminated or too long", import->token);
        while (length > 0u && (path[length - 1u] == '\\' || path[length - 1u] == '/'))
            path[--length] = '\0';
#ifdef _WIN32
        int same_directory = equal_ascii_fold(path, guest_current_directory);
        DWORD attributes = GetFileAttributesA(path);
        int exists = attributes != INVALID_FILE_ATTRIBUTES &&
                     (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#elif defined(__ANDROID__)
        int same_directory = android_guest_path_is_root(path);
        struct stat directory_status;
        int exists = same_directory &&
                     stat(host_game_directory, &directory_status) == 0 &&
                     S_ISDIR(directory_status.st_mode);
#else
        int same_directory = strcmp(path, guest_current_directory) == 0;
        struct stat directory_status;
        int exists = stat(path, &directory_status) == 0 &&
                     S_ISDIR(directory_status.st_mode);
#endif
        if (!exists) {
            cpu->eax = 0u;
            last_error = 3u; /* ERROR_PATH_NOT_FOUND */
        } else if (!same_directory) {
            stop_guest("SetCurrentDirectoryA requested an unmodeled directory change",
                       import->token);
        } else {
            cpu->eax = 1u;
            last_error = 0u;
        }
        return_stdcall(cpu, 4u);
        return 1;
    }

    if (!strcmp(import->name, "GetLogicalDriveStringsA")) {
        uint32_t capacity = rd32(cpu->esp + 4u);
        uint32_t output_address = rd32(cpu->esp + 8u);
        uint32_t result = 0u;
#ifdef _WIN32
        char drives[4096];
        DWORD native_capacity = capacity < sizeof(drives) ? (DWORD)capacity : (DWORD)sizeof(drives);
        memset(drives, 0, sizeof(drives));
        DWORD native_result = GetLogicalDriveStringsA(native_capacity, drives);
        if (!native_result) {
            last_error = GetLastError();
        } else if (capacity && native_result < sizeof(drives)) {
            size_t bytes = (size_t)native_result + 1u;
            if (bytes > capacity) bytes = capacity;
            uint8_t *output = (uint8_t *)guest_memory_try_ptr(output_address, bytes);
            if (!output) stop_guest("GetLogicalDriveStringsA output is unmapped", import->token);
            memcpy(output, drives, bytes);
            result = native_result;
            last_error = 0u;
        } else {
            /* Win32 reports the required character count, including its final
             * NUL, when the supplied buffer cannot hold the full MULTI_SZ. */
            result = native_result;
            last_error = capacity ? 122u : 0u; /* ERROR_INSUFFICIENT_BUFFER */
        }
#else
        /* Android/POSIX has no Win32 logical drives to enumerate. Return the
         * documented failure shape, rather than claiming a synthetic drive. */
        if (capacity) {
            uint8_t *output = (uint8_t *)guest_memory_try_ptr(output_address, capacity);
            if (!output) stop_guest("GetLogicalDriveStringsA output is unmapped", import->token);
            output[0] = 0u;
            if (capacity > 1u) output[1] = 0u;
        }
        last_error = 120u; /* ERROR_CALL_NOT_IMPLEMENTED */
#endif
        cpu->eax = result;
        return_stdcall(cpu, 8u);
        return 1;
    }

    if (!strcmp(import->name, "GetACP")) {
        /* The pinned DX9 runtime's ANSI profile is Windows-1252, also used
         * for CP_ACP in WideCharToMultiByte above. */
        cpu->eax = 1252u;
        return_stdcall(cpu, 0u);
        return 1;
    }

    if (!strcmp(import->name, "GetCurrentDirectoryA")) {
        uint32_t capacity = rd32(cpu->esp + 4u);
        uint32_t output_address = rd32(cpu->esp + 8u);
        size_t length = strlen(guest_current_directory);
        if (length > UINT32_MAX - 1u) {
            cpu->eax = 0u;
            last_error = 206u; /* ERROR_FILENAME_EXCED_RANGE */
        } else if (!capacity) {
            cpu->eax = (uint32_t)length + 1u;
            last_error = 0u;
        } else if (capacity <= length) {
            cpu->eax = (uint32_t)length + 1u;
            last_error = 122u; /* ERROR_INSUFFICIENT_BUFFER */
        } else {
            char *output = (char *)guest_memory_try_ptr(output_address, length + 1u);
            if (!output) stop_guest("GetCurrentDirectoryA output is unmapped", import->token);
            memcpy(output, guest_current_directory, length + 1u);
            cpu->eax = (uint32_t)length;
            last_error = 0u;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }

    if (!strcmp(import->name, "IsValidCodePage")) {
        uint32_t code_page = rd32(cpu->esp + 4u);
        cpu->eax = code_page == 1252u || code_page == 65001u;
        return_stdcall(cpu, 4u);
        return 1;
    }

    if (!strcmp(import->name, "GetCPInfo")) {
        uint32_t code_page = rd32(cpu->esp + 4u);
        uint32_t output_address = rd32(cpu->esp + 8u);
        uint8_t *info = (uint8_t *)guest_memory_try_ptr(output_address, 18u);
        if (!info) stop_guest("GetCPInfo output is unmapped", import->token);
        if (code_page != 1252u && code_page != 65001u) {
            cpu->eax = 0;
            last_error = 87u;
        } else {
            memset(info, 0, 18u);
            wr32(output_address, code_page == 65001u ? 4u : 1u);
            info[4] = '?';
            cpu->eax = 1;
        }
        return_stdcall(cpu, 8u);
        return 1;
    }

    if (!strcmp(import->name, "SetUnhandledExceptionFilter")) {
        uint32_t next_filter = rd32(cpu->esp + 4u);
        cpu->eax = unhandled_exception_filter;
        unhandled_exception_filter = next_filter;
        return_stdcall(cpu, 4u);
        return 1;
    }

    if (!strcmp(import->name, "GetStringTypeW")) {
        uint32_t type = rd32(cpu->esp + 4u);
        uint32_t source_address = rd32(cpu->esp + 8u);
        int32_t requested_units = (int32_t)rd32(cpu->esp + 12u);
        uint32_t output_address = rd32(cpu->esp + 16u);
        if (type != 1u)
            stop_guest("GetStringTypeW classification type is unsupported", import->token);
        if (!source_address || !output_address || requested_units == 0 || requested_units < -1) {
            cpu->eax = 0;
            last_error = 87u;
            return_stdcall(cpu, 16u);
            return 1;
        }
        size_t units = requested_units > 0 ? (size_t)requested_units : 0u;
        if (requested_units == -1) {
            while (units < 0x01000000u) {
                uint16_t *p = (uint16_t *)guest_memory_try_ptr(source_address + (uint32_t)(units * 2u), 2u);
                if (!p) stop_guest("GetStringTypeW input string is unmapped", import->token);
                ++units;
                if (!*p) break;
            }
            if (units == 0x01000000u)
                stop_guest("GetStringTypeW input string is unterminated", import->token);
        }
        const uint16_t *source = (const uint16_t *)guest_memory_try_ptr(source_address, units * 2u);
        uint16_t *output = (uint16_t *)guest_memory_try_ptr(output_address, units * 2u);
        if (!source || !output) stop_guest("GetStringTypeW input/output range is unmapped", import->token);
#ifdef _WIN32
        cpu->eax = GetStringTypeW(type, (LPCWCH)source, (int)units, (LPWORD)output) != 0;
#else
        for (size_t i = 0; i < units; ++i) {
            uint16_t c = source[i];
            uint16_t flags = 0;
            if ((c >= 'A' && c <= 'Z') || (c >= 0x00c0u && c <= 0x00deu && c != 0x00d7u)) flags |= 0x0001u | 0x0100u;
            else if ((c >= 'a' && c <= 'z') || (c >= 0x00dfu && c <= 0x00ffu && c != 0x00f7u)) flags |= 0x0002u | 0x0100u;
            if (c >= '0' && c <= '9') flags |= 0x0004u;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == 0x00a0u) flags |= 0x0008u;
            if (c == ' ' || c == '\t' || c == 0x00a0u) flags |= 0x0040u;
            if ((c >= 0x21u && c <= 0x7eu && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) ||
                (c >= 0x00a1u && c <= 0x00bfu)) flags |= 0x0010u;
            if (c < 0x20u || (c >= 0x7fu && c <= 0x9fu)) flags |= 0x0020u;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) flags |= 0x0080u;
            output[i] = flags;
        }
        cpu->eax = 1u;
#endif
        return_stdcall(cpu, 16u);
        return 1;
    }

    if (!strcmp(import->name, "MultiByteToWideChar")) {
        uint32_t code_page = rd32(cpu->esp + 4u);
        uint32_t flags = rd32(cpu->esp + 8u);
        uint32_t source_address = rd32(cpu->esp + 12u);
        int32_t requested_bytes = (int32_t)rd32(cpu->esp + 16u);
        uint32_t output_address = rd32(cpu->esp + 20u);
        uint32_t output_capacity = rd32(cpu->esp + 24u);
        int utf8 = code_page == 65001u;
        if (code_page != 0u && code_page != 1252u && !utf8)
            stop_guest("MultiByteToWideChar code page is unsupported", import->token);
        if ((utf8 && (flags & ~8u)) || (!utf8 && (flags & ~9u)))
            stop_guest("MultiByteToWideChar flags are unsupported", import->token);
        if (!source_address || requested_bytes == 0 || requested_bytes < -1) {
            cpu->eax = 0;
            last_error = 87u;
            return_stdcall(cpu, 24u);
            return 1;
        }
        size_t bytes;
        if (requested_bytes == -1) {
            bytes = 0;
            while (bytes < 0x01000000u) {
                uint8_t *p = (uint8_t *)guest_memory_try_ptr(source_address + (uint32_t)bytes, 1u);
                if (!p) stop_guest("MultiByteToWideChar input string is unmapped", import->token);
                ++bytes;
                if (!*p) break;
            }
            if (bytes == 0x01000000u)
                stop_guest("MultiByteToWideChar input string is unterminated", import->token);
        } else bytes = (size_t)requested_bytes;
        const uint8_t *source = (const uint8_t *)guest_memory_try_ptr(source_address, bytes);
        if (!source) stop_guest("MultiByteToWideChar input range is unmapped", import->token);
        int valid;
        int strict = utf8 && (flags & 8u);
        size_t required = convert_ansi_to_wide(source, bytes, utf8, NULL, strict, &valid);
        if (!valid && strict) {
            cpu->eax = 0;
            last_error = 1113u; /* ERROR_NO_UNICODE_TRANSLATION */
        } else if (required > INT32_MAX) {
            stop_guest("MultiByteToWideChar output is too large", import->token);
        } else if (!output_address && output_capacity == 0u) {
            cpu->eax = (uint32_t)required;
        } else {
            uint16_t *output = (uint16_t *)guest_memory_try_ptr(output_address,
                                                                (size_t)output_capacity * 2u);
            if (!output || output_capacity < required) {
                cpu->eax = 0;
                last_error = output ? 122u : 87u;
            } else {
                convert_ansi_to_wide(source, bytes, utf8, output, 0, &valid);
                cpu->eax = (uint32_t)required;
            }
        }
        return_stdcall(cpu, 24u);
        return 1;
    }

    if (!strcmp(import->name, "LCMapStringW")) {
        uint32_t locale = rd32(cpu->esp + 4u);
        uint32_t flags = rd32(cpu->esp + 8u);
        uint32_t source_address = rd32(cpu->esp + 12u);
        int32_t requested_units = (int32_t)rd32(cpu->esp + 16u);
        uint32_t output_address = rd32(cpu->esp + 20u);
        uint32_t output_capacity = rd32(cpu->esp + 24u);
        uint32_t case_flags = flags & (0x100u | 0x200u);
        if ((flags & ~0x01000300u) || (case_flags != 0x100u && case_flags != 0x200u))
            stop_guest("LCMapStringW mapping flags are unsupported", import->token);
        if (!source_address || requested_units == 0 || requested_units < -1) {
            cpu->eax = 0;
            last_error = 87u;
            return_stdcall(cpu, 24u);
            return 1;
        }
        size_t units;
        if (requested_units == -1) {
            units = 0;
            while (units < 0x01000000u) {
                uint16_t *p = (uint16_t *)guest_memory_try_ptr(source_address + (uint32_t)(units * 2u), 2u);
                if (!p) stop_guest("LCMapStringW input string is unmapped", import->token);
                ++units;
                if (!*p) break;
            }
            if (units == 0x01000000u)
                stop_guest("LCMapStringW input string is unterminated", import->token);
        } else units = (size_t)requested_units;
        const uint16_t *source = (const uint16_t *)guest_memory_try_ptr(source_address, units * 2u);
        if (!source) stop_guest("LCMapStringW input range is unmapped", import->token);
#ifdef _WIN32
        uint16_t *output = output_address ? (uint16_t *)guest_memory_try_ptr(output_address, (size_t)output_capacity * 2u) : NULL;
        if (output_address && !output) stop_guest("LCMapStringW output range is unmapped", import->token);
        cpu->eax = (uint32_t)LCMapStringW(locale, flags, (LPCWCH)source, (int)units,
                                          (LPWSTR)output, (int)output_capacity);
        if (!cpu->eax && output_address) last_error = 122u;
#else
        (void)locale;
        if (output_address && output_capacity < units) {
            cpu->eax = 0u;
            last_error = 122u;
        } else if (!output_address && output_capacity == 0u) {
            cpu->eax = (uint32_t)units;
        } else {
            uint16_t *output = (uint16_t *)guest_memory_try_ptr(output_address, units * 2u);
            if (!output) stop_guest("LCMapStringW output range is unmapped", import->token);
            for (size_t i = 0; i < units; ++i)
                output[i] = simple_case_map(source[i], case_flags == 0x200u);
            cpu->eax = (uint32_t)units;
        }
#endif
        return_stdcall(cpu, 24u);
        return 1;
    }

    if (!strcmp(import->name, "GetCommandLineA")) {
        if (!process_heap || !exe_path_copy[0])
            stop_guest("GetCommandLineA called before process initialization", import->token);
        if (!command_line_guest_address) {
            size_t path_length = strlen(exe_path_copy);
            /* The launcher supplies only the executable path. Quote it so paths
             * containing spaces retain normal Windows command-line parsing. */
            size_t length = path_length + 3u;
            command_line_guest_address = pcrecomp_heap_alloc(process_heap, (uint32_t)length, 0);
            char *command = (char *)guest_ptr(command_line_guest_address, length);
            if (!command) stop_guest("GetCommandLineA allocation failed", import->token);
            command[0] = '"';
            memcpy(command + 1u, exe_path_copy, path_length);
            command[path_length + 1u] = '"';
            command[path_length + 2u] = '\0';
        }
        cpu->eax = command_line_guest_address;
        return_stdcall(cpu, 0u);
        return 1;
    }

    if (!strcmp(import->name, "GetEnvironmentStringsW")) {
        if (!process_heap) stop_guest("GetEnvironmentStringsW called before process initialization", import->token);
#ifdef _WIN32
        char **entries = _environ;
#else
        char **entries = environ;
#endif
        size_t units = 2u;
        if (entries) {
            for (char **entry = entries; *entry; ++entry) {
                size_t count = utf8_to_utf16_count((const unsigned char *)*entry);
                if (count > (SIZE_MAX / sizeof(uint16_t)) - units - 1u)
                    stop_guest("environment block size overflow", import->token);
                units += count + 1u;
            }
        }
        if (units > UINT32_MAX / sizeof(uint16_t))
            stop_guest("environment block exceeds guest allocation limit", import->token);
        uint32_t block = pcrecomp_heap_alloc(process_heap, (uint32_t)(units * sizeof(uint16_t)), 1);
        uint16_t *destination = (uint16_t *)guest_ptr(block, units * sizeof(uint16_t));
        if (!destination) stop_guest("environment block allocation failed", import->token);
        if (entries) {
            for (char **entry = entries; *entry; ++entry)
                destination = utf8_to_utf16_write(destination, (const unsigned char *)*entry);
        }
        *destination = 0; /* The Windows environment block ends with an extra NUL. */
        cpu->eax = block;
        return_stdcall(cpu, 0u);
        return 1;
    }

    if (!strcmp(import->name, "WideCharToMultiByte")) {
        uint32_t code_page = rd32(cpu->esp + 4u);
        uint32_t flags = rd32(cpu->esp + 8u);
        uint32_t source_address = rd32(cpu->esp + 12u);
        int32_t requested_units = (int32_t)rd32(cpu->esp + 16u);
        uint32_t output_address = rd32(cpu->esp + 20u);
        uint32_t output_capacity = rd32(cpu->esp + 24u);
        uint32_t default_address = rd32(cpu->esp + 28u);
        uint32_t used_default_address = rd32(cpu->esp + 32u);
        int utf8 = code_page == 65001u;
        if (code_page != 0u && code_page != 1252u && !utf8)
            stop_guest("WideCharToMultiByte code page is unsupported", import->token);
        if ((utf8 && (default_address || used_default_address || (flags & ~0x80u))) ||
            (!utf8 && flags))
            stop_guest("WideCharToMultiByte flags/default arguments are unsupported", import->token);
        if (!source_address || requested_units == 0 || requested_units < -1) {
            cpu->eax = 0;
            last_error = 87u;
            return_stdcall(cpu, 32u);
            return 1;
        }
        size_t units;
        if (requested_units == -1) {
            units = 0;
            while (units < 0x01000000u) {
                uint16_t *p = (uint16_t *)guest_memory_try_ptr(source_address + (uint32_t)(units * 2u), 2u);
                if (!p) stop_guest("WideCharToMultiByte input string is unmapped", import->token);
                ++units;
                if (!*p) break;
            }
            if (units == 0x01000000u)
                stop_guest("WideCharToMultiByte input string is unterminated", import->token);
        } else units = (size_t)requested_units;
        const uint16_t *source = (const uint16_t *)guest_memory_try_ptr(source_address, units * 2u);
        if (!source) stop_guest("WideCharToMultiByte input range is unmapped", import->token);
        uint8_t replacement = '?';
        if (default_address) {
            uint8_t *value = (uint8_t *)guest_memory_try_ptr(default_address, 1u);
            if (!value) stop_guest("WideCharToMultiByte default character is unmapped", import->token);
            replacement = *value;
        }
        int used_default = 0;
        size_t required = convert_wide_to_ansi(source, units, utf8, replacement, NULL, &used_default);
        if (required > INT32_MAX) stop_guest("WideCharToMultiByte output is too large", import->token);
        if (used_default_address) {
            uint8_t *value = (uint8_t *)guest_memory_try_ptr(used_default_address, 4u);
            if (!value) stop_guest("WideCharToMultiByte used-default output is unmapped", import->token);
            wr32(used_default_address, (uint32_t)used_default);
        }
        if (!output_address && output_capacity == 0u) {
            cpu->eax = (uint32_t)required;
        } else {
            uint8_t *output = (uint8_t *)guest_memory_try_ptr(output_address, output_capacity);
            if (!output || output_capacity < required) {
                cpu->eax = 0;
                last_error = output ? 122u : 87u;
            } else {
                convert_wide_to_ansi(source, units, utf8, replacement, output, &used_default);
                cpu->eax = (uint32_t)required;
            }
        }
        return_stdcall(cpu, 32u);
        return 1;
    }

    if (strcmp(import->name, "GetSystemTimeAsFileTime")) return 0;

    uint32_t output = rd32(cpu->esp + 4u);
    if (!guest_memory_try_ptr(output, 8))
        stop_guest("GetSystemTimeAsFileTime output is unmapped", import->token);

#ifdef _WIN32
    FILETIME host_filetime;
    GetSystemTimeAsFileTime(&host_filetime);
    uint64_t filetime = ((uint64_t)host_filetime.dwHighDateTime << 32) |
                        host_filetime.dwLowDateTime;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now))
        stop_guest("host UTC clock query failed", import->token);
    const uint64_t epoch_delta = UINT64_C(11644473600);
    if (now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(10000000) - epoch_delta)
        stop_guest("host UTC clock is outside FILETIME range", import->token);
    uint64_t filetime = ((uint64_t)now.tv_sec + epoch_delta) * UINT64_C(10000000) +
                        (uint64_t)now.tv_nsec / 100u;
#endif
    wr32(output, (uint32_t)filetime);
    wr32(output + 4u, (uint32_t)(filetime >> 32));
    return_stdcall(cpu, 4u);
    return 1;
}

static void stop_guest(const char *reason, uint32_t target)
{
    if (active_guest_thread && active_guest_thread->slot != 0u &&
        active_guest_thread->scheduler && runtime_stop_ready) {
        ac1_scheduler *scheduler = active_guest_thread->scheduler;
        scheduler_lock(scheduler);
        int already_failed = scheduler->failed;
        if (!already_failed) {
            scheduler->failed = 1;
            scheduler->failure_target = target;
            scheduler->failure_caller = last_guest_target;
            scheduler->failure_thread_id = active_guest_thread->id;
            scheduler->failure_cpu = active_guest_thread->cpu;
            for (unsigned i = 0; i < 8u; ++i) {
                uint32_t at = active_guest_thread->cpu.esp + i * 4u;
                scheduler->failure_stack[i] = guest_memory_try_ptr(at, 4u) ? rd32(at) : 0u;
            }
            scheduler->failure_object_readable = 0;
            scheduler->failure_vtable_readable = 0;
            if (trace_imports &&
                guest_memory_try_ptr(active_guest_thread->cpu.ecx,
                                     sizeof(scheduler->failure_object_words))) {
                for (unsigned i = 0; i < 8u; ++i)
                    scheduler->failure_object_words[i] =
                        rd32(active_guest_thread->cpu.ecx + i * 4u);
                scheduler->failure_object_readable = 1;
                uint32_t vptr = scheduler->failure_object_words[0];
                if (guest_memory_try_ptr(vptr,
                                         sizeof(scheduler->failure_vtable_words))) {
                    for (unsigned i = 0; i < 8u; ++i)
                        scheduler->failure_vtable_words[i] = rd32(vptr + i * 4u);
                    scheduler->failure_vtable_readable = 1;
                }
            }
            snprintf(scheduler->failure_reason, sizeof(scheduler->failure_reason),
                     "guest thread %u: %s", active_guest_thread->id,
                     reason ? reason : "unsupported guest operation");
            scheduler->owner = 0u;
            scheduler_notify(scheduler);
        }
        scheduler_unlock(scheduler);
        if (already_failed) longjmp(active_guest_thread->exit_env, 1);
    }
    if (runtime_stop_ready) {
        snprintf(stop_reason, sizeof(stop_reason), "%s; target=0x%08x",
                 reason, target);
        snprintf(last_run_error, sizeof(last_run_error), "%s", stop_reason);
        longjmp(stop_runtime, 1);
    }
    fprintf(stderr, "AC1 native runtime: %s; target=0x%08x\n", reason, target);
    abort();
}

static void trace_receiver_snapshot(const CPU *cpu)
{
    if (!trace_imports || !cpu ||
        !guest_memory_try_ptr(cpu->ecx, 8u * sizeof(uint32_t)))
        return;
    uint32_t object_words[8];
    for (unsigned i = 0; i < 8u; ++i)
        object_words[i] = rd32(cpu->ecx + i * 4u);
    fprintf(stderr,
            "AC1 guest receiver: ecx=0x%08x words=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
            cpu->ecx, object_words[0], object_words[1], object_words[2],
            object_words[3], object_words[4], object_words[5],
            object_words[6], object_words[7]);
    if (!guest_memory_try_ptr(object_words[0], 8u * sizeof(uint32_t)))
        return;
    fprintf(stderr,
            "AC1 guest candidate vtable: address=0x%08x slots=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
            object_words[0], rd32(object_words[0]), rd32(object_words[0] + 4u),
            rd32(object_words[0] + 8u), rd32(object_words[0] + 12u),
            rd32(object_words[0] + 16u), rd32(object_words[0] + 20u),
            rd32(object_words[0] + 24u), rd32(object_words[0] + 28u));
}

static int resolve_import(void *context, const char *dll_name,
                          const char *name, uint16_t ordinal,
                          uint32_t *guest_target, char *error,
                          size_t error_capacity)
{
    (void)context;
    if (!dll_name || !guest_target || import_count >= AC1_IMPORT_LIMIT) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "invalid/excessive AC1 import table");
        return 0;
    }
    import_record *record = &imports[import_count];
    memset(record, 0, sizeof(*record));
    snprintf(record->dll, sizeof(record->dll), "%s", dll_name);
    if (name) snprintf(record->name, sizeof(record->name), "%s", name);
    record->ordinal = ordinal;
    record->by_ordinal = name == NULL;
    record->token = AC1_IMPORT_BASE + (uint32_t)import_count * 4u;
    *guest_target = record->token;
    ++import_count;
    return 1;
}

static const import_record *find_import(uint32_t token)
{
    if (token < AC1_IMPORT_BASE || (token - AC1_IMPORT_BASE) % 4u)
        return NULL;
    size_t index = (token - AC1_IMPORT_BASE) / 4u;
    return index < import_count ? &imports[index] : NULL;
}

static int handle_network_import(CPU *cpu, const import_record *import)
{
    if (!import || !equal_ascii_fold(import->dll, "WS2_32.dll")) return 0;
    uint32_t argument_count = 0u;
    if ((import->by_ordinal && import->ordinal == 115u) ||
        (!import->by_ordinal && equal_ascii_fold(import->name, "WSAStartup")))
        argument_count = 2u;
    else if ((import->by_ordinal && import->ordinal == 112u) ||
             (!import->by_ordinal && equal_ascii_fold(import->name, "WSASetLastError")))
        argument_count = 1u;
    uint32_t arguments[2] = {0u, 0u};
    if (argument_count) {
        uint32_t bytes = argument_count * 4u;
        if (!guest_memory_try_ptr(cpu->esp + 4u, bytes))
            stop_guest("Winsock import arguments are unmapped", import->token);
        for (uint32_t i = 0; i < argument_count; ++i)
            arguments[i] = rd32(cpu->esp + 4u + i * 4u);
    }
    uint32_t result = 0u, stack_bytes = 0u;
    if (!ac1_network_import(import->dll,
                            import->by_ordinal ? NULL : import->name,
                            import->ordinal, import->by_ordinal,
                            argument_count ? arguments : NULL,
                            &result, &stack_bytes))
        return 0;
    cpu->eax = result;
    return_stdcall(cpu, stack_bytes);
    return 1;
}

static int handle_crypto_import(CPU *cpu, const import_record *import)
{
    if (!import || import->by_ordinal ||
        !equal_ascii_fold(import->dll, "ADVAPI32.dll"))
        return 0;

    if (!strcmp(import->name, "CryptAcquireContextA")) {
        uint32_t args[5];
        if (!guest_memory_try_ptr(cpu->esp + 4u, sizeof(args)))
            stop_guest("CryptAcquireContextA arguments are unmapped", import->token);
        for (unsigned i = 0u; i < 5u; ++i)
            args[i] = rd32(cpu->esp + 4u + i * 4u);

        uint32_t *provider_out = (uint32_t *)guest_memory_try_ptr(args[0], 4u);
        if (!provider_out)
            stop_guest("CryptAcquireContextA provider output is unmapped", import->token);
        char provider[260];
        const char *provider_value = NULL;
        if (args[2]) {
            if (!copy_guest_string(args[2], provider, sizeof(provider)))
                stop_guest("CryptAcquireContextA provider string is unmapped or too long",
                           import->token);
            provider_value = provider;
        }
        if (args[1] != 0u ||
            !provider_value ||
            strcmp(provider_value, "Microsoft Base Cryptographic Provider v1.0") != 0 ||
            args[3] != 1u ||
            (args[4] != 0xF0000020u && args[4] != 0xF0000028u)) {
            char message[256];
            snprintf(message, sizeof(message),
                     "CryptAcquireContextA unsupported profile container=0x%08x provider=\"%s\" type=%u flags=0x%08x",
                     args[1], provider_value ? provider_value : "<null>", args[3], args[4]);
            stop_guest(message, import->token);
        }

        uint32_t guest_provider = 0u, error = 0u;
        int success = ac1_crypto_acquire(NULL, provider_value, args[3], args[4],
                                         &guest_provider, &error);
        wr32(args[0], guest_provider);
        cpu->eax = success ? 1u : 0u;
        if (!success) last_error = error;
        if (trace_imports)
            fprintf(stderr,
                    "AC1 CryptoAPI: CryptAcquireContextA type=%u flags=0x%08x success=%u handle=0x%08x error=%u\n",
                    args[3], args[4], success ? 1u : 0u, guest_provider, error);
        return_stdcall(cpu, 20u);
        return 1;
    }

    if (!strcmp(import->name, "CryptGenRandom")) {
        uint32_t args[3];
        if (!guest_memory_try_ptr(cpu->esp + 4u, sizeof(args)))
            stop_guest("CryptGenRandom arguments are unmapped", import->token);
        for (unsigned i = 0u; i < 3u; ++i)
            args[i] = rd32(cpu->esp + 4u + i * 4u);
        uint32_t length = args[1];
        void *buffer = length ? guest_memory_try_ptr(args[2], length) : NULL;
        if (length && !buffer)
            stop_guest("CryptGenRandom output buffer is unmapped", import->token);
        uint32_t error = 0u;
        int success = ac1_crypto_gen_random(args[0], buffer, length, &error);
        cpu->eax = success ? 1u : 0u;
        if (!success) last_error = error;
        if (trace_imports)
            fprintf(stderr,
                    "AC1 CryptoAPI: CryptGenRandom length=%u success=%u error=%u\n",
                    length, success ? 1u : 0u, error);
        return_stdcall(cpu, 12u);
        return 1;
    }

    if (!strcmp(import->name, "CryptReleaseContext")) {
        uint32_t args[2];
        if (!guest_memory_try_ptr(cpu->esp + 4u, sizeof(args)))
            stop_guest("CryptReleaseContext arguments are unmapped", import->token);
        args[0] = rd32(cpu->esp + 4u);
        args[1] = rd32(cpu->esp + 8u);
        uint32_t error = 0u;
        int success = ac1_crypto_release(args[0], args[1], &error);
        cpu->eax = success ? 1u : 0u;
        if (!success) last_error = error;
        if (trace_imports)
            fprintf(stderr,
                    "AC1 CryptoAPI: CryptReleaseContext success=%u error=%u\n",
                    success ? 1u : 0u, error);
        return_stdcall(cpu, 8u);
        return 1;
    }
    return 0;
}

static int handle_audio_import(CPU *cpu, const import_record *import)
{
    if (!import || !import->by_ordinal ||
        !equal_ascii_fold(import->dll, "DSOUND.dll") || import->ordinal != 2u)
        return 0;

#ifndef _WIN32
    (void)cpu;
    return 0;
#else
    const uint32_t *guest_arguments = (const uint32_t *)guest_memory_try_ptr(
        cpu->esp + 4u, 8u);
    if (!guest_arguments)
        stop_guest("DirectSoundEnumerateA arguments are unmapped", import->token);
    uint32_t callback = guest_arguments[0];
    uint32_t guest_context = guest_arguments[1];
    if (!callback)
        stop_guest("DirectSoundEnumerateA received a null guest callback", import->token);

    ac1_audio_device_list devices;
    int status = ac1_audio_enumerate_a(&devices);
    if (status == AC1_AUDIO_ENUM_UNSUPPORTED)
        stop_guest("DirectSoundEnumerateA has no native provider on this platform",
                   import->token);
    if (status != AC1_AUDIO_ENUM_HANDLED) {
        int32_t native_error = devices.native_hresult;
        ac1_audio_device_list_destroy(&devices);
        char message[128];
        snprintf(message, sizeof(message),
                 "DirectSoundEnumerateA host setup failed (HRESULT=0x%08x)",
                 (uint32_t)native_error);
        stop_guest(message, import->token);
    }
    if (devices.overflow_flags) {
        uint32_t overflow = devices.overflow_flags;
        ac1_audio_device_list_destroy(&devices);
        char message[128];
        snprintf(message, sizeof(message),
                 "DirectSoundEnumerateA host record overflow (flags=0x%08x)", overflow);
        stop_guest(message, import->token);
    }

    int32_t native_hresult = devices.native_hresult;
    if (native_hresult < 0) {
        ac1_audio_device_list_destroy(&devices);
        cpu->eax = (uint32_t)native_hresult;
        return_stdcall(cpu, 8u);
        return 1;
    }
    if (!process_heap)
        stop_guest("DirectSoundEnumerateA process heap is unavailable", import->token);

    uint32_t guest_guid[AC1_AUDIO_ENUM_MAX_DEVICES] = {0};
    uint32_t guest_description[AC1_AUDIO_ENUM_MAX_DEVICES] = {0};
    uint32_t guest_module[AC1_AUDIO_ENUM_MAX_DEVICES] = {0};
    size_t allocated = 0u;
    for (size_t i = 0u; i < devices.count; ++i) {
        const ac1_audio_device_record *record = &devices.devices[i];
        if (record->has_guid) {
            guest_guid[i] = pcrecomp_heap_alloc(process_heap, 16u, 0);
            if (!guest_guid[i]) goto audio_guest_allocation_failed;
            void *destination = guest_memory_try_ptr(guest_guid[i], 16u);
            if (!destination) goto audio_guest_allocation_failed;
            memcpy(destination, record->guid, 16u);
        }
        size_t description_length = strlen(record->description) + 1u;
        guest_description[i] = pcrecomp_heap_alloc(process_heap,
                                                     (uint32_t)description_length, 0);
        if (!guest_description[i]) goto audio_guest_allocation_failed;
        void *description_destination = guest_memory_try_ptr(
            guest_description[i], description_length);
        if (!description_destination) goto audio_guest_allocation_failed;
        memcpy(description_destination, record->description, description_length);

        size_t module_length = strlen(record->module) + 1u;
        guest_module[i] = pcrecomp_heap_alloc(process_heap, (uint32_t)module_length, 0);
        if (!guest_module[i]) goto audio_guest_allocation_failed;
        void *module_destination = guest_memory_try_ptr(guest_module[i], module_length);
        if (!module_destination) goto audio_guest_allocation_failed;
        memcpy(module_destination, record->module, module_length);
        allocated = i + 1u;
    }

    ac1_audio_device_list_destroy(&devices);
    for (size_t i = 0u; i < allocated; ++i) {
        uint32_t callback_arguments[4] = {
            guest_guid[i], guest_description[i], guest_module[i], guest_context
        };
        uint32_t callback_result = 0u;
        if (!invoke_guest_stdcall_callback(cpu, callback, callback_arguments, 4u,
                                           &callback_result))
            stop_guest("DirectSoundEnumerateA guest callback frame is unavailable",
                       callback);
        if (!callback_result) break;
    }

    for (size_t i = 0u; i < allocated; ++i) {
        if (guest_guid[i]) pcrecomp_heap_free(process_heap, guest_guid[i]);
        if (guest_description[i]) pcrecomp_heap_free(process_heap, guest_description[i]);
        if (guest_module[i]) pcrecomp_heap_free(process_heap, guest_module[i]);
    }
    cpu->eax = (uint32_t)native_hresult;
    return_stdcall(cpu, 8u);
    return 1;

audio_guest_allocation_failed:
    ac1_audio_device_list_destroy(&devices);
    for (size_t i = 0u; i <= allocated && i < AC1_AUDIO_ENUM_MAX_DEVICES; ++i) {
        if (guest_guid[i]) pcrecomp_heap_free(process_heap, guest_guid[i]);
        if (guest_description[i]) pcrecomp_heap_free(process_heap, guest_description[i]);
        if (guest_module[i]) pcrecomp_heap_free(process_heap, guest_module[i]);
    }
    stop_guest("DirectSoundEnumerateA guest record allocation failed", import->token);
    return 1;
#endif
}

/* The pinned EAX.DLL export table identifies ordinal 6 as
 * EAXDirectSoundCreate8.  The observed caller treats any failing HRESULT as
 * an optional-provider miss and then calls DSOUND!DirectSoundCreate8 with the
 * same arguments.  This runtime has no EAX provider, so report that explicit
 * absence and leave the caller's output pointer null; do not fabricate a
 * DirectSound object. */
static int handle_optional_eax_import(CPU *cpu, const import_record *import)
{
    if (!import || !import->by_ordinal ||
        !equal_ascii_fold(import->dll, "EAX.DLL") || import->ordinal != 6u)
        return 0;

    uint32_t *arguments = (uint32_t *)guest_memory_try_ptr(cpu->esp + 4u, 12u);
    if (!arguments)
        stop_guest("EAXDirectSoundCreate8 arguments are unmapped", import->token);
    uint32_t *output = (uint32_t *)guest_memory_try_ptr(arguments[1], 4u);
    if (!output)
        stop_guest("EAXDirectSoundCreate8 output pointer is unmapped", import->token);

    *output = 0u;
    cpu->eax = 0x80004001u; /* E_NOTIMPL: optional EAX provider is absent. */
    return_stdcall(cpu, 12u);
    return 1;
}

static int handle_bink_import(CPU *cpu, const import_record *import)
{
    uint32_t export_address = 0u;
    char error[192] = {0};
    int found;

    if (!import || !equal_ascii_fold(import->dll, "binkw32.dll")) return 0;
    if (!bink_image_mapped)
        stop_guest("Bink import executed before its module was mapped", import->token);
    if (import->by_ordinal) {
        found = pe32_export_by_ordinal(&bink_image, import->ordinal,
                                      &export_address, error, sizeof(error));
    } else {
        found = pe32_export_by_name(&bink_image, import->name,
                                    &export_address, error, sizeof(error));
    }
    if (!found) {
        char message[320];
        if (import->by_ordinal)
            snprintf(message, sizeof(message),
                     "pinned Bink export #%u is unavailable: %s",
                     (unsigned)import->ordinal, error);
        else
            snprintf(message, sizeof(message),
                     "pinned Bink export %s is unavailable: %s",
                     import->name, error);
        stop_guest(message, import->token);
    }

    uint32_t allocator = 0u, free_callback = 0u;
    int check_set_memory = !import->by_ordinal &&
                           !strcmp(import->name, "_BinkSetMemory@8");
    if (check_set_memory) {
        if (!guest_memory_try_ptr(cpu->esp + 4u, 8u) ||
            !guest_memory_try_ptr(0x18027C8Cu, 8u))
            stop_guest("BinkSetMemory arguments or callback globals are unmapped",
                       import->token);
        allocator = rd32(cpu->esp + 4u);
        free_callback = rd32(cpu->esp + 8u);
    }
    dispatch(cpu, export_address);
    if (check_set_memory) {
        uint32_t stored_allocator = rd32(0x18027C8Cu);
        uint32_t stored_free = rd32(0x18027C90u);
        if (stored_allocator != allocator || stored_free != free_callback)
            stop_guest("BinkSetMemory did not store the supplied guest callbacks",
                       export_address);
        fprintf(stderr,
                "AC1 BinkSetMemory: export=0x%08x allocator=0x%08x free=0x%08x stored=0x%08x,0x%08x\n",
                export_address, allocator, free_callback,
                stored_allocator, stored_free);
    }
    return 1;
}

/* Called by generated code for direct calls, jumps and returns. Imports are
 * assigned unique IAT tokens and stop here until a measured implementation is
 * added; an unknown guest address is equally fatal and includes its caller. */
void dispatch(CPU *cpu, uint32_t target)
{
    if (!cpu) stop_guest("null CPU state", target);
    const uint32_t guest_callsite = cpu->eip;
    ++dispatch_count;
    if (dispatch_limit && dispatch_count > dispatch_limit) {
        char reason[128];
        snprintf(reason, sizeof(reason), "guest dispatch budget exceeded (limit=%llu)",
                 (unsigned long long)dispatch_limit);
        stop_guest(reason, target);
    }
    if (dispatch_depth >= AC1_DISPATCH_DEPTH)
        stop_guest("guest call depth exceeded", target);

    if (trace_object_types && !object_type_registry_traced &&
        target == 0x0085B160u) {
        object_type_registry_traced = 1;
        const uint32_t count_address = 0x01928098u;
        const uint32_t array_address = 0x01A19EE0u;
        if (!guest_memory_try_ptr(count_address, 1u)) {
            fprintf(stderr, "AC1 ObjectTypeInfo registry: count byte is unmapped\n");
        } else {
            uint32_t count = rd8(count_address);
            uint64_t array_bytes = (uint64_t)count * 4u;
            fprintf(stderr,
                    "AC1 ObjectTypeInfo registry: caller=0x%08x array=0x%08x count=%u stride=4\n",
                    cpu->eip, array_address, count);
            if (count && !guest_memory_try_ptr(array_address, (size_t)array_bytes)) {
                fprintf(stderr,
                        "AC1 ObjectTypeInfo registry: populated array range is unmapped (bytes=%llu)\n",
                        (unsigned long long)array_bytes);
            } else {
                for (uint32_t i = 0u; i < count; ++i) {
                    uint32_t object = rd32(array_address + i * 4u);
                    uint32_t vtable = 0u, col = 0u, type_descriptor = 0u;
                    uint32_t methods[3] = {0u, 0u, 0u};
                    char name[192];
                    name[0] = '\0';
                    int object_ok = object && guest_memory_try_ptr(object, 4u);
                    if (object_ok) vtable = rd32(object);
                    int vtable_ok = vtable >= 4u &&
                        guest_memory_try_ptr(vtable - 4u, 20u);
                    if (vtable_ok) {
                        col = rd32(vtable - 4u);
                        for (unsigned slot = 0u; slot < 3u; ++slot)
                            methods[slot] = rd32(vtable + slot * 4u);
                    }
                    int col_ok = col && guest_memory_try_ptr(col, 20u);
                    int type_ok = 0;
                    int object_type_name = 0;
                    if (col_ok && rd32(col) == 0u) {
                        type_descriptor = rd32(col + 12u);
                        type_ok = type_descriptor &&
                            guest_memory_try_ptr(type_descriptor, 12u);
                    }
                    if (type_ok && type_descriptor <= UINT32_MAX - 8u) {
                        uint32_t name_address = type_descriptor + 8u;
                        size_t n = 0u;
                        int name_terminated = 0;
                        while (n + 1u < sizeof(name) &&
                               name_address <= UINT32_MAX - (uint32_t)n &&
                               guest_memory_try_ptr(name_address + (uint32_t)n, 1u)) {
                            name[n] = (char)rd8(name_address + (uint32_t)n);
                            if (!name[n]) {
                                name_terminated = 1;
                                break;
                            }
                            ++n;
                        }
                        name[n] = '\0';
                        object_type_name = name_terminated &&
                            strstr(name, "ObjectTypeInfo@") != NULL;
                    }
                    int method_in_image[3];
                    for (unsigned slot = 0u; slot < 3u; ++slot)
                        method_in_image[slot] = methods[slot] >= image_base &&
                            (uint64_t)methods[slot] <
                                (uint64_t)image_base + image_size_loaded;
                    fprintf(stderr,
                            "AC1 ObjectTypeInfo[%u]: object=0x%08x vtable=0x%08x COL=0x%08x TD=0x%08x slots=0x%08x,0x%08x,0x%08x checks=obj:%d,rtti:%d,name:%d,method-in-image:%d,%d,%d name=\"%s\"\n",
                            i, object, vtable, col, type_descriptor,
                            methods[0], methods[1], methods[2],
                            object_ok, vtable_ok && col_ok && type_ok,
                            object_type_name, method_in_image[0],
                            method_in_image[1], method_in_image[2], name);
                }
            }
        }
    }

    if (trace_imports && cpu->eip == 0x00F9FA67u)
        fprintf(stderr, "AC1 CRT callback: target=0x%08x index=%u table=0x%08x stride=%u context=0x%08x\n",
                target, rd32(cpu->ebp - 0x1cu), rd32(cpu->ebp + 8u),
                rd32(cpu->ebp + 12u), rd32(cpu->ebp + 20u));

    if (trace_imports && target == 0x00933370u && cpu->eip == 0x00937546u &&
        (dispatch_count <= 8u || dispatch_count % 100000u == 0u)) {
        uint32_t stack[4] = {0};
        uint32_t node = 0u, node_plus4 = 0u, node_plus7 = 0u, edi_value = 0u;
        uint32_t header[4] = {0};
        for (unsigned i = 0; i < 4u; ++i) {
            uint32_t address = cpu->esp + 4u + i * 4u;
            if (guest_memory_try_ptr(address, 4u)) stack[i] = rd32(address);
        }
        if (guest_memory_try_ptr(cpu->esi, 4u)) node = rd32(cpu->esi);
        if (guest_memory_try_ptr(cpu->esi + 4u, 4u)) node_plus4 = rd32(cpu->esi + 4u);
        if (guest_memory_try_ptr(cpu->esi + 7u, 1u)) node_plus7 = rd8(cpu->esi + 7u);
        if (guest_memory_try_ptr(cpu->edi, 4u)) edi_value = rd32(cpu->edi);
        uint32_t header_address = node & ~7u;
        if (guest_memory_try_ptr(header_address, sizeof(header)))
            for (unsigned i = 0; i < 4u; ++i) header[i] = rd32(header_address + i * 4u);
        fprintf(stderr, "AC1 parser sample: dispatch=%llu esp=0x%08x eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x esi=0x%08x edi=0x%08x args=%08x,%08x,%08x,%08x node=%08x,%08x,%08x edi0=%08x header=%08x,%08x,%08x,%08x\n",
                (unsigned long long)dispatch_count, cpu->esp, cpu->eax,
                cpu->ebx, cpu->ecx, cpu->edx, cpu->esi, cpu->edi,
                stack[0], stack[1], stack[2], stack[3], node,
                node_plus4, node_plus7, edi_value,
                header[0], header[1], header[2], header[3]);
    }

    if (trace_imports && cpu->eip == 0x00FD2180u && trace_callback_count < 32u) {
        uint32_t fields[5] = {0};
        for (unsigned i = 0; i < 5u; ++i) {
            uint32_t address = cpu->esi + i * 4u;
            if (guest_memory_try_ptr(address, 4u)) fields[i] = rd32(address);
        }
        fprintf(stderr, "AC1 callback-table sample[%u]: target=0x%08x esi=0x%08x ecx=0x%08x eax=0x%08x edi=0x%08x edx=0x%08x record=%08x,%08x,%08x,%08x,%08x\n",
                trace_callback_count++, target, cpu->esi, cpu->ecx, cpu->eax,
                cpu->edi, cpu->edx, fields[0], fields[1], fields[2],
                fields[3], fields[4]);
    }

    const import_record *import = find_import(target);
    if (import) {
        if (trace_imports && trace_import_count < 128u) {
            if (import->by_ordinal)
                fprintf(stderr, "AC1 import[%u] eip=0x%08x target=0x%08x %s!#%u\n",
                        trace_import_count, cpu->eip, target, import->dll, import->ordinal);
            else
                fprintf(stderr, "AC1 import[%u] eip=0x%08x target=0x%08x %s!%s\n",
                        trace_import_count, cpu->eip, target, import->dll, import->name);
            ++trace_import_count;
        }
        int handled = handle_network_import(cpu, import) ||
                      handle_crypto_import(cpu, import) ||
                      handle_audio_import(cpu, import) ||
                      handle_optional_eax_import(cpu, import) ||
                      handle_bink_import(cpu, import) ||
#ifdef _WIN32
                      handle_dinput8_import(cpu, import) ||
                      handle_ds8_import(cpu, import) ||
#endif
#if defined(AC1_HOST_D3D9)
                      handle_d3d9_import(cpu, import) ||
#endif
                      handle_d3dx_texture_requirements_import(cpu, import) ||
                      handle_d3dx_shader_constant_table_import(cpu, import) ||
                      handle_module_import(cpu, import) ||
                      handle_profile_import(cpu, import) ||
                      handle_heap_import(cpu, import) ||
                      handle_critical_section_import(cpu, import) ||
                      handle_interlocked_import(cpu, import) ||
                      handle_stdio_import(cpu, import) ||
                      handle_thread_import(cpu, import) ||
                      handle_file_import(cpu, import) ||
                      handle_sync_import(cpu, import) ||
                      handle_shell_import(cpu, import) ||
                      handle_ole_import(cpu, import) ||
                      handle_registry_import(cpu, import) ||
                      handle_window_import(cpu, import) ||
                      handle_clock_import(cpu, import) ||
                      handle_version_import(cpu, import) ||
                      handle_import(cpu, import);
        if (handled) {
            scheduler_checkpoint();
            return;
        }
        char message[320];
        if (import->by_ordinal && equal_ascii_fold(import->dll, "WS2_32.dll") &&
            import->ordinal == 115u && guest_memory_try_ptr(cpu->esp + 4u, 8u))
            snprintf(message, sizeof(message),
                     "unimplemented import WS2_32.dll!#115 (WSAStartup) version=0x%04x wsadata=0x%08x from 0x%08x",
                     rd32(cpu->esp + 4u) & 0xffffu, rd32(cpu->esp + 8u), last_guest_target);
        else if (!import->by_ordinal && !strcmp(import->name, "CreateFileA")) {
            uint32_t args[7] = {0};
            int args_mapped = guest_memory_try_ptr(cpu->esp + 4u, sizeof(args)) != NULL;
            if (args_mapped)
                for (unsigned i = 0; i < 7u; ++i)
                    args[i] = rd32(cpu->esp + 4u + i * 4u);
            char path[192] = "<unmapped>";
            if (args_mapped && !copy_guest_string(args[0], path, sizeof(path)))
                snprintf(path, sizeof(path), "<invalid-or-too-long:0x%08x>", args[0]);
            snprintf(message, sizeof(message),
                     "unimplemented import %s!CreateFileA path=\"%s\" access=0x%08x share=0x%08x disposition=%u flags=0x%08x from 0x%08x",
                     import->dll, path, args_mapped ? args[1] : 0u,
                     args_mapped ? args[2] : 0u,
                     args_mapped ? args[4] : 0u,
                     args_mapped ? args[5] : 0u, last_guest_target);
        }
        else if (!import->by_ordinal &&
                 equal_ascii_fold(import->dll, "DINPUT8.dll") &&
                 !strcmp(import->name, "DirectInput8Create")) {
            uint32_t args[5] = {0};
            int args_mapped = guest_memory_try_ptr(cpu->esp + 4u, sizeof(args)) != NULL;
            uint32_t return_address = guest_memory_try_ptr(cpu->esp, 4u)
                ? rd32(cpu->esp) : 0u;
            if (args_mapped)
                for (unsigned i = 0; i < 5u; ++i)
                    args[i] = rd32(cpu->esp + 4u + i * 4u);
            snprintf(message, sizeof(message),
                     "unimplemented import DINPUT8.dll!DirectInput8Create return=0x%08x hinst=0x%08x version=0x%08x riid=0x%08x out=0x%08x outer=0x%08x from 0x%08x",
                     return_address,
                     args_mapped ? args[0] : 0u, args_mapped ? args[1] : 0u,
                     args_mapped ? args[2] : 0u, args_mapped ? args[3] : 0u,
                     args_mapped ? args[4] : 0u, last_guest_target);
        }
        else if (!import->by_ordinal && !strcmp(import->name, "SetCurrentDirectoryA")) {
            uint32_t path_address = guest_memory_try_ptr(cpu->esp + 4u, 4u)
                ? rd32(cpu->esp + 4u) : 0u;
            char path[192];
            size_t i = 0u;
            for (; i + 1u < sizeof(path); ++i) {
                const char *byte = path_address
                    ? (const char *)guest_memory_try_ptr(path_address + (uint32_t)i, 1u)
                    : NULL;
                if (!byte) break;
                path[i] = *byte;
                if (!path[i]) break;
            }
            path[i < sizeof(path) ? i : sizeof(path) - 1u] = '\0';
            snprintf(message, sizeof(message),
                     "unimplemented import KERNEL32.dll!SetCurrentDirectoryA path=\"%s\" from 0x%08x",
                     path, last_guest_target);
        }
        else if (import->by_ordinal)
            snprintf(message, sizeof(message), "unimplemented import %s!#%u from 0x%08x",
                     import->dll, import->ordinal, last_guest_target);
        else
            snprintf(message, sizeof(message), "unimplemented import %s!%s from 0x%08x",
                     import->dll, import->name, last_guest_target);
        stop_guest(message, target);
    }

#if defined(AC1_HOST_D3D9)
    if (dispatch_d3d9_method(cpu, target)) return;
#endif
#ifdef _WIN32
    if (dispatch_dinput8_method(cpu, target)) return;
    if (dispatch_ds8_method(cpu, target)) return;
#endif

    ac1_lifted_fn function = ac1_lifted_lookup(target);
    if (!function && bink_image_mapped && target >= bink_image.load_base &&
        (uint64_t)target < (uint64_t)bink_image.load_base + bink_image.image_size) {
        bink_lifted_fn bink_function = bink_lifted_lookup(target);
        if (!bink_function)
            stop_guest("no generated Bink function for guest target", target);
        function = bink_function;
    }
    if (!function)
        stop_guest("no generated function for guest target", target);

    if (target == 0x00401375u && !crt_handoff_logged) {
        crt_handoff_logged = 1;
        fprintf(stderr,
                "AC1 CRT handoff: WinMain thunk=0x%08x return=0x%08x hInstance=0x%08x hPrev=0x%08x cmdline=0x%08x show=0x%08x\n",
                target, rd32(cpu->esp), rd32(cpu->esp + 4u), rd32(cpu->esp + 8u),
                rd32(cpu->esp + 12u), rd32(cpu->esp + 16u));
    }

    uint32_t previous_target = last_guest_target;
    last_guest_target = target;
    ++dispatch_depth;
    function(cpu);
    --dispatch_depth;
    if (trace_object_types && guest_callsite == 0x0085B197u) {
        uint32_t descriptor = cpu->eax;
        unsigned descriptor_index = object_type_descriptor_count++;
        if (!descriptor || !guest_memory_try_ptr(descriptor, 0x11u)) {
            fprintf(stderr,
                    "AC1 ObjectTypeInfo descriptor[%u]: returned=0x%08x fields=unmapped\n",
                    descriptor_index, descriptor);
        } else {
            uint32_t parent_array = rd32(descriptor + 4u);
            uint32_t parent_count = rd8(descriptor + 0x10u);
            fprintf(stderr,
                    "AC1 ObjectTypeInfo descriptor[%u]: returned=0x%08x parent_array=0x%08x count=%u\n",
                    descriptor_index, descriptor, parent_array, parent_count);
            if (parent_count &&
                !guest_memory_try_ptr(parent_array, (size_t)parent_count * 4u)) {
                fprintf(stderr,
                        "AC1 ObjectTypeInfo descriptor[%u]: parent array range is unmapped\n",
                        descriptor_index);
            } else {
                for (uint32_t i = 0u; i < parent_count; ++i) {
                    if (object_type_parent_records >= 4096u) {
                        fprintf(stderr,
                                "AC1 ObjectTypeInfo parent callback collection stopped at 4096 entries\n");
                        break;
                    }
                    uint32_t callback = rd32(parent_array + i * 4u);
                    int method_in_image = callback >= image_base &&
                        (uint64_t)callback <
                            (uint64_t)image_base + image_size_loaded;
                    fprintf(stderr,
                            "AC1 ObjectTypeInfo descriptor[%u] parent[%u]: target=0x%08x method-in-image=%d\n",
                            descriptor_index, i, callback, method_in_image);
                    ++object_type_parent_records;
                }
            }
        }
    }
    last_guest_target = previous_target;
    scheduler_return_checkpoint();
}

void dispatch_jmp(CPU *cpu, uint32_t target)
{
    dispatch(cpu, target);
}

static int read_file(const char *path, uint8_t **bytes, size_t *size,
                     char *error, size_t error_capacity)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(error, error_capacity, "cannot open executable: %s", path);
        return 0;
    }
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        snprintf(error, error_capacity, "cannot size executable: %s", path);
        fclose(file);
        return 0;
    }
    long length = ftell(file);
    if (length <= 0 || (uint64_t)length > SIZE_MAX || fseek(file, 0, SEEK_SET)) {
        snprintf(error, error_capacity, "invalid executable size: %s", path);
        fclose(file);
        return 0;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (!data) {
        snprintf(error, error_capacity, "out of memory reading executable");
        fclose(file);
        return 0;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        snprintf(error, error_capacity, "short read from executable: %s", path);
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *bytes = data;
    *size = (size_t)length;
    return 1;
}

#ifndef __ANDROID__
static void derive_guest_current_directory(const char *path)
{
    size_t length = strlen(path);
    size_t separator = length;
    while (separator && path[separator - 1u] != '/' && path[separator - 1u] != '\\')
        --separator;
    if (!separator) {
        memcpy(guest_current_directory, ".", 2u);
        return;
    }
    size_t directory_length = separator - 1u;
    if (directory_length == 0u ||
        (directory_length == 2u && path[1] == ':'))
        directory_length = separator;
    if (directory_length >= sizeof(guest_current_directory))
        directory_length = sizeof(guest_current_directory) - 1u;
    memcpy(guest_current_directory, path, directory_length);
    guest_current_directory[directory_length] = '\0';
#ifdef _WIN32
    for (size_t i = 0; i < directory_length; ++i)
        if (guest_current_directory[i] == '/') guest_current_directory[i] = '\\';
#endif
}
#endif

#ifdef __ANDROID__
static int derive_host_game_directory(const char *path, char *directory,
                                      size_t capacity)
{
    const char *separator;
    size_t length;
    if (!path || !*path || !directory || !capacity) return 0;
    separator = strrchr(path, '/');
    if (!separator) separator = strrchr(path, '\\');
    if (!separator) {
        if (capacity < 2u) return 0;
        memcpy(directory, ".", 2u);
        return 1;
    }
    length = (size_t)(separator - path);
    if (length == 0u) length = 1u; /* POSIX root */
    if (length >= capacity) return 0;
    memcpy(directory, path, length);
    directory[length] = '\0';
    return 1;
}

static int initialize_android_guest_paths(const char *host_exe_path)
{
    const char *basename = strrchr(host_exe_path, '/');
    if (!basename) basename = strrchr(host_exe_path, '\\');
    basename = basename ? basename + 1u : host_exe_path;
    if (!*basename ||
        !derive_host_game_directory(host_exe_path, host_game_directory,
                                    sizeof(host_game_directory)))
        return 0;
    int written = snprintf(exe_path_copy, sizeof(exe_path_copy),
                          "C:\\AC1\\%s", basename);
    if (written < 0 || (size_t)written >= sizeof(exe_path_copy)) return 0;
    memcpy(guest_current_directory, "C:\\AC1", sizeof("C:\\AC1"));
    return 1;
}
#endif

static int load_pinned_bink_module(char *error, size_t error_capacity)
{
    uint8_t *file_bytes = NULL;
    size_t file_size = 0u;
    uint8_t digest[32];
    char hash[65];
    int path_length;

#ifdef __ANDROID__
    path_length = snprintf(bink_module_path, sizeof(bink_module_path),
                           "%s\\%s", guest_current_directory, "binkw32.dll");
    if (path_length < 0 || (size_t)path_length >= sizeof(bink_module_path)) {
        snprintf(error, error_capacity, "pinned Bink DLL path is too long");
        return 0;
    }
    int host_path_length = snprintf(bink_host_path, sizeof(bink_host_path),
                                    "%s/%s", host_game_directory,
                                    "binkw32.dll");
    if (host_path_length < 0 ||
        (size_t)host_path_length >= sizeof(bink_host_path)) {
        snprintf(error, error_capacity, "pinned Bink host path is too long");
        return 0;
    }
    if (!read_file(bink_host_path, &file_bytes, &file_size,
                   error, error_capacity))
        return 0;
#else
    path_length = snprintf(bink_module_path, sizeof(bink_module_path),
                           "%s/%s", guest_current_directory, "binkw32.dll");
    if (path_length < 0 || (size_t)path_length >= sizeof(bink_module_path)) {
        snprintf(error, error_capacity, "pinned Bink DLL path is too long");
        return 0;
    }
#ifdef _WIN32
    for (size_t i = 0u; i < (size_t)path_length; ++i)
        if (bink_module_path[i] == '/') bink_module_path[i] = '\\';
#endif
    if (!read_file(bink_module_path, &file_bytes, &file_size,
                   error, error_capacity))
        return 0;
#endif
    sha256(file_bytes, file_size, digest);
    digest_hex(digest, hash);
    if (strcmp(hash, BINK_MODULE_SHA256)) {
        snprintf(error, error_capacity,
                 "Bink DLL SHA-256 mismatch: expected %s, found %s",
                 BINK_MODULE_SHA256, hash);
        free(file_bytes);
        return 0;
    }
    if (!pe32_load(file_bytes, file_size, BINK_MODULE_PREFERRED_BASE,
                   &bink_image, error, error_capacity)) {
        free(file_bytes);
        return 0;
    }
    free(file_bytes);
    if (bink_image.preferred_base != BINK_MODULE_PREFERRED_BASE ||
        bink_image.load_base != BINK_MODULE_PREFERRED_BASE ||
        bink_image.image_size != BINK_MODULE_IMAGE_SIZE ||
        bink_image.entry_rva != BINK_MODULE_ENTRY_RVA) {
        snprintf(error, error_capacity,
                 "pinned Bink PE layout mismatch: preferred=0x%08x load=0x%08x size=0x%08x entry=0x%08x",
                 bink_image.preferred_base, bink_image.load_base,
                 bink_image.image_size, bink_image.entry_rva);
        pe32_unload(&bink_image);
        return 0;
    }
    for (size_t i = 0u; i < BINK_MODULE_EXPORT_COUNT; ++i) {
        uint32_t by_name = 0u, by_ordinal = 0u;
        const bink_export_record *record = &bink_export_records[i];
        if (!pe32_export_by_name(&bink_image, record->name, &by_name,
                                 error, error_capacity) ||
            !pe32_export_by_ordinal(&bink_image, record->ordinal, &by_ordinal,
                                    error, error_capacity) ||
            by_name != record->guest_va || by_ordinal != record->guest_va) {
            if (!error[0])
                snprintf(error, error_capacity,
                         "pinned Bink export map mismatch at %s ordinal %u",
                         record->name, (unsigned)record->ordinal);
            pe32_unload(&bink_image);
            return 0;
        }
    }
    if (!pe32_link_imports(&bink_image, resolve_import, NULL,
                           error, error_capacity)) {
        pe32_unload(&bink_image);
        return 0;
    }
    return 1;
}

static uint64_t read_scheduler_return_slice_ns(void)
{
    const char *setting = getenv("AC1_SCHEDULER_SLICE_US");
    if (!setting || !*setting) return UINT64_C(1000000);
    if (setting[0] == '-') return UINT64_C(1000000);
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(setting, &end, 0);
    if (errno == ERANGE || end == setting || *end != '\0' ||
        value > UINT64_MAX / UINT64_C(1000))
        return UINT64_C(1000000);
    return (uint64_t)value * UINT64_C(1000);
}

static uint64_t read_dispatch_limit(void)
{
    const char *setting = getenv("AC1_DISPATCH_LIMIT");
    if (!setting || !*setting) return AC1_DISPATCH_LIMIT_DEFAULT;
    if (setting[0] == '-') return AC1_DISPATCH_LIMIT_DEFAULT;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(setting, &end, 0);
    if (errno == ERANGE || end == setting || *end != '\0')
        return AC1_DISPATCH_LIMIT_DEFAULT;
    return (uint64_t)value;
}

int ac1_run(const char *exe_path)
{
    char error[512] = {0};
    uint8_t *file_bytes = NULL;
    size_t file_size = 0;
    pe32_module image = {0};
    guest_address_space *space = NULL;
    ac1_scheduler *scheduler = NULL;
    int result = 1;
    last_run_error[0] = '\0';
    if (!exe_path || !*exe_path) {
        snprintf(last_run_error, sizeof(last_run_error), "executable path is empty");
        fprintf(stderr, "AC1 native runtime: executable path is empty\n");
        return 2;
    }
    if (strlen(exe_path) >= sizeof(exe_path_copy)) {
        snprintf(last_run_error, sizeof(last_run_error), "executable path is too long");
        fprintf(stderr, "AC1 native runtime: executable path is too long\n");
        return 2;
    }
    lock_ac1_run();
    dispatch_limit = read_dispatch_limit();
    scheduler_return_slice_ns = read_scheduler_return_slice_ns();
#ifdef _WIN32
    window_bridge = NULL;
    ds8_configured = 0;
    dinput8_configured = 0;
#endif
    import_count = 0;
    memset(imports, 0, sizeof(imports));
    memset(&bink_image, 0, sizeof(bink_image));
    bink_image_mapped = 0;
    bink_module_path[0] = '\0';
#ifdef __ANDROID__
    bink_host_path[0] = '\0';
    host_game_directory[0] = '\0';
#endif
    process_heap = NULL;
    guest_files = NULL;
    exe_path_copy[0] = '\0';
    command_line_guest_address = 0;
    image_base = 0;
    image_size_loaded = 0u;
    static_tls_data_base = 0;
    static_tls_vector_base = 0;
    static_tls_template = NULL;
    static_tls_template_size = 0u;
    static_tls_zero_fill = 0u;
    static_tls_index_address = 0u;
    last_error = 0;
    max_guest_handles = 16u;
    unhandled_exception_filter = 0u;
    com_init_count = 0u;
    com_apartment_model = UINT32_MAX;
    guest_affinity_mask = 0u;
    guest_thread_affinity_mask = 0u;
    memset(tls_used, 0, sizeof(tls_used));
    memset(tls_values, 0, sizeof(tls_values));
    memset(private_heaps, 0, sizeof(private_heaps));
    memset(sync_objects, 0, sizeof(sync_objects));
    memset(virtual_alloc_bases, 0, sizeof(virtual_alloc_bases));
    const char *trace_setting = getenv("AC1_TRACE_IMPORTS");
    trace_imports = trace_setting && *trace_setting && strcmp(trace_setting, "0");
    const char *type_trace_setting = getenv("AC1_TRACE_OBJECT_TYPES");
    trace_object_types = type_trace_setting && *type_trace_setting &&
                         strcmp(type_trace_setting, "0");
    object_type_registry_traced = 0;
    object_type_parent_records = 0u;
    object_type_descriptor_count = 0u;
    if (trace_imports)
        fprintf(stderr, "AC1 dispatch guard: limit=%llu (0 disables)\n",
                (unsigned long long)dispatch_limit);
    trace_import_count = 0;
    trace_callback_count = 0;
    trace_file_read_count = 0;
    crt_handoff_logged = 0;
#ifdef __ANDROID__
    if (!initialize_android_guest_paths(exe_path)) {
        snprintf(last_run_error, sizeof(last_run_error),
                 "cannot derive Android host/guest executable paths");
        fprintf(stderr, "AC1 native runtime: %s\n", last_run_error);
        unlock_ac1_run();
        return 2;
    }
#else
    memcpy(exe_path_copy, exe_path, strlen(exe_path) + 1u);
#ifdef _WIN32
    for (size_t i = 0; exe_path_copy[i]; ++i)
        if (exe_path_copy[i] == '/') exe_path_copy[i] = '\\';
#endif
    derive_guest_current_directory(exe_path_copy);
#endif
    if (!read_file(exe_path, &file_bytes, &file_size, error, sizeof(error))) {
        snprintf(last_run_error, sizeof(last_run_error), "%s", error);
        fprintf(stderr, "AC1 native runtime: %s\n", error);
        unlock_ac1_run();
        return 2;
    }
    uint8_t binary_digest[32];
    char binary_hash[65];
    sha256(file_bytes, file_size, binary_digest);
    digest_hex(binary_digest, binary_hash);
    if (strcmp(binary_hash, AC1_EXPECTED_EXE_SHA256)) {
        snprintf(last_run_error, sizeof(last_run_error),
                 "executable SHA-256 mismatch: expected %s, found %s",
                 AC1_EXPECTED_EXE_SHA256, binary_hash);
        fprintf(stderr, "AC1 native runtime: %s\n", last_run_error);
        free(file_bytes);
        unlock_ac1_run();
        return 9;
    }
    if (!pe32_load(file_bytes, file_size, 0x00400000u, &image,
                   error, sizeof(error))) {
        snprintf(last_run_error, sizeof(last_run_error), "PE load failed: %s", error);
        fprintf(stderr, "AC1 native runtime: PE load failed: %s\n", error);
        free(file_bytes);
        unlock_ac1_run();
        return 3;
    }
    free(file_bytes);
    if (!pe32_link_imports(&image, resolve_import, NULL, error, sizeof(error))) {
        snprintf(last_run_error, sizeof(last_run_error), "import linking failed: %s", error);
        fprintf(stderr, "AC1 native runtime: import linking failed: %s\n", error);
        pe32_unload(&image);
        import_count = 0;
        unlock_ac1_run();
        return 4;
    }
    if (!load_pinned_bink_module(error, sizeof(error))) {
        snprintf(last_run_error, sizeof(last_run_error),
                 "pinned Bink module load failed: %s", error);
        fprintf(stderr, "AC1 native runtime: %s\n", last_run_error);
        pe32_unload(&image);
        memset(imports, 0, sizeof(imports));
        import_count = 0;
        bink_module_path[0] = '\0';
        unlock_ac1_run();
        return 4;
    }
    space = guest_address_space_create();
    if (!space) {
        snprintf(last_run_error, sizeof(last_run_error), "guest address-space allocation failed");
        fprintf(stderr, "AC1 native runtime: guest address-space allocation failed\n");
        pe32_unload(&bink_image);
        bink_module_path[0] = '\0';
        pe32_unload(&image);
        unlock_ac1_run();
        return 5;
    }
    guest_address_space_select(space);
    guest_memory_add(image.image, image.load_base, image.image_size);
    guest_memory_add(bink_image.image, bink_image.load_base, bink_image.image_size);
    bink_image_mapped = 1;
#ifdef __ANDROID__
    guest_files = ac1_files_create_with_guest_root(host_game_directory,
                                                   guest_current_directory);
#else
    guest_files = ac1_files_create(guest_current_directory);
#endif
    if (!guest_files) {
        snprintf(last_run_error, sizeof(last_run_error),
                 "read-only file service initialization failed for game directory");
        fprintf(stderr, "AC1 native runtime: %s\n", last_run_error);
        result = 6;
        goto cleanup;
    }
    if (!guest_memory_allocate(AC1_STACK_BASE, AC1_STACK_SIZE) ||
        !guest_memory_allocate(AC1_TIB_BASE, AC1_TIB_SIZE)) {
        snprintf(last_run_error, sizeof(last_run_error), "guest stack/TIB allocation failed");
        fprintf(stderr, "AC1 native runtime: guest stack/TIB allocation failed\n");
        result = 6;
        goto cleanup;
    }
    process_heap = pcrecomp_heap_create(0x100000u);
    if (!process_heap) {
        snprintf(last_run_error, sizeof(last_run_error), "process heap initialization failed");
        fprintf(stderr, "AC1 native runtime: process heap initialization failed\n");
        result = 6;
        goto cleanup;
    }

    /* Minimal x86 process state: stack and TIB are mapped at stable guest VAs;
     * API services remain strict traps until their real semantics are added. */
    uint32_t stack_top = AC1_STACK_BASE + AC1_STACK_SIZE - 0x100u;
    uint32_t tib[7] = {0};
    tib[0] = UINT32_MAX;                    /* ExceptionList: empty SEH chain. */
    tib[1] = stack_top;                     /* StackBase. */
    tib[2] = AC1_STACK_BASE;                /* StackLimit. */
    tib[6] = AC1_TIB_BASE;                  /* Self pointer at NT_TIB+0x18. */
    memcpy(guest_ptr(AC1_TIB_BASE, sizeof(tib)), tib, sizeof(tib));
    if (!initialize_static_tls(&image, error, sizeof(error))) {
        snprintf(last_run_error, sizeof(last_run_error), "static TLS initialization failed: %s", error);
        fprintf(stderr, "AC1 native runtime: %s\n", last_run_error);
        result = 6;
        goto cleanup;
    }
    memset(tls_used, 0, sizeof(tls_used));
    memset(tls_values, 0, sizeof(tls_values));
    /* The executable is the first and only loaded module in this profile. */
    tls_used[0] = 1u;
    tls_values[0] = static_tls_data_base;

    scheduler = (ac1_scheduler *)calloc(1, sizeof(*scheduler));
    if (!scheduler) {
        snprintf(last_run_error, sizeof(last_run_error), "guest scheduler allocation failed");
        result = 6;
        goto cleanup;
    }
    scheduler_initialize(scheduler, space);
    active_scheduler = scheduler;
    ac1_guest_thread *primary = &scheduler->threads[0];
    primary->scheduler = scheduler;
    primary->slot = 0u;
    primary->id = current_host_thread_id();
    primary->stack_base = AC1_STACK_BASE;
    primary->stack_size = AC1_STACK_SIZE;
    primary->tib_base = AC1_TIB_BASE;
    primary->static_tls_data = static_tls_data_base;
    primary->static_tls_vector = static_tls_vector_base;
    primary->image_delta = image.load_base - image.preferred_base;
    active_guest_thread = primary;
    primary->return_slice_deadline = scheduler_return_slice_ns
        ? guest_monotonic_ns() + scheduler_return_slice_ns : 0u;
    primary->return_slice_checks = 0u;
    wr32(AC1_TIB_BASE + 0x20u, 1u);
    wr32(AC1_TIB_BASE + 0x24u, primary->id);
    CPU *cpu = &primary->cpu;
    memset(cpu, 0, sizeof(*cpu));
    cpu->eip = image.load_base + image.entry_rva;
    cpu->esp = stack_top - 4u;
    cpu->eflags_misc = 2u;
    cpu->fs_base = AC1_TIB_BASE;
    image_base = image.load_base;
    image_size_loaded = image.image_size;
    wr32(cpu->esp, AC1_RETURN_SENTINEL);
    dispatch_count = 0;
    dispatch_depth = 0;
    last_guest_target = cpu->eip;
    g_image_delta = image.load_base - image.preferred_base;

    printf("AC1 native entry: image=0x%08x size=0x%08x entry=0x%08x imports=%zu\n",
           image.load_base, image.image_size, cpu->eip, import_count);
    fflush(stdout);

    if (setjmp(stop_runtime) == 0) {
        runtime_stop_ready = 1;
        const uint32_t bink_attach_arguments[3] = {
            bink_image.load_base, 1u, 0u /* DLL_PROCESS_ATTACH, reserved */
        };
        uint32_t bink_attach_result = 0u;
        uint32_t bink_entry = bink_image.load_base + bink_image.entry_rva;
        if (!invoke_guest_stdcall_callback(cpu, bink_entry,
                                           bink_attach_arguments, 3u,
                                           &bink_attach_result))
            stop_guest("Bink DllMain PROCESS_ATTACH frame setup failed", bink_entry);
        if (!bink_attach_result)
            stop_guest("Bink DllMain rejected DLL_PROCESS_ATTACH", bink_entry);
        fprintf(stderr,
                "AC1 Bink attached: base=0x%08x entry=0x%08x result=0x%08x\n",
                bink_image.load_base, bink_entry, bink_attach_result);
        dispatch(cpu, cpu->eip);
        runtime_stop_ready = 0;
        if (cpu->esp == stack_top && rd32(stack_top - 4u) == AC1_RETURN_SENTINEL) {
            printf("AC1 entry returned naturally after %llu guest dispatches\n",
                   (unsigned long long)dispatch_count);
            result = 0;
        } else {
            snprintf(last_run_error, sizeof(last_run_error),
                     "entry returned at guest EIP=0x%08x ESP=0x%08x", cpu->eip, cpu->esp);
            fprintf(stderr, "AC1 entry returned at guest EIP=0x%08x ESP=0x%08x\n",
                    cpu->eip, cpu->esp);
            result = 7;
        }
    } else {
        runtime_stop_ready = 0;
        fprintf(stderr, "AC1 guest stop: %s; main-caller=0x%08x main-EIP=0x%08x main-ESP=0x%08x main-EAX=0x%08x main-dispatches=%llu\n",
                stop_reason, last_guest_target, cpu->eip, cpu->esp, cpu->eax,
                (unsigned long long)dispatch_count);
        if (!(scheduler && scheduler->failed))
            trace_receiver_snapshot(cpu);
        if (scheduler && scheduler->failed) {
            const CPU *failed = &scheduler->failure_cpu;
            fprintf(stderr,
                    "AC1 worker stop: thread=%u reason=%s caller=0x%08x target=0x%08x EIP=0x%08x ESP=0x%08x EAX=0x%08x ECX=0x%08x EDX=0x%08x EBX=0x%08x EBP=0x%08x ESI=0x%08x EDI=0x%08x stack=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
                    scheduler->failure_thread_id, scheduler->failure_reason,
                    scheduler->failure_caller, scheduler->failure_target,
                    failed->eip, failed->esp, failed->eax, failed->ecx,
                    failed->edx, failed->ebx, failed->ebp, failed->esi,
                    failed->edi,
                    scheduler->failure_stack[0], scheduler->failure_stack[1],
                    scheduler->failure_stack[2], scheduler->failure_stack[3],
                    scheduler->failure_stack[4], scheduler->failure_stack[5],
                    scheduler->failure_stack[6], scheduler->failure_stack[7]);
            if (trace_imports && scheduler->failure_object_readable) {
                fprintf(stderr,
                        "AC1 worker receiver: ecx=0x%08x words=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
                        failed->ecx, scheduler->failure_object_words[0],
                        scheduler->failure_object_words[1],
                        scheduler->failure_object_words[2],
                        scheduler->failure_object_words[3],
                        scheduler->failure_object_words[4],
                        scheduler->failure_object_words[5],
                        scheduler->failure_object_words[6],
                        scheduler->failure_object_words[7]);
                if (scheduler->failure_vtable_readable)
                    fprintf(stderr,
                            "AC1 worker candidate vtable: address=0x%08x slots=[%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x]\n",
                            scheduler->failure_object_words[0],
                            scheduler->failure_vtable_words[0],
                            scheduler->failure_vtable_words[1],
                            scheduler->failure_vtable_words[2],
                            scheduler->failure_vtable_words[3],
                            scheduler->failure_vtable_words[4],
                            scheduler->failure_vtable_words[5],
                            scheduler->failure_vtable_words[6],
                            scheduler->failure_vtable_words[7]);
            }
        }
        result = 8;
    }

cleanup:
    runtime_stop_ready = 0;
    if (scheduler) {
        scheduler_stop_and_join(scheduler);
        free(scheduler);
        scheduler = NULL;
    }
#ifdef _WIN32
    if (dinput8_configured) {
        ac1_dinput8_shutdown();
        dinput8_configured = 0;
    }
#endif
    ac1_network_reset();
    ac1_crypto_reset();
#ifdef _WIN32
    if (ds8_configured) {
        ac1_ds8_shutdown();
        ds8_configured = 0;
    }
#endif
#if defined(AC1_HOST_D3D9)
    if (d3d9_configured) {
        ac1_d3d9_shutdown();
        d3d9_configured = 0;
    }
    free(d3d9_heap_blocks);
    d3d9_heap_blocks = NULL;
    d3d9_heap_block_count = 0u;
    d3d9_heap_block_capacity = 0u;
#endif
#ifdef _WIN32
    free(guest_monitors);
    guest_monitors = NULL;
    guest_monitor_count = 0u;
    guest_monitor_capacity = 0u;
    next_guest_monitor = 0xE6000004u;
    if (window_bridge) {
        ac1_window_win32_destroy(window_bridge);
        window_bridge = NULL;
    }
#endif
    if (guest_files) {
        ac1_files_destroy(guest_files);
        guest_files = NULL;
    }
    sync_objects_destroy();
    active_guest_thread = NULL;
    if (process_heap && space) {
        guest_address_space_select(space);
        pcrecomp_heap_destroy(process_heap);
    }
    for (size_t i = 0; i < AC1_MAX_PRIVATE_HEAPS; ++i) {
        if (private_heaps[i].heap && space) {
            guest_address_space_select(space);
            pcrecomp_heap_destroy(private_heaps[i].heap);
        }
        private_heaps[i].heap = NULL;
        private_heaps[i].handle = 0;
    }
    process_heap = NULL;
    image_base = 0;
    if (space) {
        guest_address_space_select(space);
        if (static_tls_vector_base) guest_memory_release(static_tls_vector_base);
        if (static_tls_data_base) guest_memory_release(static_tls_data_base);
        guest_memory_release(AC1_TIB_BASE);
        guest_memory_release(AC1_STACK_BASE);
        if (bink_image_mapped) (void)guest_memory_remove(bink_image.load_base);
        (void)guest_memory_remove(image.load_base);
    }
    bink_image_mapped = 0;
    bink_module_path[0] = '\0';
    static_tls_data_base = 0;
    static_tls_vector_base = 0;
    static_tls_template = NULL;
    static_tls_template_size = 0u;
    static_tls_zero_fill = 0u;
    static_tls_index_address = 0u;
    guest_address_space_select(NULL);
    guest_address_space_destroy(space);
    pe32_unload(&bink_image);
    pe32_unload(&image);
    memset(imports, 0, sizeof(imports));
    import_count = 0;
    g_image_delta = 0;
    unlock_ac1_run();
    return result;
}
