/* Synthetic FLAGABI contract: both callsites use the same public target. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x10000];
static int version;
uint64_t ac1_dispatch_limit;
int ac1_direct_slow_mode;
void cfg_L_00401000(CPU *);
void cfg_L_00401100(CPU *);
void abi_L_00401000(CPU *);
void abi_L_00401100(CPU *);
void abi_local_L_00401000(CPU *);
void abi_local_L_00401100(CPU *);
void guest_execution_read(uint32_t a, void *o, size_t n) {
    assert((uint64_t)a+n <= sizeof memory);memcpy(o,memory+a,n);
}
void guest_execution_write(uint32_t a, const void *v, size_t n) {
    assert((uint64_t)a+n <= sizeof memory);memcpy(memory+a,v,n);
}
void ac1_dispatch_guard_failed(CPU *c,uint32_t t) {(void)c;(void)t;abort();}
void ac1_scheduler_return_checkpoint(CPU *c) {(void)c;}
void dispatch(CPU *c,uint32_t target) {
    assert(target==0x401100u);
    if(version==2)abi_local_L_00401100(c);
    else if(version)abi_L_00401100(c);else cfg_L_00401100(c);
}
static uint32_t random_state=0x12345678u;
static uint32_t random32(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;
    return random_state^=random_state<<5;
}
int main(void) {
    for(unsigned i=0;i<4096;++i) {
        CPU seed={0},actual,expected;
        ac1_call_state calls={0},expected_calls;
        unsigned char expected_memory[32];
        seed.eax=random32();seed.ebx=random32();seed.edx=random32();
        seed.cf=i&1;seed.pf=(i>>1)&1;seed.af=(i>>2)&1;
        seed.zf=(i>>3)&1;seed.sf=(i>>4)&1;seed.of=(i>>5)&1;
        seed.esp=0x8000;seed.calls=&calls;
        ac1_direct_slow_mode=(i>>6)&1;
        memset(memory+0x7ff0,0x55,32);
        actual=seed;version=0;
        if(i&128)cfg_L_00401100(&actual);else cfg_L_00401000(&actual);
        expected=actual;expected.calls=NULL;expected_calls=calls;
        memcpy(expected_memory,memory+0x7ff0,32);
        for(version=1;version<=2;++version) {
            memset(memory+0x7ff0,0x55,32);
            memset(&calls,0,sizeof calls);
            actual=seed;
            if(version==1) {
                if(i&128)abi_L_00401100(&actual);else abi_L_00401000(&actual);
            } else {
                if(i&128)abi_local_L_00401100(&actual);else abi_local_L_00401000(&actual);
            }
            actual.calls=NULL;
            assert(!memcmp(&actual,&expected,sizeof actual));
            assert(!memcmp(&calls,&expected_calls,sizeof calls));
            assert(!memcmp(expected_memory,memory+0x7ff0,32));
        }
    }
    puts("4096 public/private/fallback full CPU cases passed");return 0;
}
