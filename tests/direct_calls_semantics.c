#include "cpu.h"
#include <assert.h>
#include <stdio.h>
uint64_t ac1_dispatch_limit;
int ac1_direct_slow_mode;
uint8_t *ac1_direct_arena;
static unsigned slow, checkpoints, guard;
static ac1_call_state binding;
static unsigned char memory[65536];
void (guest_execution_read)(uint32_t a, void *o, size_t w) { assert((uint64_t)a+w<=sizeof memory); memcpy(o,memory+a,w); }
void (guest_execution_write)(uint32_t a,const void *i,size_t w) { assert((uint64_t)a+w<=sizeof memory);memcpy(memory+a,i,w); }
void ac1_unsupported_instruction(uint32_t va,const char *why) { (void)va;(void)why;abort(); }
void ac1_dispatch_guard_failed(CPU *c,uint32_t target) { (void)target;guard=ac1_dispatch_limit && c->calls->count>ac1_dispatch_limit ? 1:2; assert(c->calls->last_target==77); assert(c->calls->depth==(guard==1?0:4096)); exit(80+guard); }
void ac1_scheduler_return_checkpoint(CPU *c) { assert(c->calls); ++checkpoints; c->calls->returns=0; }
void L_00401000(CPU *);void L_00401100(CPU *);void L_00401200(CPU *);
void dispatch(CPU *c,uint32_t target) {
    ++slow;if(!c->calls)c->calls=&binding;
    ac1_call_state *s=c->calls;++s->count;
    if((ac1_dispatch_limit && s->count>ac1_dispatch_limit)||s->depth>=4096)ac1_dispatch_guard_failed(c,target);
    unsigned previous=s->last_target;s->last_target=target;++s->depth;
    if(target==0x401100)L_00401100(c);else if(target==0x401200)L_00401200(c);else abort();
    --s->depth;s->last_target=previous;if(++s->returns>=s->return_threshold)ac1_scheduler_return_checkpoint(c);
}
void dispatch_jmp(CPU *c,uint32_t target){dispatch(c,target);}
int main(int argc,char **argv) {
    if(argc>1) {
        ac1_call_state state={0};CPU c={0};c.calls=&state;state.last_target=77;
        if(argv[1][0]=='b'){ac1_dispatch_limit=1;state.count=1;}else state.depth=4096;
        AC1_CALL(&c,L_00401100,0x401100);abort();
    }
    ac1_direct_arena=memory;
    for(unsigned traced=0;traced<2;++traced)for(unsigned n=0;n<1000;++n) {
        ac1_call_state state={0};state.last_target=0x12345678;CPU c={0};c.calls=&state;c.esp=0x8000;c.eax=n;c.ebx=0xfefefefe;
        unsigned before=checkpoints;ac1_direct_slow_mode=(int)traced;
        L_00401000(&c);
        assert(c.eax==n+4 && c.edx==n+3 && c.ebx==0xfefefefe && c.esp==0x8004);
        assert(state.count==2 && state.depth==0 && state.last_target==0x12345678 && checkpoints==before+2);
        CPU saved=c;AC1_CALL(&c,L_00401100,0x401100);c=saved;
        assert(state.count==3 && c.calls==&state && state.depth==0 && state.last_target==0x12345678);
    }
    ac1_direct_slow_mode=0;CPU c={0};c.esp=0x8000;
    AC1_CALL(&c,L_00401100,0x401100);assert(c.calls==&binding && binding.count==1);
    { /* A threshold enters the runtime checkpoint once per that many returns. */
      ac1_call_state state={0};state.return_threshold=3;CPU t={0};t.calls=&state;t.esp=0x8000;
      unsigned before=checkpoints;
      for(unsigned n=0;n<6;++n){CPU saved=t;AC1_CALL(&t,L_00401100,0x401100);t=saved;}
      assert(checkpoints==before+2 && state.returns==0); }
    printf("direct transfers: 2000 nested/tail cases, tracing fallback, callback snapshots passed (slow=%u)\n",slow);
}
