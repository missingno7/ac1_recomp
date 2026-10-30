/* Branch joins/backedges and CPU observations at an opaque helper boundary. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
static uint64_t digest;
static unsigned calls;
void plain_L_00401000(CPU *);
void local_L_00401000(CPU *);
void opaque(CPU *c) {
    const unsigned char *bytes=(const unsigned char *)c;
    for(size_t i=0;i<sizeof *c;++i)digest=(digest^bytes[i])*1099511628211ull;
    ++calls;c->eax^=1u;c->edi+=c->edx;c->esp+=4u;c->ebp+=1u;c->cf^=1u;
}
static uint32_t random_state=0x2468ace0;
static uint32_t random32(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;
    return random_state^=random_state<<5;
}
int main(void) {
    for(unsigned i=0;i<4096;++i) {
        CPU seed,actual,expected;
        memset(&seed,0x5a,sizeof seed);
        seed.eax=random32();seed.ecx=random32();seed.edx=random32();seed.ebx=random32();
        seed.esp=0x8000;seed.ebp=random32();seed.esi=random32();seed.edi=random32();seed.calls=NULL;
        seed.cf=i&1;
        actual=seed;digest=1469598103934665603ull;calls=0;plain_L_00401000(&actual);
        expected=actual;uint64_t expected_digest=digest;unsigned expected_calls=calls;
        assert(calls==(seed.ecx&7u)+1u);
        actual=seed;digest=1469598103934665603ull;calls=0;local_L_00401000(&actual);
        assert(calls==expected_calls && digest==expected_digest);
        assert(!memcmp(&actual,&expected,sizeof actual));
        assert(actual.esp==seed.esp+4u*calls+4u);
        assert(actual.ebp==seed.ebp+calls);
    }
    puts("4096 dirty CFG full CPU and observer cases passed");return 0;
}
