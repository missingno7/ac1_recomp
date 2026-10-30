"""Private original-code oracle for the palette region (no game artifacts published)."""
import argparse,json,re,subprocess,sys
from pathlib import Path
from native_region_support import guest_image
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from lift_ac1 import emit_region_sources,FP_ADAPTER_HELPERS,function_tu_bucket

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases',type=int,default=10000)
    parser.add_argument('--portable',action='store_true')
    parser.add_argument('--reuse',action='store_true')
    args=parser.parse_args()
    config=json.loads((ROOT/'config/ac1_dx9.json').read_text())
    entry=next(e for e in config['native_overrides'] if e.get('kind')=='region' and int(e['address'],0)==0xae11a0)
    members=sorted(int(m['address'],0) for m in entry['members']); bodies={}
    shards=list((ROOT/'work/lift_initial').glob('initial_[0-9][0-9][0-9][0-9].c'))
    wanted={function_tu_bucket(va,len(shards)) for va in members}
    shards.sort(key=lambda path:(int(path.stem[8:]) not in wanted,path.name))
    for p in shards:
        text=p.read_text()
        for va in set(members)-bodies.keys():
            match=re.search(rf'(?:static )?void (?:L|ac1_translated)_{va:08X}\(CPU \*c\)\n\{{(.*?)\n\}}',text,re.S)
            if match:bodies[va]=f'void L_{va:08X}(CPU *c)\n{{{match[1]}\n}}'
        if len(bodies)==len(members):break
    if set(members)!=bodies.keys():raise RuntimeError('missing original region members')
    out=ROOT/'work/engine_prof/phase2/oracle';out.mkdir(parents=True,exist_ok=True)
    image_path=guest_image(ROOT,out)
    header='#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'+FP_ADAPTER_HELPERS+'\n'
    emit_region_sources({0xae11a0:entry},members,[bodies[v] for v in members],out,header)
    source=header+r'''
#include "ac1_palette.h"
#include <stdio.h>
static unsigned char memory[0x4000000];
uint8_t *ac1_direct_arena=memory;
#undef guest_execution_read
#undef guest_execution_write
void guest_execution_read(uint32_t a,void *o,size_t n){if((uint64_t)a+n>sizeof(memory))abort();memcpy(o,memory+a,n);}
void guest_execution_write(uint32_t a,const void *o,size_t n){if((uint64_t)a+n>sizeof(memory))abort();memcpy(memory+a,o,n);}
void *guest_memory_try_ptr(uint32_t a,size_t n){return a && (uint64_t)a+n<=sizeof(memory)?memory+a:NULL;}
static unsigned current_case,effects;
static uint64_t frame,epoch;
uint64_t ac1_d3d9_present_count(void){return frame;}
uint64_t ac1_d3d9_state_epoch(void){return epoch;}
void ac1_unsupported_instruction(uint32_t a,const char *s){fprintf(stderr,"CASE %u: %08x %s\n",current_case,a,s);exit(2);}
void dispatch(CPU *c,uint32_t target){
 if(target!=0xfffe0100u){ac1_unsupported_instruction(c->eip,"unexpected fixture external target");}
 if(rd32(c->esp+8)!=120u || rd32(c->esp+12)!=0x2300790u)abort();
 ++effects;c->eax=0;c->esp+=20;
}
#undef AC1_CALL
#define AC1_CALL(c,fn,target) fn(c)
'''
    source+='\n'.join(f'void L_{v:08X}(CPU *);' for v in members)+'\n'
    source+='\n\n'.join(bodies[v] for v in members)
    source+=r'''
void ac1_region_oracle_00AE11A0(CPU *);
void ac1_region_continue_00AE11A0(CPU *);
static ac1_native_region binding={.va=0xae11a0,.boundary=0xae1707,.name="palette",.context_size=8192,.prepare=ac1_palette_prepare,.body=ac1_palette_native,.pending=ac1_palette_pending};
ac1_native_region *const ac1_native_regions[]={&binding};
const size_t ac1_native_region_count=1;
ac1_native_binding *const ac1_native_bindings[]={NULL};
const size_t ac1_native_binding_count=0;
static uint32_t seed=0x9e67ae11;
static uint32_t rng(void){seed^=seed<<13;seed^=seed>>17;return seed^=seed<<5;}
static float number(void){if(!(rng()%13)){uint32_t u=(rng()&1)?0x80000000u:0;float f;memcpy(&f,&u,4);return f;}return ((int)(rng()%20001)-10000)/4096.0f;}
int main(int argc,char **argv){
 if(argc!=3)abort();FILE *f=fopen(argv[1],"rb");if(!f)abort();
 size_t bytes=fread(memory+0x400000,1,0x2000000,f);fclose(f);if(bytes<0x1000000)abort();
 unsigned cases=(unsigned)strtoul(argv[2],NULL,10);char count[32];snprintf(count,sizeof(count),"%u",cases*2);
 _putenv_s("AC1_NATIVE_REGIONS","00AE11A0");_putenv_s("AC1_NATIVE_REGION_VERIFY",count);ac1_native_init();
 for(unsigned i=0;i<cases;++i){
  current_case=i;
  CPU c={0};c.esp=0x3004000+(rng()&15);c.ebp=rng();c.eax=rng();c.ecx=0x2010000;c.edx=rng();c.ebx=rng();c.esi=rng();c.edi=rng();
  c.fp_initialized=1;c.fpu_cw=(uint16_t)(0x37f|((rng()%4)<<10));c.fpu_tw=0xffff;c.mxcsr=0x1f80|((rng()%4)<<13)|((rng()%2)<<15);c.fpu_top=(uint8_t)(rng()%8);
  for(unsigned k=0;k<8;++k)for(unsigned j=0;j<4;++j)c.xmm[k].u32[j]=rng();
  for(unsigned k=0;k<0x1410;++k)memory[c.esp-0x1400+k]=(unsigned char)rng();
  unsigned n=i%33,tag=(i/33)%3,g=0x2020000+(rng()%4)*0x104,pose=0x2030000,storage=0x2100000;
  float scale=(i%7==0)?0.0f:(i%7==1)?-1.0f:(i%7==2)?1.00001f:(i%7==3)?2.5f:1.0f;
  wr32(c.esp,0xfefe0000);wr32(c.esp+4,0x2010100);wr32(c.esp+8,0x2010200);wr32(c.esp+12,0x2010300);
  wr8(c.ecx+0x10,(uint8_t)((g-0x2020000)/0x104));wr32(0x2010104,0x2300000);wr32(0x2010300,0x2020000);
  wr32(0x201029c,i%19?pose:0);wrss(0x20102ac,scale);wr32(0x2300000,0x2200000);wr32(0x2200000,0x2200100);wr32(0x2200278,0xfffe0100);
  wr32(g,n);wr32(pose+0x58,tag);wr32(pose+0x60,storage);wr32(pose+0x5c,0x2040000);
  wr32(0x2040034,0x2050000);wr32(0x2040054,0x2060000);wr32(0x2040004,0x2070000);
  for(unsigned k=0;k<4;++k)wrss(pose+4*k,number());
  for(unsigned j=0;j<64;++j){
   wr16(0x2050000+2*j,(uint16_t)j);wr16(0x2060000+2*(j+1),(uint16_t)j);wr16(0x2070000+12*j+6,(uint16_t)(j*32));
   for(unsigned k=0;k<8;++k)wrss(storage+32*j+4*k,number());
  }
  for(unsigned j=0;j<n;++j){
   uint32_t bind=0x2400000+128*j,index=rng()%64;
   wr32(g+4+4*j,(i%5 && rng()%5)?bind:0);wr32(g+0x84+4*j,index);
   if(tag==2)for(unsigned k=0;k<8;++k)wr16(storage+16*index+2*k,(uint16_t)rng());
   for(unsigned k=0;k<16;++k)wrss(bind+4*k,number());
  }
  CPU original=c; frame=i/64; if(!(i%211)) ++epoch;
  for(unsigned repeat=0;repeat<2;++repeat) {
   c=original;unsigned before=effects;
   ac1_native_region_invoke(&c,&binding,L_00AE11A0,ac1_region_oracle_00AE11A0,ac1_region_continue_00AE11A0);
   if(effects!=before+1 || c.esp!=original.esp+16)abort();
  }
 }
 if(binding.checked!=cases*2 || binding.fallbacks){fprintf(stderr,"incomplete checked=%llu/%u fallbacks=%llu\n",(unsigned long long)binding.checked,cases*2,(unsigned long long)binding.fallbacks);return 3;}
 printf("PASS palette region: %u bit-exact CPU/stack/palette/pending cases, one real continuation per case\n",cases*2);
 return 0;
}
'''
    fixture=out/'fixture.c';fixture.write_text(source)
    exe=out/('portable.exe' if args.portable else 'sse2.exe')
    if args.reuse:source=source.replace('_putenv_s("AC1_NATIVE_REGIONS"','_putenv_s("AC1_NATIVE_PALETTE_REUSE","1");_putenv_s("AC1_NATIVE_STATS","1");_putenv_s("AC1_NATIVE_REGIONS"');fixture.write_text(source)
    flags=['-DAC1_NATIVE_PORTABLE=1'] if args.portable else []
    cmd=['C:/msys64/mingw64/bin/gcc.exe','-O2','-g','-std=c11','-DAC1_DIRECT_MEMORY','-DAC1_TRACE_EIP','-ffp-contract=off','-fno-strict-aliasing',
         '-I'+str(ROOT/'src/runtime'),*flags,str(fixture),str(out/'initial_region_00AE11A0.c'),
         str(ROOT/'src/runtime/ac1_native.c'),str(ROOT/'src/runtime/ac1_palette.c'),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    subprocess.run([str(exe),str(image_path),str(args.cases)],check=True)
if __name__=='__main__':main()
