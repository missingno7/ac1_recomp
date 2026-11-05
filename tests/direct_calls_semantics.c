#include "cpu.h"
#include <assert.h>
#include <stdio.h>
uint8_t *ac1_direct_arena;
static unsigned checkpoints;
static unsigned char memory[65536];
void (guest_execution_read)(uint32_t a, void *o, size_t w) { assert((uint64_t)a+w<=sizeof memory); memcpy(o,memory+a,w); }
void (guest_execution_write)(uint32_t a,const void *i,size_t w) { assert((uint64_t)a+w<=sizeof memory);memcpy(memory+a,i,w); }
void ac1_unsupported_instruction(uint32_t va,const char *why) { (void)va;(void)why;abort(); }
void ac1_scheduler_return_checkpoint(CPU *c) { ++checkpoints; c->returns=0; }
void L_00401000(CPU *);void L_00401100(CPU *);void L_00401200(CPU *);
void dispatch(CPU *c,uint32_t target) {
    if(target==0x401100)L_00401100(c);else if(target==0x401200)L_00401200(c);else abort();
}
void dispatch_jmp(CPU *c,uint32_t target){dispatch(c,target);}
int main(void) {
    ac1_direct_arena=memory;
    for(unsigned n=0;n<1000;++n) {
        CPU c={0};c.esp=0x8000;c.eax=n;c.ebx=0xfefefefe;
        L_00401000(&c);
        assert(c.eax==n+4 && c.edx==n+3 && c.ebx==0xfefefefe && c.esp==0x8004);
    }
    /* A counted call enters the runtime checkpoint once per interval. */
    CPU t={0};t.esp=0x8000;
    unsigned before=checkpoints;
    for(unsigned n=0;n<2*AC1_RETURN_INTERVAL;++n){AC1_CALL(&t,L_00401100,0x401100);t.esp=0x8000;}
    assert(checkpoints==before+2 && t.returns==0);
    printf("direct transfers: 1000 nested/tail cases and checkpoint interval passed\n");
}
