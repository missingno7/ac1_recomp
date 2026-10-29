/* Exercise the runtime's real private wait/scheduler implementation, without
 * executing translated game code. Build with function sections + --gc-sections.
 * No copies of event acquisition, wait completion or CS semantics live here. */
#include "../src/runtime/ac1_runtime.c"
#include <assert.h>

static ac1_scheduler fixture;
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)

static void reset_fixture(unsigned count)
{
    sync_objects_destroy();
    memset(&fixture, 0, sizeof(fixture));
    fixture.count = count;
    fixture.owner = 0;
    active_scheduler = &fixture;
    for (unsigned i=0; i<count; ++i) {
        ac1_guest_thread *t=&fixture.threads[i];
        t->scheduler=&fixture; t->slot=i; t->id=100+i;
        t->handle=AC1_THREAD_HANDLE_BASE+i*4;
    }
    active_guest_thread=&fixture.threads[0];
}

static sync_object *event(int manual)
{
    sync_object *o=sync_object_create(1);
    o->manual_reset=manual;
    o->host_event=CreateEventW(NULL, manual, FALSE, NULL);
    CHECK(o->host_event != NULL);
    return o;
}

static void wait_on(unsigned slot, sync_object *o)
{
    ac1_guest_thread *t=&fixture.threads[slot];
    t->blocked=1; t->wait_kind=1; t->wait_handle=o->handle;
    t->wait_count=0; t->wait_deadline=0;
}

static void signal_import(const char *name, sync_object *o)
{
    import_record import={0};
    strcpy(import.dll,"KERNEL32.dll"); strcpy(import.name,name);
    CPU *cpu=&fixture.threads[0].cpu;
    cpu->esp=0x10000;
    wr32(cpu->esp,0x1234); wr32(cpu->esp+4,o->handle);
    CHECK(handle_sync_import(cpu,&import));
    CHECK(cpu->eax==1);
}

static ac1_native_reset_event_fn actual_reset;
static LONG NTAPI completion_at_reset(HANDLE event, LONG *previous)
{
    /* Deterministically put completions on both sides of the kernel reset.
     * A separate pre-reset readiness poll cannot see the first completion. */
    CHECK(SetEvent(event));
    LONG status=actual_reset(event,previous);
    CHECK(status==0 && *previous==1);
    CHECK(SetEvent(event));
    return status;
}

static void event_contract(void)
{
    reset_fixture(3);
    InitializeCriticalSection(&fixture.mutex);
    InitializeConditionVariable(&fixture.condition);
    sync_object *a=event(0), *b=event(0);
    wait_on(1, a); wait_on(2, a);
    fixture.threads[1].wait_count=2;
    fixture.threads[1].wait_handles[0]=b->handle;
    fixture.threads[1].wait_handles[1]=a->handle;
    SetEvent(a->host_event);
    scheduler_refresh_waits(&fixture);
    CHECK(!fixture.threads[1].blocked && fixture.threads[1].cpu.eax==1);
    CHECK(fixture.threads[2].blocked);
    scheduler_refresh_waits(&fixture);
    CHECK(fixture.threads[2].blocked); /* signal is consumed exactly once */
    SetEvent(a->host_event); SetEvent(b->host_event);
    wait_on(1,a);
    fixture.threads[1].wait_count=2;
    fixture.threads[1].wait_handles[0]=b->handle;
    fixture.threads[1].wait_handles[1]=a->handle;
    scheduler_refresh_waits(&fixture);
    CHECK(!fixture.threads[1].blocked && fixture.threads[1].cpu.eax==0);
    CHECK(!fixture.threads[2].blocked); /* nonselected event was not consumed */

    sync_object *m=event(1);
    wait_on(1,m); wait_on(2,m);
    SetEvent(m->host_event);
    scheduler_refresh_waits(&fixture);
    ResetEvent(m->host_event);
    CHECK(!fixture.threads[1].blocked && !fixture.threads[2].blocked);
    wait_on(1,m);
    scheduler_refresh_waits(&fixture);
    CHECK(fixture.threads[1].blocked);

    /* Thread completion is persistent, unlike an auto-reset event. */
    fixture.threads[0].finished=1;
    fixture.threads[1].wait_handle=fixture.threads[0].handle;
    fixture.threads[2].blocked=1; fixture.threads[2].wait_kind=1;
    fixture.threads[2].wait_handle=fixture.threads[0].handle;
    scheduler_refresh_waits(&fixture);
    CHECK(!fixture.threads[1].blocked && !fixture.threads[2].blocked);
    DeleteCriticalSection(&fixture.mutex);
}

