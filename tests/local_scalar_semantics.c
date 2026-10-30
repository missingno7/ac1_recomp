/* Synthetic PUSH ESP / POP ESP / PUSHFD / shift-count and branch oracle. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x10000];
void plain_L_00401000(CPU *);
void local_L_00401000(CPU *);
void guest_execution_read(uint32_t a,void *o,size_t n) {
    assert((uint64_t)a+n<=sizeof memory);memcpy(o,memory+a,n);
}
void guest_execution_write(uint32_t a,const void *v,size_t n) {
    assert((uint64_t)a+n<=sizeof memory);memcpy(memory+a,v,n);
}
static uint32_t random_state=0x13579bdf;
static uint32_t random32(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;
    return random_state^=random_state<<5;
}
int main(void) {
    for(unsigned i=0;i<4096;++i) {
        CPU seed,actual,expected;
        unsigned char writes[0x100];
        memset(&seed,0x5a,sizeof seed);
        seed.eax=random32();seed.ecx=random32();seed.edx=random32();seed.ebx=random32();
        seed.ebp=random32();seed.edi=random32();seed.esi=(i&8)?random32():0;
        seed.esp=0x8000+(i&31);seed.calls=NULL;
        seed.cf=i&1;seed.pf=(i>>1)&1;seed.af=(i>>2)&1;
        seed.zf=(i>>3)&1;seed.sf=(i>>4)&1;seed.of=(i>>5)&1;
        memset(memory+0x7ff0,0xa5,0x100);
        actual=seed;plain_L_00401000(&actual);expected=actual;
        memcpy(writes,memory+0x7ff0,0x100);
        memset(memory+0x7ff0,0xa5,0x100);
        actual=seed;local_L_00401000(&actual);
        assert(actual.esp==seed.esp+0x44);
        assert(!memcmp(&actual,&expected,sizeof actual));
        assert(!memcmp(writes,memory+0x7ff0,0x100));
    }
    puts("4096 scalar-local full CPU and stack cases passed");return 0;
}
