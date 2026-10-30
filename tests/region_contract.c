#include "ac1_native.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
static unsigned char memory[4096];
static jmp_buf trap;
static char diagnostic[512];
static unsigned effects,oracles,natives;
void guest_execution_read(uint32_t a,void *o,size_t n) { assert(n<=16 && a+n<=sizeof(memory)); memcpy(o,memory+a,n); }
void guest_execution_write(uint32_t a,const void *o,size_t n) { assert(n<=16 && a+n<=sizeof(memory)); memcpy(memory+a,o,n); }
void ac1_unsupported_instruction(uint32_t a,const char *s) { snprintf(diagnostic,sizeof(diagnostic),"%08x %s",a,s); longjmp(trap,1); }
static int prepare(const CPU *c,ac1_region_plan *p) {
    (void)c;
    ac1_native_add_range(&p->writes,100,32); ac1_native_add_range(&p->writes,120,32);
    ac1_native_add_range(&p->writes,800,64); assert(p->writes.count==2); return 1;
}
static void operation(CPU *c) { wr32(100,rd32(100)+1); wr32(124,c->edi); wr32(800,0x11223344); c->eax=4;c->xmm[7].u32[3]=7; c->eip=0x402010; }
static void oracle(CPU *c) { ++oracles; operation(c); }
static void native(CPU *c,const ac1_region_plan *p) { (void)p; ++natives; operation(c); }
static void pending(const CPU *c,ac1_pending_upload *u) { (void)c; *u=(ac1_pending_upload){1,42,120,4,800,64}; }
static void continuation(CPU *c) { ++effects; c->esp+=16; }
static void original(CPU *c) { oracle(c); continuation(c); }
static void bad_cpu(CPU *c,const ac1_region_plan *p) { native(c,p); c->xmm[7].u32[3]++; }
static void bad_stack(CPU *c,const ac1_region_plan *p) { native(c,p); wr8(129,1); }
static void bad_upload(CPU *c,const ac1_region_plan *p) { native(c,p); wr8(823,1); }
static ac1_native_region binding={.va=0x402000,.boundary=0x402010,.name="region_fixture",.context_size=128,.prepare=prepare,.body=native,.pending=pending};
ac1_native_region *const ac1_native_regions[]={&binding};
const size_t ac1_native_region_count=1;
ac1_native_binding *const ac1_native_bindings[]={NULL};
const size_t ac1_native_binding_count=0;
static void init(void) { _putenv_s("AC1_NATIVE_REGIONS","00402000"); _putenv_s("AC1_NATIVE_REGION_VERIFY","3"); ac1_native_init(); assert(binding.enabled); }
int main(void) {
    CPU c={0}; c.esp=1000;c.edi=0xabcdef12;
    init();
    for(unsigned i=0;i<5;++i) ac1_native_region_invoke(&c,&binding,original,oracle,continuation);
    assert(effects==5 && oracles==3 && natives==5 && rd32(100)==5 && c.esp==1080 && binding.checked==3);
    unsigned e=effects;
    init();binding.body=bad_cpu;
    if(!setjmp(trap)) { ac1_native_region_invoke(&c,&binding,original,oracle,continuation); assert(0); }
    assert(strstr(diagnostic,"CPU byte") && effects==e);
    init();binding.body=bad_stack;
    if(!setjmp(trap)) { ac1_native_region_invoke(&c,&binding,original,oracle,continuation); assert(0); }
    assert(strstr(diagnostic,"memory[00000081]") && effects==e);
    init();binding.body=bad_upload;
    if(!setjmp(trap)) { ac1_native_region_invoke(&c,&binding,original,oracle,continuation); assert(0); }
    assert(strstr(diagnostic,"pending upload byte +23") && effects==e);
    _putenv_s("AC1_NATIVE_REGIONS","0"); ac1_native_init();
    ac1_native_region_invoke(&c,&binding,original,oracle,continuation); assert(effects==e+1);
    _putenv_s("AC1_NATIVE_REGIONS","deadbeef");
    if(!setjmp(trap)) { ac1_native_init(); assert(0); }
    assert(strstr(diagnostic,"unbound"));
    puts("PASS: region rollback/CPU/stack/pending-upload checks; external effect exactly once; default-off and namespace diagnostics");
}