/* Both signalers retain their quantum. A reset or another SetEvent before
 * the waiter's next guest instruction cannot retract/collapse completed waits. */
static void signal_boundary_contract(void)
{
    reset_fixture(3);
    InitializeCriticalSection(&fixture.mutex);
    InitializeConditionVariable(&fixture.condition);
    sync_object *a=event(0), *m=event(1);
    wait_on(1,a); wait_on(2,a);
    signal_import("SetEvent",a);
    CHECK(!fixture.threads[1].blocked && fixture.threads[2].blocked);
    signal_import("SetEvent",a);
    CHECK(!fixture.threads[1].blocked && !fixture.threads[2].blocked);
    CHECK(!sync_object_try_acquire(a,100));
    wait_on(1,m); wait_on(2,m);
    signal_import("SetEvent",m);
    signal_import("ResetEvent",m);
    CHECK(!fixture.threads[1].blocked && !fixture.threads[2].blocked);
    CHECK(!sync_object_try_acquire(m,100));
    /* Native I/O completion before a reset must grant an existing wait too. */
    wait_on(1,a);
    SetEvent(a->host_event);
    signal_import("ResetEvent",a);
    CHECK(!fixture.threads[1].blocked);
    wait_on(1,a); wait_on(2,a);
    actual_reset=sync_native_reset_event;
    sync_native_reset_event=completion_at_reset;
    signal_import("ResetEvent",a);
    sync_native_reset_event=actual_reset;
    CHECK(!fixture.threads[1].blocked && !fixture.threads[2].blocked);
    CHECK(!sync_object_try_acquire(a,100));
    /* The read-only wait dump must not consume the remaining native signal. */
    SetEvent(a->host_event);
    trace_wait_interval_ns=1;
    scheduler_trace_waits(&fixture,guest_monotonic_ns());
    trace_wait_interval_ns=0;
    CHECK(sync_object_try_acquire(a,100));
    /* NT failure is translated to the guest error; it cannot report success. */
    sync_object *invalid=event(0);
    CHECK(CloseHandle(invalid->host_event));
    import_record reset={0};
    strcpy(reset.dll,"KERNEL32.dll"); strcpy(reset.name,"ResetEvent");
    CPU *cpu=&fixture.threads[0].cpu;
    cpu->esp=0x10000; wr32(cpu->esp,0x1234); wr32(cpu->esp+4,invalid->handle);
    CHECK(handle_sync_import(cpu,&reset));
    CHECK(cpu->eax==0 && last_error==ERROR_INVALID_HANDLE);
    invalid->host_event=NULL;
    DeleteCriticalSection(&fixture.mutex);
}

static void critical_section_contract(void)
{
    reset_fixture(3);
    InitializeCriticalSection(&fixture.mutex);
    InitializeConditionVariable(&fixture.condition);
    import_record import={0};
    strcpy(import.dll,"KERNEL32.dll");
    const char *calls[]={"InitializeCriticalSection", "EnterCriticalSection",
        "EnterCriticalSection", "EnterCriticalSection", "LeaveCriticalSection",
        "LeaveCriticalSection", "LeaveCriticalSection"};
    const unsigned slots[]={0,0,1,2,0,1,2};
    for(unsigned i=0;i<7;++i) {
        active_guest_thread=&fixture.threads[slots[i]];
        CPU *cpu=&active_guest_thread->cpu;
        cpu->esp=0x10000; wr32(cpu->esp,0x1234); wr32(cpu->esp+4,0x10100);
        strcpy(import.name,calls[i]);
        CHECK(handle_critical_section_import(cpu,&import));
        if(i==3) CHECK(fixture.threads[1].blocked && fixture.threads[2].blocked);
        if(i==4) CHECK(rd32(0x1010c)==101 && !fixture.threads[1].blocked && fixture.threads[2].blocked);
        if(i==5) CHECK(rd32(0x1010c)==102 && !fixture.threads[2].blocked);
    }
    CHECK(rd32(0x1010c)==0 && rd32(0x10104)==UINT32_MAX);
    DeleteCriticalSection(&fixture.mutex);
}

