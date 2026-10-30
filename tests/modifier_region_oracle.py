"""Synthetic closed-cone differential fixture; original bytes stay in work/."""
import argparse,subprocess
from pathlib import Path
from native_region_support import guest_image
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "ac1_region_journal.h"
#include <stdio.h>
static unsigned char memory[0x4000000];uint8_t *ac1_direct_arena=memory;
#undef guest_execution_read
#undef guest_execution_write
void guest_execution_read(uint32_t a,void *o,size_t n){if((uint64_t)a+n>sizeof(memory))abort();memcpy(o,memory+a,n);}
void guest_execution_write(uint32_t a,const void *o,size_t n){if((uint64_t)a+n>sizeof(memory))abort();memcpy(memory+a,o,n);}
void *guest_memory_try_ptr(uint32_t a,size_t n){return (uint64_t)a+n<=sizeof(memory)?memory+a:NULL;}
static unsigned current_case;
void ac1_unsupported_instruction(uint32_t a,const char *s){fprintf(stderr,"CASE %u: %08x %s\n",current_case,a,s);exit(2);}
void ac1_modifier_native(CPU *,const ac1_region_plan *);void ac1_region_oracle_004E6820(CPU *);
static ac1_native_region binding={.va=0x4e6820,.name="modifier",.body=ac1_modifier_native};
ac1_native_region *const ac1_native_regions[]={&binding};const size_t ac1_native_region_count=1;
ac1_native_binding *const ac1_native_bindings[]={NULL};const size_t ac1_native_binding_count=0;
static void fallback(CPU *c){(void)c;fprintf(stderr,"CASE %u unexpectedly declined\n",current_case);exit(3);}
static uint32_t seed=0x4e6820;
static uint32_t rng(void){seed^=seed<<13;seed^=seed>>17;return seed^=seed<<5;}
static void identity(uint32_t a){for(unsigned i=0;i<16;++i)wrss(a+4*i,i%5?0.0f:1.0f);}
static void quaternion(uint32_t a){for(unsigned i=0;i<4;++i)wrss(a+4*i,i==3?1.0f:0.0f);}
int main(int argc,char **argv){
 if(argc!=3)abort();FILE *f=fopen(argv[1],"rb");if(!f)abort();size_t n=fread(memory+0x400000,1,0x2000000,f);fclose(f);if(n<0x1000000)abort();
 unsigned cases=(unsigned)strtoul(argv[2],NULL,10);char count[32];snprintf(count,sizeof(count),"%u",cases);
 _putenv_s("AC1_NATIVE_REGIONS","004E6820");_putenv_s("AC1_NATIVE_REGION_VERIFY",count);ac1_native_init();
 const uint32_t s=0x2010000,list=0x2020000,m=0x2021000,dt=0x2030000,handle=0x2031000,entity=0x2032000;
 const uint32_t pa=0x2040000,pb=0x2041000,desc=0x2050000,map=0x2060000,remap=0x2061000,rec=0x2062000,parents=0x2063000;
 wr32(s+8,dt);wr8(s+0x14c,0x11);wr32(s+0x214,list);wr32(s+0x218,1);wr32(list,m);
 wr32(s+0x180,pb);wr32(s+0x184,pa);wr32(s+0x174,desc);wr8(s+0x19c,0);
 wr32(desc+4,rec);wr32(desc+0x34,map);wr32(desc+0x54,remap);wr32(desc+0x44,parents);wr8(desc+0x20,250);
 wr32(handle,entity);wr32(handle+8,0x10000000);wr32(entity+0x9fc,0);wr32(entity+0xa00,0);
 wr32(0x1a3561c,0);wr32(0x208002c,0x2081000);wr32(0x2081000,0x2082000);wr32(0x2082108,2);
 wr32(0x1a44e80,1);wr8(0x168966b,255);wrss(0x192dda8,0.016f);wrss(0x192ddac,0.5f);
 for(unsigned i=0;i<256;++i){wr16(map+2*i,(uint16_t)i);wr16(remap+2*(i+1),(uint16_t)i);wr16(rec+12*i+4,(uint16_t)(i*32));wr16(rec+12*i+6,(uint16_t)(i*32));wr8(parents+5*i,255);wr8(parents+5*i+1,0);}
 wr16(remap+500,250);wr16(rec+12*250+4,0x3000);
 for(unsigned pass=0;pass<2;++pass){uint32_t p=pass?pb:pa,storage=pass?0x2110000:0x2100000;wr32(p+0x58,0);wr32(p+0x5c,desc);wr32(p+0x60,storage);
  quaternion(p+0x20);for(unsigned i=0;i<256;++i){for(unsigned k=0;k<4;++k)wrss(storage+32*i+4*k,k==0?(float)i*0.1f:0.0f);quaternion(storage+32*i+16);wr8(storage+0x3000+i,255);}}
 for(unsigned i=0;i<cases;++i){
  current_case=i;CPU c={0};c.esp=0x3004000+(rng()&15);c.ecx=s;c.ebp=rng();c.ebx=rng();c.esi=rng();c.edi=rng();c.eax=rng();c.edx=rng();c.fs_base=0x2080000;
  wr32(0x1a44e80,(i>>2)&1); /* Exercise both CRT SSE and x87/control-restore paths. */
  c.fp_initialized=1;c.fpu_cw=(uint16_t)(0x37f|((rng()%4)<<10));c.fpu_tw=0xffff;c.mxcsr=0x1f80|((rng()%4)<<13);c.fpu_top=rng()%8;
  for(unsigned k=0;k<8;++k)for(unsigned j=0;j<4;++j)c.xmm[k].u32[j]=rng();
  for(unsigned k=0;k<8192;++k)memory[c.esp-8192+k]=(unsigned char)rng();wr32(c.esp,0xfefe0000);
  memset(memory+m,0,0x200);unsigned type=i%4;wr32(m,type?0x169c6b4:0x1698bb4);wr32(m+8,handle);wr8(m+0xc,1);wr8(m+0x10,1);
  unsigned tag=(i/4)%3;
  for(unsigned pass=0;pass<2;++pass){uint32_t p=pass?pb:pa,storage=pass?0x2110000:0x2100000;wr32(p+0x58,tag);
   for(unsigned j=0;j<(i%4?8u:256u);++j){float angle=((int)(rng()%1001)-500)/1000.0f,sn=sinf(angle*0.5f),cs=cosf(angle*0.5f);
    if(tag==2){for(unsigned k=0;k<8;++k)wr16(storage+16*j+2*k,0);wr16(storage+16*j,(uint16_t)(j*10));wr16(storage+16*j+12,(uint16_t)(int16_t)(sn*32767));wr16(storage+16*j+14,(uint16_t)(int16_t)(cs*32767));}
    else{for(unsigned k=0;k<8;++k)wrss(storage+32*j+4*k,0.0f);wrss(storage+32*j,(float)j*0.01f);wrss(storage+32*j+24,sn);wrss(storage+32*j+28,cs);}
   }
  }
  if(!type){quaternion(m+0x20);wrss(m+0x30,0.0f);wrss(m+0x34,0.0f);wrss(m+0x38,1.0f);wrss(m+0x3c,0.0f);wrss(m+0x40,0.5f);wrss(m+0x44,0.1f);wr32(m+0x48,(i>>4)&1);wr32(m+0x50,rng()%3);wr32(m+0x54,rng()%3);wr32(m+0x58,handle);wr8(m+0x5c,2);wr32(m+0x60,0x2022000);wr32(m+0x64,(i>>3)&1);wrss(0x2022044,-0.5f);wrss(0x2022048,0.5f);wrss(0x202204c,0.1f);wrss(0x2022050,0.1f);identity(m+0x70);identity(m+0xb0);wr32(m+0xf0,(i&4)?i:(i?i-1:0));wrss(m+0xf4,((int)(rng()%101)-50)/1000.0f);wr32(0x1a1e7bc,i+1);}
  else{quaternion(m+0x20);quaternion(m+0x30);quaternion(m+0x40);quaternion(m+0x50);wrss(m+0x60,0.5f);wrss(m+0x64,0.5f);wr32(m+0x68,1);wr32(m+0x6c,1);wr32(m+0x70,handle);wr8(m+0x74,2);wr32(m+0x78,handle);wr8(m+0x7c,3);wr8(m+0x80,(uint8_t)type);}
  uint32_t sp=c.esp;ac1_native_journal_invoke(&c,&binding,fallback,ac1_region_oracle_004E6820);if(c.esp!=sp+4)abort();
 }
 if(binding.checked!=cases || binding.fallbacks)abort();printf("PASS modifier: %u complete CPU/write-journal cases across Hinge and Compress variants\n",cases);return 0;
}
'''
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cases',type=int,default=10000)
    p.add_argument('--region-object',type=Path);p.add_argument('--portable',action='store_true');a=p.parse_args()
    out=ROOT/'work/engine_prof/phase2/modifier-oracle';out.mkdir(parents=True,exist_ok=True)
    fixture=out/'fixture.c';fixture.write_text(SOURCE);exe=out/'fixture.exe'
    image_path=guest_image(ROOT,out)
    flags=['-O2','-g','-std=c11','-DAC1_DIRECT_MEMORY','-DAC1_TRACE_EIP','-ffp-contract=off','-fno-strict-aliasing','-I'+str(ROOT/'src/runtime')]
    if a.portable:flags+=['-DAC1_NATIVE_PORTABLE']
    obj=a.region_object
    if obj is None:
        obj=out/'region.o'
        subprocess.run(['C:/msys64/mingw64/bin/gcc.exe',*flags,'-c',str(ROOT/'work/lift_initial/initial_region_004E6820.c'),'-o',str(obj)],check=True)
    subprocess.run(['C:/msys64/mingw64/bin/gcc.exe',*flags,str(fixture),str(obj),str(ROOT/'src/runtime/ac1_region_journal.c'),str(ROOT/'src/runtime/ac1_native.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe),str(image_path),str(a.cases)],check=True)
if __name__=='__main__':main()
