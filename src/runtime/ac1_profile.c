/* In-process Windows x64 sampler. Only GetThreadContext/QueryThreadCycleTime,
 * fixed-size copies and ResumeThread occur while a target is suspended.
 * In particular module enumeration, allocation and all I/O happen afterwards.
 * Samples are statistical CPU-activity observations, not stack unwinds. */
#include "ac1_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int ac1_profile_enabled, ac1_profile_frames_enabled;
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#define MAX_THREADS 2048
#define MAX_GUESTS 32
#define MAX_MODULES 4096
#define SAMPLE_CAP 262144
#define FRAME_CAP 65536
_Thread_local ac1_profile_counts *ac1_profile_current;
typedef struct {
    uint64_t tick, rip, cycles;
    uint32_t tid, guest, eip, flags, module, reserved;
} sample; /* 48 bytes; flags: CPU delta nonzero=1, baton=2, baseline=4,
           * currently parked at a known kernel wait stub=8 */
typedef struct {
    volatile LONG tid;
    volatile LONG finished;
    uint32_t id;
    unsigned slot;
    const uint32_t *eip;
    const unsigned *owner;
    ac1_profile_counts counts;
    ac1_profile_counts snapshot;
    uint64_t snapshot_tick;
} guest_info;
static _Thread_local guest_info *current_guest;
typedef struct { DWORD tid; HANDLE handle; ULONG64 cycles; int baseline; unsigned cpu_index; volatile LONG busy; } thread_info;
typedef struct { DWORD tid; uint64_t first, last; } cpu_info;
static cpu_info cpu_history[MAX_THREADS];
static unsigned cpu_count;
typedef struct { uint64_t base, size, tick; char path[1024]; int active; } module_info;
static guest_info guests[MAX_GUESTS];
static thread_info threads[MAX_THREADS];
static module_info modules[MAX_MODULES];
static unsigned thread_count, module_count, sample_count, sequence;
static sample *samples;
static uint64_t frames[FRAME_CAP], frame_read;
static volatile LONG64 frame_write;
static HANDLE stop_event, sampler, native_sampler;
static DWORD sampler_id, native_sampler_id;
/* Never acquired by game threads, and never acquired while a target is paused. */
static SRWLOCK records_lock=SRWLOCK_INIT;
static SRWLOCK suspension_gate=SRWLOCK_INIT;
static uint64_t frequency, start_tick, interval_start, dropped, errors, frame_dropped;
static double interval_seconds;
static char output_dir[32768];
static uint64_t wait_stubs[32];
static unsigned wait_stub_count;
static uint64_t now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static void init_wait_stubs(void)
{
    const char *names[]={"NtWaitForSingleObject","NtWaitForMultipleObjects",
        "NtDelayExecution","NtWaitForAlertByThreadId","NtRemoveIoCompletion",
        "NtRemoveIoCompletionEx","NtSignalAndWaitForSingleObject",
        "NtWaitForWorkViaWorkerFactory","NtWaitForKeyedEvent","NtReleaseKeyedEvent"};
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll"); wait_stub_count=0;
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        FARPROC p=GetProcAddress(ntdll,names[i]);
        if(p) wait_stubs[wait_stub_count++]=(uintptr_t)p;
    }
    HMODULE win32u=GetModuleHandleW(L"win32u.dll");
    const char *graphics_waits[]={"NtUserGetMessage","NtUserMsgWaitForMultipleObjectsEx",
        "NtUserWaitMessage","NtGdiDdDDIWaitForVerticalBlankEvent",
        "NtGdiDdDDIWaitForVerticalBlankEvent2","NtGdiDdDDIWaitForSynchronizationObject",
        "NtGdiDdDDIWaitForSynchronizationObjectFromCpu"};
    if(win32u) for(unsigned i=0;i<sizeof(graphics_waits)/sizeof(graphics_waits[0]);++i) {
        FARPROC p=GetProcAddress(win32u,graphics_waits[i]);
        if(p) wait_stubs[wait_stub_count++]=(uintptr_t)p;
    }
}
static uint64_t cpu_time(HANDLE h)
{
    FILETIME creation,exit,kernel,user;
    if(!GetThreadTimes(h,&creation,&exit,&kernel,&user)) return 0;
    return ((uint64_t)kernel.dwHighDateTime<<32)+kernel.dwLowDateTime+
           ((uint64_t)user.dwHighDateTime<<32)+user.dwLowDateTime;
}

