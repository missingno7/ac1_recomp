/* Whole-program flag contracts: one guest program under per-function CFG
 * masks, contract masks, and contract masks with local registers. Flags are
 * contracts only where guest code observes them, so the comparison covers every
 * register, memory byte and call count, and the flags the program captured. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x10000];
static int version;
void variant_dispatch(int version, CPU *c, uint32_t target);
void variant_entry(int version, CPU *c, unsigned entry);
extern const unsigned variant_entries;
void guest_execution_read(uint32_t a, void *o, size_t n) {
    assert((uint64_t)a+n <= sizeof memory);memcpy(o,memory+a,n);
}
void guest_execution_write(uint32_t a, const void *v, size_t n) {
    assert((uint64_t)a+n <= sizeof memory);memcpy(memory+a,v,n);
}
uint32_t guest_atomic_xadd32(uint32_t a, uint32_t v) {
    uint32_t old;guest_execution_read(a,&old,4);v+=old;guest_execution_write(a,&v,4);return old;
}
void ac1_scheduler_return_checkpoint(CPU *c) {(void)c;}
void dispatch(CPU *c,uint32_t target) {variant_dispatch(version,c,target);}
static uint32_t random_state=0x12345678u;
static uint32_t random32(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;
    return random_state^=random_state<<5;
}
int main(void) {
    for(unsigned i=0;i<4096;++i) {
        CPU seed={0},expected={0};
        ac1_call_state calls={0},expected_calls={0};
        unsigned char expected_memory[64];
        seed.eax=random32();seed.ebx=random32();seed.ecx=random32();seed.edx=random32();
        seed.esi=random32();seed.edi=random32();seed.ebp=random32()&7u;
        if(i&256)seed.edx=seed.esi;               /* equal operands: ZF/CF edge cases */
        seed.cf=i&1;seed.pf=(i>>1)&1;seed.af=(i>>2)&1;
        seed.zf=(i>>3)&1;seed.sf=(i>>4)&1;seed.of=(i>>5)&1;
        seed.esp=0x8000;
        for(version=0;version<3;++version) {
            CPU actual=seed;
            memset(&calls,0,sizeof calls);actual.calls=&calls;
            calls.stack_floor=(i>>6)&1?UINTPTR_MAX:0u;   /* tracing: every call through dispatch */
            memset(memory+0x7fe0,0x55,64);
            variant_entry(version,&actual,(i>>7)%variant_entries);
            actual.cf=actual.pf=actual.af=actual.zf=actual.sf=actual.of=0;actual.calls=NULL;
            if(!version) {
                expected=actual;expected_calls=calls;memcpy(expected_memory,memory+0x7fe0,64);
                continue;
            }
            assert(!memcmp(&actual,&expected,sizeof actual));
            assert(!memcmp(&calls,&expected_calls,sizeof calls));
            assert(!memcmp(expected_memory,memory+0x7fe0,64));
        }
    }
    puts("4096 CFG/contract/local whole-program cases passed");return 0;
}
