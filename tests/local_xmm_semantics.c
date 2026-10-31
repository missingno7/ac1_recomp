/* XMM locals: full CPU at guest calls, indirect transfers and both returns. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t digest;
static unsigned calls;
void ac1_scheduler_return_checkpoint(CPU *c) { (void)c; }
void plain_L_00401000(CPU *);
void local_L_00401000(CPU *);
static void observe(CPU *c, uint32_t salt) {
    const unsigned char *bytes=(const unsigned char *)c;
    for(size_t i=0;i<sizeof *c;++i)digest=(digest^bytes[i])*1099511628211ull;
    digest^=salt;++calls;
}
/* A callee sees every pending XMM write and its own writes reach the caller. */
void L_00401100(CPU *c) {
    observe(c,0x1100u);c->xmm[0].u32[2]^=0x9e3779b9u;c->xmm[3].f32[1]+=1.5f;c->esp+=4u;
}
void dispatch(CPU *c, uint32_t target) {
    observe(c,target);c->xmm[1].u64[0]=~c->xmm[1].u64[0];c->xmm[2].u32[3]=c->eax;c->esp+=4u;
}
void opaque(CPU *c) { observe(c,0xabcdu);c->xmm[2].f32[0]*=-2.0f; }
static uint32_t random_state=0x13579bdf;
static uint32_t random32(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;
    return random_state^=random_state<<5;
}
int main(void) {
    for(unsigned i=0;i<4096;++i) {
        CPU seed,actual,expected;
        memset(&seed,0x5a,sizeof seed);
        seed.eax=random32();seed.ecx=random32();seed.esp=0x8000;static ac1_call_state state;seed.calls=&state;
        for(unsigned r=0;r<8;++r)for(unsigned k=0;k<4;++k)seed.xmm[r].f32[k]=(float)(int32_t)random32()/65536.0f;
        actual=seed;digest=1469598103934665603ull;calls=0;plain_L_00401000(&actual);
        expected=actual;uint64_t expected_digest=digest;unsigned expected_calls=calls;
        assert(calls==((seed.eax&1u)?2u:3u));
        actual=seed;digest=1469598103934665603ull;calls=0;local_L_00401000(&actual);
        assert(calls==expected_calls && digest==expected_digest);
        assert(!memcmp(&actual,&expected,sizeof actual));
    }
    puts("4096 XMM-local full CPU, call and return cases passed");return 0;
}