static unsigned remember_module(uint64_t base, uint64_t size, const char *path, uint64_t tick)
{
    for (unsigned i=0;i<module_count;++i)
        if (modules[i].base==base && modules[i].size==size && !strcmp(modules[i].path,path)) {
            modules[i].active=1; return i+1;
        }
    if (module_count==MAX_MODULES) { ++errors; return 0; }
    module_info *m=&modules[module_count++];
    m->base=base; m->size=size; m->tick=tick; m->active=1;
    snprintf(m->path,sizeof(m->path),"%s",path);
    return module_count;
}
static void refresh_modules(uint64_t tick)
{
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
    if (snap==INVALID_HANDLE_VALUE) { ++errors; return; }
    for (unsigned i=0;i<module_count;++i) modules[i].active=0;
    MODULEENTRY32 me; memset(&me,0,sizeof(me)); me.dwSize=sizeof(me);
    if (Module32First(snap,&me)) do {
        remember_module((uintptr_t)me.modBaseAddr,me.modBaseSize,me.szExePath,tick);
    } while (Module32Next(snap,&me));
    CloseHandle(snap);
}
static unsigned module_at(uint64_t rip, uint64_t tick, int resolve)
{
    for (unsigned i=0;i<module_count;++i)
        if (modules[i].active && rip>=modules[i].base && rip-modules[i].base<modules[i].size) return i+1;
    if(!resolve) return 0;
    /* Unknown/new module: resolve after resume, retaining a reference while
     * reading its PE header. Never enter the loader while a target is paused. */
    HMODULE h;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCWSTR)(uintptr_t)rip,&h)) {
        char path[1024]; unsigned id=0;
        if (GetModuleFileNameA(h,path,sizeof(path))) {
            IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)h;
            IMAGE_NT_HEADERS *nt=(IMAGE_NT_HEADERS *)((char *)h+dos->e_lfanew);
            id=remember_module((uintptr_t)h,nt->OptionalHeader.SizeOfImage,path,tick);
        }
        FreeLibrary(h); return id;
    }
    return 0;
}
static void refresh_threads(void)
{
    /* Keep records/handles stable until both samplers join. Pinned handles also
     * distinguish a reused TID from a previous thread object. */
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap==INVALID_HANDLE_VALUE) { ++errors; return; }
    THREADENTRY32 te; memset(&te,0,sizeof(te)); te.dwSize=sizeof(te);
    if (Thread32First(snap,&te)) do {
        if (te.th32OwnerProcessID!=GetCurrentProcessId() || te.th32ThreadID==sampler_id || te.th32ThreadID==native_sampler_id) continue;
        unsigned i; for(i=0;i<thread_count;++i)
            if(threads[i].tid==te.th32ThreadID && WaitForSingleObject(threads[i].handle,0)!=WAIT_OBJECT_0) break;
        if(i<thread_count) continue;
        if(thread_count==MAX_THREADS || cpu_count==MAX_THREADS) { ++errors; continue; }
        HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION|SYNCHRONIZE,FALSE,te.th32ThreadID);
        if(!h) { ++errors; continue; }
        uint64_t cpu=cpu_time(h);
        cpu_history[cpu_count]=(cpu_info){te.th32ThreadID,cpu,cpu};
        threads[thread_count++]=(thread_info){te.th32ThreadID,h,0,0,cpu_count++,0};
    } while(Thread32Next(snap,&te));
    CloseHandle(snap);
}
static void take_samples(int native)
{
    AcquireSRWLockShared(&records_lock);
    unsigned count=thread_count;
    ReleaseSRWLockShared(&records_lock);
    for(unsigned i=0;i<count;++i) {
        thread_info *t=&threads[i];
        if(WaitForSingleObject(t->handle,0)==WAIT_OBJECT_0) continue;
        guest_info *g=NULL; unsigned slot=0;
        for(unsigned j=0;j<MAX_GUESTS;++j)
            if((DWORD)InterlockedCompareExchange(&guests[j].tid,0,0)==t->tid &&
               !InterlockedCompareExchange(&guests[j].finished,0,0)) { g=&guests[j]; slot=j; break; }
        if((g==NULL)!=native || InterlockedCompareExchange(&t->busy,1,0)) continue;
        CONTEXT context; memset(&context,0,sizeof(context)); context.ContextFlags=CONTEXT_CONTROL;
        sample s={0}; s.tick=now(); s.tid=t->tid;
        ac1_profile_counts counts={0};
        AcquireSRWLockShared(&suspension_gate);
        if(SuspendThread(t->handle)==(DWORD)-1) {
            ReleaseSRWLockShared(&suspension_gate);
            if(WaitForSingleObject(t->handle,0)!=WAIT_OBJECT_0) {
                AcquireSRWLockExclusive(&records_lock); ++errors; ReleaseSRWLockExclusive(&records_lock);
            }
            InterlockedExchange(&t->busy,0);
            continue;
        }
        /* TARGET SUSPENDED: no allocation, logging, loader or runtime locks. */
        BOOL ok=GetThreadContext(t->handle,&context);
        ULONG64 cycles=0; BOOL cycle_ok=QueryThreadCycleTime(t->handle,&cycles);
        if(ok) s.rip=context.Rip;
        if(cycle_ok && t->baseline) { s.cycles=cycles-t->cycles; if(s.cycles) s.flags|=1; }
        else s.flags|=4;
        if(g) {
            s.guest=g->id; s.eip=*(const volatile uint32_t *)g->eip;
            if(*(const volatile unsigned *)g->owner==slot) s.flags|=2;
            counts=g->counts;
        }
        DWORD resumed=ResumeThread(t->handle);
        ReleaseSRWLockShared(&suspension_gate);
        /* TARGET RESUMED */
        AcquireSRWLockExclusive(&records_lock);
        if(g) { g->snapshot=counts; g->snapshot_tick=s.tick; }
        if(resumed==(DWORD)-1) { ++errors; SetEvent(stop_event); }
        if(cycle_ok) { t->cycles=cycles; t->baseline=1; }
        if(!ok) { ++errors; ReleaseSRWLockExclusive(&records_lock); InterlockedExchange(&t->busy,0); continue; }
        /* Suspending/getting context can itself charge a sleeping thread a few
         * kernel cycles. Preserve the raw delta, and separately mark canonical
         * syscall wait sites so those cycles do not look like CPU hotspots. */
        for(unsigned j=0;j<wait_stub_count;++j)
            if(s.rip>=wait_stubs[j] && s.rip-wait_stubs[j]<32) { s.flags|=8; break; }
        s.module=module_at(s.rip,s.tick,0);
        if(sample_count<SAMPLE_CAP) samples[sample_count++]=s; else ++dropped;
        ReleaseSRWLockExclusive(&records_lock);
        InterlockedExchange(&t->busy,0);
    }
}
static void json_string(FILE *f,const char *s)
{
    fputc('"',f);
    for(;*s;++s) { unsigned char c=(unsigned char)*s; if(c=='"'||c=='\\') fputc('\\',f); if(c>=32) fputc(c,f); }
    fputc('"',f);
}
static void flush(uint64_t end)
{
    AcquireSRWLockExclusive(&suspension_gate);
    AcquireSRWLockExclusive(&records_lock);
    for(unsigned i=0;i<sample_count;++i)
        if(!samples[i].module) samples[i].module=module_at(samples[i].rip,samples[i].tick,1);
    char path[32768]; snprintf(path,sizeof(path),"%s/raw_%u.bin",output_dir,sequence);
    FILE *f=fopen(path,"wb");
    if(!f) { fprintf(stderr,"AC1 profile: cannot write %s\n",path); ++errors; ReleaseSRWLockExclusive(&records_lock); ReleaseSRWLockExclusive(&suspension_gate); return; }
    if(fwrite(samples,sizeof(sample),sample_count,f)!=sample_count) ++errors;
    fclose(f);
    snprintf(path,sizeof(path),"%s/raw_%u.json",output_dir,sequence++);
    f=fopen(path,"w"); if(!f) { ++errors; ReleaseSRWLockExclusive(&records_lock); ReleaseSRWLockExclusive(&suspension_gate); return; }
    fprintf(f,"{\"version\":1,\"pid\":%lu,\"mode\":\"%s\",\"frequency\":%llu,\"run_start\":%llu,\"start\":%llu,\"end\":%llu,\"samples\":%u,\"dropped\":%llu,\"errors\":%llu,\"sampler_tid\":%lu,\"modules\":[",(unsigned long)GetCurrentProcessId(),ac1_profile_enabled?"sampling":"frames",(unsigned long long)frequency,(unsigned long long)start_tick,(unsigned long long)interval_start,(unsigned long long)end,sample_count,(unsigned long long)dropped,(unsigned long long)errors,(unsigned long)sampler_id);
    for(unsigned i=0;i<module_count;++i) { module_info *m=&modules[i]; if(i) fputc(',',f); fprintf(f,"{\"base\":%llu,\"size\":%llu,\"tick\":%llu,\"path\":",(unsigned long long)m->base,(unsigned long long)m->size,(unsigned long long)m->tick); json_string(f,m->path); fputc('}',f); }
    fprintf(f,"],\"threads\":[");
    for(unsigned i=0;i<thread_count;++i) cpu_history[threads[i].cpu_index].last=cpu_time(threads[i].handle);
    for(unsigned i=0;i<cpu_count;++i) {
        if(i) fputc(',',f);
        fprintf(f,"{\"tid\":%lu,\"cpu_100ns\":%llu}",(unsigned long)cpu_history[i].tid,
                (unsigned long long)(cpu_history[i].last-cpu_history[i].first));
    }
    fprintf(f,"],\"guests\":["); int comma=0;
    for(unsigned i=0;i<MAX_GUESTS;++i) if(InterlockedCompareExchange(&guests[i].tid,0,0)) {
        guest_info *g=&guests[i];
        ac1_profile_counts c=InterlockedCompareExchange(&g->finished,0,0) ? g->counts : g->snapshot;
        uint64_t wait=c.wait_ticks+(c.wait_start && end>c.wait_start ? end-c.wait_start : 0);
        if(comma++) fputc(',',f);
        fprintf(f,"{\"tid\":%ld,\"guest\":%u,\"snapshot_tick\":%llu,\"dispatches\":%llu,\"misses\":%llu,\"slow\":%llu,\"scanned\":%llu,\"handoffs\":%llu,\"wait_ticks\":%llu,\"transfer_count\":%llu,\"transfer_ns\":%llu,\"transfer_max_ns\":%llu,\"handoff_causes\":[",g->tid,g->id,(unsigned long long)g->snapshot_tick,(unsigned long long)c.dispatches,(unsigned long long)c.misses,(unsigned long long)c.slow,(unsigned long long)c.scanned,(unsigned long long)c.handoffs,(unsigned long long)wait,(unsigned long long)c.transfer_count,(unsigned long long)c.transfer_ns,(unsigned long long)c.transfer_max_ns);
        for(unsigned h=0;h<AC1_HANDOFF_CAUSES;++h) fprintf(f,"%s%llu",h?",":"",(unsigned long long)c.handoff_causes[h]);
        fprintf(f,"],\"io_wait_completions\":%llu}",(unsigned long long)c.io_wait_completions);
    }
    fprintf(f,"],\"frames\":["); comma=0;
    uint64_t written=(uint64_t)InterlockedCompareExchange64(&frame_write,0,0);
    if(written-frame_read>FRAME_CAP) { frame_dropped+=written-frame_read-FRAME_CAP; frame_read=written-FRAME_CAP; }
    while(frame_read<written) { if(comma++) fputc(',',f); fprintf(f,"%llu",(unsigned long long)frames[frame_read++%FRAME_CAP]); }
    fprintf(f,"],\"present_count\":%llu,\"frame_dropped\":%llu,\"sampler_cpu_100ns\":%llu}\n",(unsigned long long)written,(unsigned long long)frame_dropped,(unsigned long long)(cpu_time(GetCurrentThread())+(native_sampler?cpu_time(native_sampler):0))); fclose(f);
    fprintf(stderr,"AC1 profile: interval %u samples=%u errors=%llu dropped=%llu\n",sequence-1,sample_count,(unsigned long long)errors,(unsigned long long)dropped);
    sample_count=0; interval_start=end;
    ReleaseSRWLockExclusive(&records_lock);
    ReleaseSRWLockExclusive(&suspension_gate);
}
static DWORD WINAPI native_sample_thread(void *unused)
{
    (void)unused;
    while(WaitForSingleObject(stop_event,1)!=WAIT_OBJECT_0) take_samples(1);
    return 0;
}
static DWORD WINAPI sample_thread(void *unused)
{
    (void)unused; uint64_t refresh=0, deadline=now();
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,0x00000002,TIMER_ALL_ACCESS);
    if(!timer) timer=CreateWaitableTimerW(NULL,FALSE,NULL);
    HANDLE events[2]={stop_event,timer};
    for(;;) {
        uint64_t tick=now();
        if(ac1_profile_enabled) {
            if(tick>=refresh) {
                AcquireSRWLockExclusive(&suspension_gate);
                AcquireSRWLockExclusive(&records_lock);
                refresh_threads(); refresh_modules(tick); refresh=tick+frequency/4;
                ReleaseSRWLockExclusive(&records_lock);
                ReleaseSRWLockExclusive(&suspension_gate);
            }
            take_samples(0);
        }
        tick=now();
        AcquireSRWLockShared(&records_lock);
        int full=sample_count+thread_count>=SAMPLE_CAP;
        ReleaseSRWLockShared(&records_lock);
        if(tick-interval_start>=(uint64_t)(interval_seconds*frequency) || full) flush(tick);
        deadline+=ac1_profile_enabled ? frequency/1000 : frequency/10;
        if(deadline<=tick) deadline=tick+frequency/1000;
        LARGE_INTEGER due; due.QuadPart=-(LONGLONG)((deadline-tick)*10000000/frequency);
        if(!timer || !SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE)) {
            if(WaitForSingleObject(stop_event,1)==WAIT_OBJECT_0) break;
        } else if(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0) break;
    }
    /* Final samples keep per-thread counters current after stop has been requested. */
    if(native_sampler) WaitForSingleObject(native_sampler,INFINITE);
    if(ac1_profile_enabled) take_samples(0);
    flush(now());
    for(unsigned i=0;i<thread_count;++i) CloseHandle(threads[i].handle);
    if(timer) CloseHandle(timer);
    return 0;
}
void ac1_profile_init(void)
{
    const char *setting=getenv("AC1_PROFILE"), *frames_only=getenv("AC1_PROFILE_FRAMES");
    if(!(setting && !strcmp(setting,"1")) && !(frames_only && !strcmp(frames_only,"1"))) return;
    memset(guests,0,sizeof(guests)); ac1_profile_current=NULL; current_guest=NULL; cpu_count=0;
    thread_count=module_count=sample_count=sequence=0; frame_read=0; frame_write=0; dropped=errors=frame_dropped=0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); frequency=f.QuadPart;
    start_tick=interval_start=now();
    init_wait_stubs();
    interval_seconds=10; const char *iv=getenv("AC1_PROFILE_INTERVAL");
    if(iv) { double v=strtod(iv,NULL); if(v>=0.1 && v<=3600) interval_seconds=v; }
    /* Locate the worktree via the executable, not the guest current directory. */
    GetModuleFileNameA(NULL,output_dir,sizeof(output_dir));
    char *p=strrchr(output_dir,'\\'); if(!p) return; *p=0;
    p=strrchr(output_dir,'\\'); if(!p) return; *p=0;
    p=strrchr(output_dir,'\\'); if(!p) return; *p=0;
    strncat(output_dir,"\\work\\profile",sizeof(output_dir)-strlen(output_dir)-1);
    const char *directory=getenv("AC1_PROFILE_DIR");
    if(directory && *directory) snprintf(output_dir,sizeof(output_dir),"%s",directory);
    CreateDirectoryA(output_dir,NULL);
    /* Append captures across runs; an interrupted run must not leave old
     * higher-numbered files masquerading as part of a subsequent run. */
    char pattern[32768]; snprintf(pattern,sizeof(pattern),"%s/raw_*.json",output_dir);
    WIN32_FIND_DATAA entry; HANDLE find=FindFirstFileA(pattern,&entry);
    if(find!=INVALID_HANDLE_VALUE) {
        do { unsigned n; if(sscanf(entry.cFileName,"raw_%u.json",&n)==1 && n>=sequence) sequence=n+1; } while(FindNextFileA(find,&entry));
        FindClose(find);
    }
    samples=(sample *)VirtualAlloc(NULL,sizeof(sample)*SAMPLE_CAP,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!samples || !stop_event) { fprintf(stderr,"AC1 profile: allocation/event failed %lu\n",GetLastError()); ac1_profile_shutdown(); return; }
    ac1_profile_enabled=setting && !strcmp(setting,"1"); ac1_profile_frames_enabled=1;
    native_sampler_id=0;
    if(ac1_profile_enabled) {
        native_sampler=CreateThread(NULL,0,native_sample_thread,NULL,CREATE_SUSPENDED,&native_sampler_id);
        if(!native_sampler) { fprintf(stderr,"AC1 profile: native sampler CreateThread failed %lu\n",GetLastError()); ac1_profile_shutdown(); return; }
    }
    sampler=CreateThread(NULL,0,sample_thread,NULL,CREATE_SUSPENDED,&sampler_id);
    if(!sampler) { fprintf(stderr,"AC1 profile: CreateThread failed %lu\n",GetLastError()); ac1_profile_shutdown(); return; }
    if(native_sampler) ResumeThread(native_sampler);
    ResumeThread(sampler);
}
void ac1_profile_guest(unsigned slot,uint32_t id,const uint32_t *eip,const unsigned *owner)
{
    if(!ac1_profile_enabled || slot>=MAX_GUESTS) return;
    guest_info *g=&guests[slot]; g->id=id; g->slot=slot; g->eip=eip; g->owner=owner;
    ac1_profile_current=&g->counts; current_guest=g;
    InterlockedExchange(&g->tid,(LONG)GetCurrentThreadId());
}
void ac1_profile_guest_done(void)
{
    if(current_guest) InterlockedExchange(&current_guest->finished,1);
    current_guest=NULL; ac1_profile_current=NULL;
}
uint64_t ac1_profile_wait_begin(void)
{
    if(!ac1_profile_enabled || !current_guest || *current_guest->owner==current_guest->slot) return 0;
    return ac1_profile_local.wait_start=now();
}
void ac1_profile_wait_end(uint64_t start) { if(start) { ac1_profile_local.wait_ticks+=now()-start; ac1_profile_local.wait_start=0; } }
void ac1_profile_handoff(ac1_handoff_cause cause)
{
    if (!ac1_profile_enabled || !ac1_profile_current) return;
    ++ac1_profile_local.handoffs;
    ++ac1_profile_local.handoff_causes[cause];
}
void ac1_profile_transfer(uint64_t ns)
{
    if (!ac1_profile_enabled || !ac1_profile_current) return;
    ++ac1_profile_local.transfer_count;
    ac1_profile_local.transfer_ns += ns;
    if (ns > ac1_profile_local.transfer_max_ns) ac1_profile_local.transfer_max_ns = ns;
}
void ac1_profile_present(void)
{
    /* Guest Present calls are serialized by the baton. Publish after writing. */
    uint64_t index=(uint64_t)InterlockedCompareExchange64(&frame_write,0,0);
    frames[index%FRAME_CAP]=now(); InterlockedExchange64(&frame_write,(LONG64)(index+1));
}
void ac1_profile_shutdown(void)
{
    if(sampler) { SetEvent(stop_event); WaitForSingleObject(sampler,INFINITE); CloseHandle(sampler); sampler=NULL; }
    if(native_sampler) { SetEvent(stop_event); ResumeThread(native_sampler); WaitForSingleObject(native_sampler,INFINITE); CloseHandle(native_sampler); native_sampler=NULL; }
    ac1_profile_enabled=ac1_profile_frames_enabled=0;
    if(stop_event) { CloseHandle(stop_event); stop_event=NULL; }
    if(samples) { VirtualFree(samples,0,MEM_RELEASE); samples=NULL; }
}
#else
void ac1_profile_init(void) {}
void ac1_profile_shutdown(void) {}
void ac1_profile_guest(unsigned s,uint32_t id,const uint32_t *e,const unsigned *o) { (void)s;(void)id;(void)e;(void)o; }
void ac1_profile_guest_done(void) {}
uint64_t ac1_profile_wait_begin(void) { return 0; }
void ac1_profile_wait_end(uint64_t s) { (void)s; }
void ac1_profile_handoff(ac1_handoff_cause c) { (void)c; }
void ac1_profile_transfer(uint64_t ns) { (void)ns; }
void ac1_profile_present(void) {}
#endif
