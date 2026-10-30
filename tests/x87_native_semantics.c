/* Native x87 lowering versus the exact lowering: every functional result. */
#include "cpu.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static unsigned char memory[0x10000];
void exact_L_00401000(CPU *);
void native_L_00401000(CPU *);
void guest_execution_read(uint32_t a,void *o,size_t n) {
    assert((uint64_t)a+n<=sizeof memory);memcpy(o,memory+a,n);
}
void guest_execution_write(uint32_t a,const void *v,size_t n) {
    assert((uint64_t)a+n<=sizeof memory);memcpy(memory+a,v,n);
}
void ac1_unsupported_instruction(uint32_t va,const char *reason) {
    fprintf(stderr,"unsupported %08x %s\n",va,reason);abort();
}
static uint32_t random_state=0x2468ace1;
static uint32_t random32(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;
    return random_state^=random_state<<5;
}
/* Fields the native lowering intentionally drops: tags, dirty mask, FPU IP/DP/opcode. */
static void functional(CPU *c) {
    c->fpu_tw=0;c->x87_dirty=0;c->fpu_ip=0;c->fpu_dp=0;c->fpu_opcode=0;c->fpu_cs=0;c->fpu_ds=0;
}
int main(void) {
    for(unsigned i=0;i<4096;++i) {
        CPU seed,exact,native;
        unsigned char expected[0x40];
        memset(&seed,0x5a,sizeof seed);
        seed.eax=random32();seed.ecx=random32();seed.esp=0x8000;seed.calls=NULL;
        seed.fp_initialized=1;seed.fpu_cw=0x0e7fu;seed.fpu_sw=0;seed.mmx_active=0;seed.mxcsr=0x1f80u;
        seed.fpu_top=(int)(random32()&7u);
        for(unsigned r=0;r<8;++r)seed.st[r]=(double)(int32_t)random32()/4096.0;
        float in[2]={(float)(int32_t)random32()/65536.0f,(i&3)?(float)(int32_t)random32()/65536.0f:0.0f};
        memset(memory+0x8000,0xa5,0x40);memcpy(memory+0x8004,in,sizeof in);
        exact=seed;exact_L_00401000(&exact);memcpy(expected,memory+0x8000,sizeof expected);
        memset(memory+0x8000,0xa5,0x40);memcpy(memory+0x8004,in,sizeof in);
        native=seed;native_L_00401000(&native);
        functional(&exact);functional(&native);
        assert(!memcmp(&exact,&native,sizeof exact));
        assert(!memcmp(expected,memory+0x8000,sizeof expected));
    }
    puts("4096 native-x87 functional CPU and memory cases passed");return 0;
}