static void quantum_contract(void)
{
    reset_fixture(2);
    InitializeCriticalSection(&fixture.mutex);
    InitializeConditionVariable(&fixture.condition);
    scheduler_return_slice_ns=1000000;
    fixture.threads[0].return_slice_deadline=guest_monotonic_ns()+1000000000;
    fixture.threads[0].checkpoint_cause=AC1_HANDOFF_IMPORT;
    scheduler_checkpoint(); /* eligible peer, but current quantum still valid */
    CHECK(fixture.owner==0);
    CHECK(fixture.threads[0].checkpoint_cause==AC1_HANDOFF_RETURN);
    sync_object *a=event(0);
    wait_on(0,a); wait_on(1,a);
    trace_wait_interval_ns=2000000000;
    fixture.trace_wait_deadline=guest_monotonic_ns()+trace_wait_interval_ns;
    uint32_t timeout=scheduler_next_timeout(&fixture);
    CHECK(timeout>0 && timeout<=2000); /* diagnostics survive total deadlock */
    trace_wait_interval_ns=0;
    CHECK(scheduler_next_timeout(&fixture)==0); /* normal infinite wait */
    DeleteCriticalSection(&fixture.mutex);
}

static unsigned progress[2];
enum { TRANSFER_ROUNDS=20000 };
static DWORD WINAPI transfer_worker(void *parameter)
{
    ac1_guest_thread *t=parameter;
    active_guest_thread=t;
    guest_address_space_select(fixture.space);
    scheduler_lock(&fixture);
    while(fixture.owner!=1) scheduler_wait_condition(&fixture,0);
    scheduler_unlock(&fixture);
    for(unsigned n=0;n<TRANSFER_ROUNDS;++n) {
        ++progress[1];
        if(n+1<TRANSFER_ROUNDS) {
            t->checkpoint_cause=AC1_HANDOFF_YIELD;
            scheduler_checkpoint();
        }
    }
    scheduler_lock(&fixture);
    t->finished=1;
    fixture.owner=0;
    scheduler_notify(&fixture);
    scheduler_unlock(&fixture);
    guest_address_space_select(NULL);
    return 0;
}

static void transfer_contract(guest_address_space *space)
{
    reset_fixture(2);
    InitializeCriticalSection(&fixture.mutex);
    InitializeConditionVariable(&fixture.condition);
    fixture.space=space;
    uint64_t start=guest_monotonic_ns();
    HANDLE worker=CreateThread(NULL,0,transfer_worker,&fixture.threads[1],0,NULL);
    CHECK(worker!=NULL);
    for(unsigned n=0;n<TRANSFER_ROUNDS;++n) {
        ++progress[0];
        active_guest_thread->checkpoint_cause=AC1_HANDOFF_YIELD;
        scheduler_checkpoint();
    }
    WaitForSingleObject(worker,INFINITE);
    CloseHandle(worker);
    uint64_t elapsed=guest_monotonic_ns()-start;
    CHECK(progress[0]==TRANSFER_ROUNDS && progress[1]==TRANSFER_ROUNDS);
    printf("baton ping-pong: %u transfers, %.3f us/transfer (wall, includes release/wait/wake/acquire)\n",
           TRANSFER_ROUNDS*2, elapsed/(double)(TRANSFER_ROUNDS*2)/1000.0);
    DeleteCriticalSection(&fixture.mutex);
}

int main(void)
{
    guest_address_space *space=guest_address_space_create();
    guest_address_space_select(space);
    CHECK(guest_memory_allocate(0x10000,0x1000));
    event_contract();
    signal_boundary_contract();
    critical_section_contract();
    quantum_contract();
    transfer_contract(space);
    sync_objects_destroy();
    active_scheduler=NULL; active_guest_thread=NULL;
    guest_address_space_select(NULL);
    guest_address_space_destroy(space);
    printf("scheduler contract: %u assertions passed\n",checks);
    return 0;
}
