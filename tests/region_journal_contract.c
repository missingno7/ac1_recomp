#define AC1_REGION_JOURNAL_ADAPTERS
#include "ac1_region_journal.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
static unsigned char memory[65536];
static jmp_buf trap;
static char error[300];
static unsigned effects,mode;
void guest_execution_read(uint32_t a,void *o,size_t n) { assert(a+n<=sizeof(memory));memcpy(o,memory+a,n); }
void guest_execution_write(uint32_t a,const void *o,size_t n) { assert(a+n<=sizeof(memory));memcpy(memory+a,o,n); }
void *guest_memory_try_ptr(uint32_t a,size_t n) { return a+n<=sizeof(memory)?memory+a:NULL; }
void ac1_unsupported_instruction(uint32_t va,const char *s) { snprintf(error,sizeof(error),"%08x %s",va,s);longjmp(trap,1); }
static void original(CPU *c) { unsigned v=rd32(100)+1;guest_execution_write(100,&v,4);c->eax=7;c->esp+=4;++effects; }
static void operation(CPU *c) { wr32(100,rd32(100)+1);wr16(103,0x1234);wrf32(200,1.25);push32(c,c->edi);c->esp+=8;c->eax=7; }
static void oracle(CPU *c) { operation(c);if(mode==1)ac1_native_region_forbidden(c,0xdead0000); }
static void native(CPU *c,const ac1_region_plan *p) {
    (void)p;operation(c);
    if(mode==2)wr8(900,1);
    if(mode==3)c->xmm[7].u32[3]=1;
    if(mode==4)ac1_native_region_forbidden(c,0xbeef0000);
}
static ac1_native_region binding={.va=0x402000,.name="journal_fixture",.body=native};
ac1_native_region *const ac1_native_regions[]={&binding};const size_t ac1_native_region_count=1;
ac1_native_binding *const ac1_native_bindings[]={NULL};const size_t ac1_native_binding_count=0;
static void init(void) { _putenv_s("AC1_NATIVE_REGIONS","00402000");_putenv_s("AC1_NATIVE_REGION_VERIFY","100");ac1_native_init(); }
int main(void) {
    CPU c={0};c.esp=8000;c.edi=0xabcdef;init();
    for(unsigned i=0;i<100;++i)ac1_native_journal_invoke(&c,&binding,original,oracle);
    assert(binding.checked==100 && effects==0);
    init();mode=1;uint32_t before=rd32(100);CPU saved=c;
    ac1_native_journal_invoke(&c,&binding,original,oracle);
    assert(binding.fallbacks==1 && effects==1 && rd32(100)==before+1 && c.esp==saved.esp+4);
    init();mode=2;
    if(!setjmp(trap)){ac1_native_journal_invoke(&c,&binding,original,oracle);assert(0);}
    assert(strstr(error,"memory[00000384]"));
    init();mode=3;
    if(!setjmp(trap)){ac1_native_journal_invoke(&c,&binding,original,oracle);assert(0);}
    assert(strstr(error,"CPU byte"));
    init();mode=4;saved=c;before=rd32(100);
    ac1_native_journal_invoke(&c,&binding,original,oracle);
    assert(effects==2 && rd32(100)==before+1 && c.esp==saved.esp+4 && binding.fallbacks==1);
    puts("PASS journal: overlapping writes, x87/stack adapters, additional native writes, CPU mismatch, oracle/native decline rollback before single translated fallback");
}
