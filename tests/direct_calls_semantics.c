#include "cpu.h"
#include <assert.h>
#include <stdio.h>
uint8_t *ac1_direct_arena;
static unsigned slow, checkpoints;
static ac1_call_state binding;
static unsigned char memory[65536];
void (guest_execution_read)(uint32_t a, void *o, size_t w) { assert((uint64_t)a+w<=sizeof memory); memcpy(o,memory+a,w); }
void (guest_execution_write)(uint32_t a,const void *i,size_t w) { assert((uint64_t)a+w<=sizeof memory);memcpy(memory+a,i,w); }
void ac1_unsupported_instruction(uint32_t va,const char *why) { (void)va;(void)why;abort(); }
/* The runtime's checkpoint settles the returns into the dispatch budget. */
void ac1_scheduler_return_checkpoint(CPU *c) { assert(c->calls); ++checkpoints; c->calls->count+=c->calls->returns; c->calls->returns=0; }
void L_00401000(CPU *);void L_00401100(CPU *);void L_00401200(CPU *);
void dispatch(CPU *c,uint32_t target) {
    ++slow;if(!c->calls)c->calls=&binding;
    ac1_call_state *s=c->calls;
    if(AC1_HOST_SP()<s->stack_limit)exit(82);   /* the runtime stops: guest call depth exceeded */
    unsigned previous=s->last_target;s->last_target=target;
    if(target==0x401100)L_00401100(c);else if(target==0x401200)L_00401200(c);else abort();
    s->last_target=previous;if(++s->returns>=s->return_threshold)ac1_scheduler_return_checkpoint(c);
}
void dispatch_jmp(CPU *c,uint32_t target){dispatch(c,target);}
int main(int argc,char **argv) {
    if(argc>1) {   /* a floor above the host stack pointer leaves the call to the dispatcher */
        ac1_call_state state={0};CPU c={0};c.calls=&state;state.stack_floor=state.stack_limit=UINTPTR_MAX;
        AC1_CALL(&c,L_00401100,0x401100);abort();
    }
    ac1_direct_arena=memory;
    for(unsigned traced=0;traced<2;++traced)for(unsigned n=0;n<1000;++n) {
        ac1_call_state state={0};state.last_target=0x12345678;state.stack_floor=traced?UINTPTR_MAX:0u;
        CPU c={0};c.calls=&state;c.esp=0x8000;c.eax=n;c.ebx=0xfefefefe;
        unsigned before=checkpoints,dispatched=slow;
        L_00401000(&c);
        assert(c.eax==n+4 && c.edx==n+3 && c.ebx==0xfefefefe && c.esp==0x8004);
        assert(state.count==2 && state.last_target==0x12345678 && checkpoints==before+2);
#ifdef AC1_NO_DIRECT_CALLS
        assert(slow-dispatched==2u);
#else
        assert(slow-dispatched==(traced?2u:0u));
#endif
        CPU saved=c;AC1_CALL(&c,L_00401100,0x401100);c=saved;
        assert(state.count==3 && c.calls==&state && state.last_target==0x12345678);
    }
    CPU c={0};c.esp=0x8000;
    AC1_CALL(&c,L_00401100,0x401100);assert(c.calls==&binding && binding.count==1);
    { /* A threshold enters the runtime checkpoint once per that many returns. */
      ac1_call_state state={0};state.return_threshold=3;CPU t={0};t.calls=&state;t.esp=0x8000;
      unsigned before=checkpoints;
      for(unsigned n=0;n<6;++n){CPU saved=t;AC1_CALL(&t,L_00401100,0x401100);t=saved;}
      assert(checkpoints==before+2 && state.returns==0 && state.count==6); }
    printf("direct transfers: 2000 nested/tail cases, tracing fallback, callback snapshots passed (slow=%u)\n",slow);
}
