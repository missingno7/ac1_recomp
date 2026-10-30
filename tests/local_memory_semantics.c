/* Same operation sequence is compiled with legacy and captured-base helpers. */
#include "cpu.h"
#ifdef LOCAL_VARIANT
#include "ac1_local_memory.h"
#define RUN run_local
#define RUN_FAIL fail_local
#else
#define AC1_DECLARE_LOCAL_ARENA ((void)0)
#define RUN run_legacy
#define RUN_FAIL fail_legacy
#endif

void RUN(CPU *c, uint32_t seed)
{
    AC1_DECLARE_LOCAL_ARENA;
    uint32_t a=0x8ff9u, b=0x8200u;
    wr8(a,(uint8_t)seed); wr16(a+1,(uint16_t)(seed>>3));
    wr32(a+3,seed^0x71234567u); wr64(a+7,((uint64_t)seed<<32)|~seed);
    c->eax=rd8(a)^rd16(a+1)^rd32(a+3); c->edx=(uint32_t)(rd64(a+7)>>32);
    wri16(b,(int16_t)seed); wri32(b+3,(int32_t)seed); wri64(b+9,(int64_t)(int32_t)seed*17777);
    c->esi=(uint32_t)(int32_t)rdi16(b)^(uint32_t)(int32_t)rdi32(b+3); c->edi=(uint32_t)(int64_t)rdi64(b+9);
    double value=(double)(int32_t)seed/1024.0;
    wrf32(b+32,(float)value); wrf64(b+39,value); wrf80(b+49,value);
    c->st[0]=(double)rdf32(b+32); c->st[1]=rdf64(b+39); c->st[2]=rdf80(b+49);
    XMM x; x.u64[0]=((uint64_t)seed<<32)|seed; x.u64[1]=~x.u64[0];
    wrxm(a,x); c->xmm[0]=rdxm(a);
    wrss(b+80,(float)value); c->xmm[1].f32[0]=rdss(b+80);
    wrsd(b+89,-value); c->xmm[2].f64[0]=rdsd(b+89);
    c->esp=0x9100; push32(c,seed); c->ebx=pop32(c);
    wr32(0x8300,seed); c->ecx=guest_atomic_xadd32(0x8300,11);
    c->eax^=guest_atomic_cmpxchg32(0x8300,seed+11,seed^17);
    c->edx^=guest_atomic_xchg32(0x8300,seed+41);
    c->ecx^=bit_string_op(c,0x8400,(int32_t)(seed%127)-63,(int)(seed%4));
    ac1_x87_reset(c);
    c->fpu_top=(int)(seed&7); c->fpu_cw=(uint16_t)(0x37f|((seed&3)<<10));
    c->fpu_sw=seed&0x4700; c->fpu_ip=seed; c->fpu_dp=~seed;
    c->fpu_cs=0x23; c->fpu_ds=0x2b; c->fpu_opcode=(uint16_t)(seed&0x7ff);
    c->fpu_tw=0;
    for(unsigned i=0;i<8;++i){c->st[i]=(double)(int32_t)(seed+i)/32.;c->mmx[i]=((uint64_t)seed<<32)|i;c->mmx_exp[i]=0xffff;}
    c->mmx_physical=(uint8_t)seed; c->x87_dirty=(uint8_t)(seed>>8);
    ac1_x87_store_env(c,0x8ff0); ac1_x87_load_env(c,0x8ff0);
    ac1_x87_save(c,0x8ff0); ac1_x87_restore(c,0x8ff0);
}

void RUN_FAIL(CPU *c, uint32_t address, unsigned operation)
{
    AC1_DECLARE_LOCAL_ARENA;
    if(operation==0)c->eax=guest_atomic_xadd32(address,3);
    else if(operation==1)c->eax=guest_atomic_cmpxchg32(address,3,7);
    else c->eax=guest_atomic_xchg32(address,7);
}
