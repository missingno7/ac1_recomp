/* Native x86-64 MMX oracles for instruction probes, plus hand-computed
 * references for original IA-32 routines whose stack/address ABI cannot be
 * executed as raw bytes in this 64-bit test process. No game runtime links. */
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <limits>
#include <xmmintrin.h>
#if defined(_WIN32)
#include <windows.h>
#endif
extern "C" {
#define _Thread_local thread_local
#include "cpu.h"
#include "ops_declarations.h"
thread_local uint32_t g_image_delta;
}

static std::array<uint8_t,0x10000> memory{};
static std::array<uint8_t,0x1000> globals{};
static uint32_t guard_address, guard_width;
static unsigned diagnostic_count;
static bool expect_diagnostic;
static const char *diagnostic_reason;
static uint32_t diagnostic_va;

static void *mapped(uint32_t address,size_t width) {
    if(address>=0x100000 && uint64_t(address-0x100000)+width<=memory.size()) return memory.data()+address-0x100000;
    if(address>=0x1a19000 && uint64_t(address-0x1a19000)+width<=globals.size()) return globals.data()+address-0x1a19000;
    std::fprintf(stderr,"bad guest access %08x width %zu\n",address,width);std::abort();
}
extern "C" void guest_execution_read(uint32_t address,void *out,size_t width) {
    if(address==guard_address && width>guard_width) std::abort();
    std::memcpy(out,mapped(address,width),width);
}
extern "C" void guest_execution_write(uint32_t address,const void *input,size_t width) {
    if(address==guard_address && width>guard_width) std::abort();
    std::memcpy(mapped(address,width),input,width);
}
extern "C" void dispatch(CPU *,uint32_t address) { std::fprintf(stderr,"unexpected dispatch %08x\n",address);std::abort(); }
extern "C" void dispatch_jmp(CPU *c,uint32_t address) { dispatch(c,address); }
extern "C" void ac1_unsupported_instruction(uint32_t va,const char *reason) {
    if(!expect_diagnostic) { std::fprintf(stderr,"unexpected diagnostic %08x: %s\n",va,reason);std::abort(); }
    ++diagnostic_count;diagnostic_reason=reason;diagnostic_va=va;
}
static uint32_t random_state=0x54934a90u;
static uint32_t next32() { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static uint64_t next64() { return uint64_t(next32()) | (uint64_t(next32())<<32); }
static unsigned oracle_mxcsr=0x1f80u,oracle_flags;
struct Operator { const char *name;uint64_t (*native)(uint64_t,uint64_t);void (*translated)(CPU *);unsigned result; };
#include "ops_native.inc"

#define CHECK(expr) do { if(!(expr)) { std::fprintf(stderr,"check failed line %d: %s\n",__LINE__,#expr);return false; } } while(0)
static bool instructions() {
    const uint64_t edges[]={0,UINT64_MAX,0x7f807f807f807f80ull,0x7fff80007fff8000ull,
        0x7fffffff80000000ull,0x8000800080008000ull,0x0001000100010001ull,0x00000001ffffffffull,
        0x7fc000007f800000ull,0xff800000cf000000ull,0x3fc00000bfc00000ull,0x3f000000bf000000ull};
    uint64_t cases=0;
    for(const auto &op:operators) {
        bool convert=std::strncmp(op.name,"cvt",3)==0;
        unsigned modes=convert?8:1;
        for(unsigned mode=0;mode<modes;++mode) for(unsigned i=0;i<3000;++i) {
            uint64_t a=i<sizeof(edges)/sizeof(edges[0])?edges[i]:next64();
            uint64_t b=i<sizeof(edges)/sizeof(edges[0])?edges[(i+3)%(sizeof(edges)/sizeof(edges[0]))]:next64();
            if(i<130 && (!std::strncmp(op.name,"psll",4)||!std::strncmp(op.name,"psrl",4)||!std::strncmp(op.name,"psra",4))) b=i;
            oracle_mxcsr=0x1f80u|((mode&3u)<<13)|(mode>=4?0x40u:0u);
            uint64_t expected=op.native(a,b);
            CPU cpu{};ac1_fp_init(&cpu);ac1_mmx_write(&cpu,0,a);ac1_mmx_write(&cpu,1,b);
            cpu.xmm[0].u64[0]=b;cpu.xmm[0].u64[1]=0x12345678abcdef00ull;
            cpu.eax=(uint32_t)b;cpu.mxcsr=oracle_mxcsr;
            cpu.cf=cpu.pf=cpu.af=cpu.zf=cpu.sf=cpu.of=cpu.df=1;cpu.eflags_misc=0x202;
            op.translated(&cpu);
            uint64_t actual=op.result==0?cpu.mmx[0]:op.result==1?cpu.eax:cpu.xmm[0].u64[0];
            if(actual!=expected || (convert && (cpu.mxcsr&63u)!=oracle_flags)) {
                std::fprintf(stderr,"%s i=%u mode=%u a=%016llx b=%016llx actual=%016llx expected=%016llx flags=%x/%x\n",
                    op.name,i,mode,(unsigned long long)a,(unsigned long long)b,(unsigned long long)actual,
                    (unsigned long long)expected,cpu.mxcsr&63u,oracle_flags);return false;
            }
            CHECK(cpu.fpu_top==0 && cpu.fpu_tw==0 && cpu.mmx_active);
            CHECK(cpu.cf && cpu.pf && cpu.af && cpu.zf && cpu.sf && cpu.of && cpu.df && cpu.eflags_misc==0x202);
            if(!std::strcmp(op.name,"cvtpi2ps")) CHECK(cpu.xmm[0].u64[1]==0x12345678abcdef00ull);
            if(!std::strcmp(op.name,"movq2dq")) CHECK(cpu.xmm[0].u64[1]==0);
            ++cases;
        }
    }
    std::printf("native MMX instruction oracle: %llu randomized/edge cases, %zu forms, all MXCSR rounding modes\n",
        (unsigned long long)cases,sizeof(operators)/sizeof(operators[0]));
    return true;
}

static bool memory_widths() {
    CPU c{};c.eax=0x108000;c.edi=c.eax;guard_address=c.eax;guard_width=8;
    wr64(c.eax,0xaabbccdd12345678ull);
    guard_width=4;probe_mem_movd_read(&c);CHECK(c.mmx[0]==0x12345678ull);
    ac1_mmx_write(&c,0,0xffffaaaa87654321ull);probe_mem_movd_write(&c);
    CHECK(rd32(c.eax)==0x87654321u);guard_width=8;CHECK(rd64(c.eax)==0xaabbccdd87654321ull);
    probe_mem_movq_read(&c);CHECK(c.mmx[0]==0xaabbccdd87654321ull);
    ac1_mmx_write(&c,0,0x0123456789abcdefull);probe_mem_movq_write(&c);CHECK(rd64(c.eax)==c.mmx[0]);
    probe_mem_movntq(&c);CHECK(rd64(c.eax)==c.mmx[0]);
    guard_width=2;probe_mem_pinsrw(&c);CHECK(c.mmx[0]==0x01234567cdefcdefull);
    guard_width=8;ac1_mmx_write(&c,0,0x0123456789abcdefull);ac1_mmx_write(&c,1,0x0080008000800080ull);
    wr64(c.edi,0x5555555555555555ull);probe_maskmovq(&c);CHECK(rd64(c.edi)==0x5523556755ab55efull);
    guard_address=guard_width=0;
    std::puts("MMX memory widths, MOVD zero extension, MASKMOVQ and MOVNTQ passed");return true;
}

static bool pushad() {
    for(unsigned n=0;n<2000;++n) {
        CPU c{};c.eax=next32();c.ecx=next32();c.edx=next32();c.ebx=next32();
        c.esp=0x101000;c.ebp=next32();c.esi=next32();c.edi=next32();c.cf=c.df=1;
        uint32_t expected[]={c.edi,c.esi,c.ebp,c.esp,c.ebx,c.edx,c.ecx,c.eax};
        probe_pushal(&c);CHECK(c.esp==0x100fe0);
        for(unsigned i=0;i<8;++i) CHECK(rd32(c.esp+4*i)==expected[i]);
        wr32(c.esp+12,0xdeadbeef);c.eax=c.ebx=c.ecx=c.edx=c.esi=c.edi=c.ebp=0;
        probe_popal(&c);CHECK(c.esp==0x101000 && c.eax==expected[7] && c.ecx==expected[6] && c.edx==expected[5]);
        CHECK(c.ebx==expected[4] && c.ebp==expected[2] && c.esi==expected[1] && c.edi==expected[0] && c.cf && c.df);
    }
    std::puts("PUSHAD/POPAD: 2000 randomized round trips and original-ESP/skip semantics passed");return true;
}

static bool environments() {
    const uint32_t addr=0x108000;
    CHECK(ac1_fp_round(-0.5,0)==0.0 && std::signbit(ac1_fp_round(-0.5,0)));
    CHECK(ac1_fp_round(1.5,0)==2.0 && ac1_fp_round(2.5,0)==2.0);
    CPU c{};ac1_x87_reset(&c);fpush(&c,1.5);fpush(&c,-0.0);fpush(&c,-2.25);
    c.fpu_cw=0x0320;c.fpu_sw=0x4500;c.fpu_ip=0x12345678;c.fpu_dp=0x23456789;
    c.fpu_cs=0x23;c.fpu_ds=0x2b;c.fpu_opcode=0x5a5;c.edi=addr;
    std::memset(mapped(addr,128),0x5a,128);probe_fnstenv(&c);
    CHECK(rd16(addr)==0x0320 && c.fpu_cw==0x033f);
    CHECK(rd16(addr+4)==(0x4500u|(5u<<11)) && rd16(addr+8)==0x13ffu);
    CHECK(rd32(addr+12)==0x12345678 && rd16(addr+16)==0x23 && rd16(addr+18)==0x5a5);
    CHECK(rd32(addr+20)==0x23456789 && rd16(addr+24)==0x2b && rd8(addr+28)==0x5a);
    c.fpu_top=0;c.fpu_cw=0;c.fpu_tw=0;probe_fldenv(&c);
    CHECK(c.fpu_top==5 && c.fpu_cw==0x0320 && c.fpu_tw==0x13ffu && c.st[5]==-2.25);
    probe_fnsave(&c);CHECK(c.fpu_top==0 && c.fpu_tw==0xffff && c.fpu_cw==0x037f && c.fpu_sw==0);
    CHECK(rd64(addr+28)==0x9000000000000000ull && rd16(addr+36)==0xc000);
    CHECK(rd64(addr+38)==0 && rd16(addr+46)==0x8000);
    CHECK(rd64(addr+48)==0xc000000000000000ull && rd16(addr+56)==0x3fff);
    CHECK(rd8(addr+108)==0x5a);
    std::array<uint8_t,108> first{};std::memcpy(first.data(),mapped(addr,108),108);
    c.st[5]=99;probe_frstor(&c);CHECK(c.fpu_top==5 && c.st[5]==-2.25 && std::signbit(c.st[6]) && c.st[7]==1.5);
    probe_fnsave(&c);CHECK(!std::memcmp(first.data(),mapped(addr,108),108));

    // Compare the defined environment fields and nonempty logical ST payloads
    // against actual hardware FSAVE (reserved bytes and host selectors differ).
    alignas(16) unsigned char hardware[108]{};double a=1.5,b=-2.25;
    __asm__ volatile("fninit\n\tfldl %1\n\tfldl %2\n\tfnsave %0\n\tfninit"
                     : "=m"(hardware):"m"(a),"m"(b):"memory");
    ac1_x87_reset(&c);fpush(&c,a);fpush(&c,b);probe_fnsave(&c);
    CHECK(!std::memcmp(hardware,mapped(addr,2),2));
    CHECK(!std::memcmp(hardware+4,mapped(addr+4,2),2));
    CHECK(!std::memcmp(hardware+8,mapped(addr+8,2),2));
    CHECK(!std::memcmp(hardware+28,mapped(addr+28,20),20));
    const double values[]={0.0,-0.0,1.0,-1.0,std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
    for(double value:values) {
        __asm__ volatile("fninit\n\tfldl %1\n\tfnsave %0\n\tfninit"
                         : "=m"(hardware):"m"(value):"memory");
        ac1_x87_reset(&c);fpush(&c,value);probe_fnsave(&c);
        CHECK(!std::memcmp(hardware+8,mapped(addr+8,2),2));
        CHECK(!std::memcmp(hardware+28,mapped(addr+28,10),10));
        probe_frstor(&c);CHECK(c.st[c.fpu_top]==value && bool(std::signbit(c.st[c.fpu_top]))==bool(std::signbit(value)));
    }
    CPU partial{};partial.edi=addr;ac1_mmx_write(&partial,0,123);
    probe_fnsave(&partial);probe_frstor(&partial);CHECK(partial.mmx_active && partial.mmx[0]==123);
    for(unsigned i=0;i<8;++i) ac1_mmx_write(&c,i,next64());
    uint64_t saved[8];std::memcpy(saved,c.mmx,sizeof saved);
    CHECK(c.fpu_tw==0 && c.fpu_top==0);probe_fnsave(&c);CHECK(rd16(addr+8)==0xaaaau);
    probe_frstor(&c);CHECK(c.mmx_active && !std::memcmp(saved,c.mmx,sizeof saved));
    probe_emms(&c);CHECK(!c.mmx_active && c.fpu_tw==0xffff);
    probe_fnsave(&c);probe_frstor(&c);CHECK(!c.mmx_active && !std::memcmp(saved,c.mmx,sizeof saved));
    c.esp=0x101000;ac1_mmx_write(&c,0,1);expect_diagnostic=true;diagnostic_count=0;
    L_00600000(&c);expect_diagnostic=false;
    CHECK(diagnostic_count==1 && diagnostic_va==0x600000 && std::strstr(diagnostic_reason,"missing EMMS"));
    CHECK(c.esp==0x101000 && c.mmx[0]==1);
    probe_emms(&c);wr32(c.esp,0x12345678);L_00600000(&c);CHECK(c.esp==0x101004 && c.st[c.fpu_top]==1.0);
    c.fpu_top=5;probe_emms(&c);CHECK(c.fpu_top==5); // EMMS does not change TOP
    std::puts("x87 28/108-byte environments: native FSAVE, ext80, tags/TOP, MMX round trips and mixed-use diagnostic passed");
    return true;
}

static int64_t floor_div(int64_t value,int64_t divisor) {
    return value>=0?value/divisor:-((-value+divisor-1)/divisor);
}
static bool audio() {
    const unsigned lengths[]={0,4,8,16,28,32,36,60,64,68,96,128,252,256,508,512};
    const int16_t coefficients[]={0,1,-1,0x3fff,0x4000,0x7fff,(int16_t)0x8000};
    unsigned cases=0;
    for(unsigned iteration=0;iteration<700;++iteration) for(unsigned length:lengths) {
        int16_t left=iteration<7?coefficients[iteration]:(int16_t)next32();
        int16_t right=iteration<7?coefficients[6-iteration]:(int16_t)next32();
        uint32_t src=0x104000,dst=0x106000,stack=0x101000;
        std::memset(mapped(dst,640),0x5a,640);
        for(unsigned i=0;i<length/2;++i) wr16(src+2*i,(uint16_t)next32());
        CPU c{};c.esp=stack;c.esi=0x11111111;c.edi=0x22222222;c.ebp=0x33333333;c.ebx=0x44444444;c.fpu_top=5;
        uint32_t frame[]={0x12345678,src,dst,length,(uint32_t)(int32_t)left,(uint32_t)(int32_t)right};
        std::memcpy(mapped(stack,sizeof frame),frame,sizeof frame);L_00834A90(&c);
        for(unsigned i=0;i<length/2;++i) {
            int32_t product=(int16_t)rd16(src+2*i)*(int32_t)(i&1?right:left);
            uint16_t expected;
            if(length<32) {
                int32_t hi=(int32_t)floor_div(int64_t(product)*int32_t(0x80020009u),INT64_C(1)<<32);
                int32_t sum=(int32_t)((uint32_t)hi+(uint32_t)product);
                int32_t q=(int32_t)floor_div(sum,8192);
                expected=(uint16_t)(q+(q<0?1:0));
            } else expected=(uint16_t)((uint32_t)(int32_t)floor_div(product,65536)<<2);
            CHECK(rd16(dst+2*i)==expected);
        }
        CHECK(rd8(dst+length)==0x5a && c.esp==stack+4 && c.esi==0x11111111 && c.edi==0x22222222);
        CHECK(c.ebp==0x33333333 && c.ebx==0x44444444 && !c.mmx_active);
        ++cases;
    }
    std::printf("original lifted audio 00834A90: %u randomized scalar/MMX/tail/ABI cases against integer reference\n",cases);
    return true;
}

static bool bink() {
    unsigned cases=0;
    for(unsigned iteration=0;iteration<60;++iteration) for(unsigned length=0;length<=512;length+=2) {
        uint32_t src=0x104000,dst=0x106000,stack=0x101000;
        for(unsigned i=0;i<length;++i) wr8(src+i,(uint8_t)next32());
        std::memset(mapped(dst,640),0x5a,640);
        CPU c{};c.esp=stack;c.esi=0x11111111;c.edi=0x22222222;c.ebp=0x33333333;
        uint32_t frame[]={0x12345678,0,dst,src,src+length};std::memcpy(mapped(stack,sizeof frame),frame,sizeof frame);
        L_1801E500(&c);
        CHECK(!std::memcmp(mapped(src,length),mapped(dst,length),length));
        CHECK(rd8(dst+length)==0x5a && c.eax==dst+length && c.esp==stack+4);
        CHECK(c.esi==0x11111111 && c.edi==0x22222222 && c.ebp==0x33333333);
        CHECK(bool(c.mmx_active)==(length>=8));
        if(length>=8) { uint64_t last=c.mmx[0];CHECK(ac1_mmx_read(&c,0)==last);probe_emms(&c);CHECK(c.mmx[0]==last && c.fpu_tw==0xffff); }
        ++cases;
    }
    std::printf("original lifted Bink 1801E500: %u randomized vector/tail/ABI/cross-call-state cases against byte-copy reference\n",cases);
    return true;
}

int main() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#endif
    std::signal(SIGABRT, [](int){ std::_Exit(3); });
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(!instructions() || !memory_widths() || !pushad() || !environments() || !audio() || !bink()) return 1;
    std::puts("general MMX/state oracle passed");return 0;
}
