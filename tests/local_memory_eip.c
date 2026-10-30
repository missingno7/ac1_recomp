/* Full CPU/observer oracle and resumed real direct-memory read/write faults. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
uint8_t *ac1_direct_arena;
void full_L_00401000(CPU *);
void local_L_00401000(CPU *);
static CPU observed_cpu, *active_cpu;
static unsigned observer_calls, fault_calls;
static uint32_t expected_fault_eip;
void observe(CPU *c) {
    assert(c->eip == 0x00401003u);
    memcpy(&observed_cpu,c,sizeof *c); ++observer_calls;
}
void (guest_execution_read)(uint32_t a,void *p,size_t n) {(void)a;(void)p;(void)n;abort();}
void (guest_execution_write)(uint32_t a,const void *p,size_t n) {(void)a;(void)p;(void)n;abort();}
#ifdef _WIN32
static LONG CALLBACK fault_handler(EXCEPTION_POINTERS *p) {
    if(p->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a=(uintptr_t)p->ExceptionRecord->ExceptionInformation[1];
    if(a<(uintptr_t)ac1_direct_arena+0x8000 || a>=(uintptr_t)ac1_direct_arena+0x9000) return EXCEPTION_CONTINUE_SEARCH;
    assert(active_cpu && active_cpu->eip==expected_fault_eip);
    assert(VirtualAlloc(ac1_direct_arena+0x8000,0x1000,MEM_COMMIT,PAGE_READWRITE));
    ++fault_calls;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif
static uint32_t state=0x12345678u;
static uint32_t random32(void) {state^=state<<13;state^=state>>17;return state^=state<<5;}
int main(void) {
#ifdef _WIN32
    ac1_direct_arena=VirtualAlloc(NULL,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(ac1_direct_arena);
    void *handler=AddVectoredExceptionHandler(1,fault_handler);assert(handler);
#else
    ac1_direct_arena=calloc(1,0x10000);assert(ac1_direct_arena);
#endif
    void (*functions[])(CPU *)={full_L_00401000,local_L_00401000};
    for(unsigned i=0;i<4096;++i) {
        CPU seed,expected,expected_observer;
        memset(&seed,0x5a,sizeof seed);
        seed.eax=random32();seed.ecx=random32();seed.edx=random32();seed.esp=0x9000;
        for(unsigned version=0;version<2;++version) {
            CPU c;memcpy(&c,&seed,sizeof c);active_cpu=&c;
            uint32_t value=random32();
            /* Keep the same memory input for every version. */
            if(version==0) memcpy(ac1_direct_arena+0x8000,&value,4);
            unsigned before=observer_calls;functions[version](&c);
            assert(observer_calls==before+1 && c.eip==0x00401004u);
            if(version==0){memcpy(&expected,&c,sizeof c);memcpy(&expected_observer,&observed_cpu,sizeof c);}
            else {assert(!memcmp(&expected,&c,sizeof c));assert(!memcmp(&expected_observer,&observed_cpu,sizeof c));}
        }
    }
#ifdef _WIN32
    for(unsigned version=0;version<2;++version) {
        CPU c={0};c.esp=0x9000;active_cpu=&c;expected_fault_eip=0x00401002u;
        assert(VirtualFree(ac1_direct_arena+0x8000,0x1000,MEM_DECOMMIT));
        unsigned before=fault_calls;functions[version](&c);
        assert(fault_calls==before+1 && c.ebx==0);
    }
    assert(RemoveVectoredExceptionHandler(handler));
    assert(VirtualFree(ac1_direct_arena,0,MEM_RELEASE));
#else
    free(ac1_direct_arena);
#endif
    puts("4096 full CPU/observer cases and precise direct fault EIPs passed");return 0;
}
