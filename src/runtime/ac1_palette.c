/* Bounded palette region: native data/math path plus a generated, pinned
 * architectural residual. Only the final decode/product/transpose is replayed
 * to retain exact CPU lanes and the guest-stack scratch visible at the barrier.
 * The real D3D call belongs exclusively to the shared continuation. */
#include "ac1_palette.h"
#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stddef.h>
#include "../graphics/ac1_d3d9_bridge.h"
#if defined(__SSE2__) && !defined(AC1_NATIVE_PORTABLE)
#include <emmintrin.h>
#define PALETTE_SSE 1
#elif defined(__aarch64__) && !defined(AC1_NATIVE_PORTABLE)
#include <arm_neon.h>
#define PALETTE_NEON 1
#endif
enum { PALETTE_BONES=32 };
typedef struct { XMM row[4]; } matrix;
typedef struct {
    uint32_t bind_address, transform_address;
    XMM quaternion, position;
    matrix bind;
} bone_input;
typedef struct {
    uint32_t entry_sp, stack, device, group, pose, count, tag, last;
    uint32_t scaled;
    float scale, one;
    double two, double_one;
    bone_input bone[PALETTE_BONES];
} palette_input;
_Static_assert(sizeof(palette_input)<=8192,"palette region context cap");
extern void ac1_region_residual_00AE11A0(CPU *, const ac1_region_plan *);

/* No pose version is assumed. Compare EVERY resolved numerical input, including
 * all bind matrices and all decoded transforms. Identity only selects a bucket;
 * a hash collision cannot produce a hit. Never cache CPU state or D3D effects.
 * Guest execution holds the runtime baton throughout prepare/body/residual. */
typedef struct {
    uint64_t frame, epoch;
    palette_input input;
    matrix decoded[PALETTE_BONES], result[PALETTE_BONES];
    int valid;
} palette_cache_entry;
static palette_cache_entry *palette_cache;
static uint64_t cache_hits,cache_misses;
static int cache_enabled=-1;
static int cache_report;
static void cache_progress(void) {
    if(cache_report && !((cache_hits+cache_misses)%100000u)) {
        fprintf(stderr,"[palette-reuse-progress] hits=%" PRIu64 " misses=%" PRIu64 "\n",cache_hits,cache_misses); fflush(stderr);
    }
}
static void cache_stats(void) {
    const char *stats=getenv("AC1_NATIVE_STATS");
    if(stats && strcmp(stats,"0")) fprintf(stderr,"[palette-reuse] hits=%" PRIu64 " misses=%" PRIu64 " entries=1024 bytes=%zu\n",
        cache_hits,cache_misses,1024*sizeof(palette_cache_entry));
    free(palette_cache); palette_cache=NULL;
}
static palette_cache_entry *cache_lookup(const palette_input *p,int *hit) {
    if(cache_enabled<0) {
        const char *value=getenv("AC1_NATIVE_PALETTE_REUSE");
        cache_enabled=value && !strcmp(value,"1");
        const char *stats=getenv("AC1_NATIVE_STATS"); cache_report=stats && strcmp(stats,"0");
        if(cache_enabled) atexit(cache_stats);
    }
    *hit=0;
    if(!cache_enabled) return NULL;
    if(!palette_cache) {
        palette_cache=calloc(1024,sizeof(*palette_cache));
        if(!palette_cache) { cache_enabled=0; fprintf(stderr,"[palette-reuse] allocation failed; recomputing\n"); return NULL; }
    }
    uint64_t frame=ac1_d3d9_present_count(),epoch=ac1_d3d9_state_epoch();
    uint32_t scale_bits;memcpy(&scale_bits,&p->scale,4);
    uint32_t hash=(p->pose>>4)^((p->group>>2)*2654435761u)^scale_bits^(p->count*2246822519u)^p->tag;
    hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
    uint32_t bucket=hash&255u;
    palette_cache_entry *base=palette_cache+4*bucket,*replace=base;
    size_t offset=offsetof(palette_input,group);
    for(unsigned i=0;i<4;++i) {
        palette_cache_entry *e=base+i;
        if(!e->valid || e->frame!=frame || e->epoch!=epoch) replace=e;
        if(e->valid && e->frame==frame && e->epoch==epoch &&
           !memcmp((const unsigned char *)&e->input+offset,(const unsigned char *)p+offset,sizeof(*p)-offset)) {
            ++cache_hits; cache_progress(); *hit=1; return e;
        }
    }
    ++cache_misses; cache_progress(); replace->valid=0; replace->frame=frame; replace->epoch=epoch; replace->input=*p;
    return replace;
}

static int reject(ac1_region_plan *p,uint32_t a,const char *reason) {
    p->writes.decline_address=a; p->writes.decline_reason=reason; return 0;
}
static int span(uint32_t a,size_t size,uint32_t lo,size_t n) {
    return a && (uint64_t)a+size<=UINT64_C(0x100000000) &&
           !((uint64_t)a+size>lo && (uint64_t)lo+n>a) && guest_memory_try_ptr(a,size)!=NULL;
}
static int ordinary(float f) {
    uint32_t u; memcpy(&u,&f,4); u&=0x7fffffffu;
    return u<=0x49800000u && (!u || u>=0x00800000u);
}
static XMM mul(XMM a,XMM b) {
#if PALETTE_SSE
    __m128 x,y; memcpy(&x,&a,16); memcpy(&y,&b,16); x=_mm_mul_ps(x,y); memcpy(&a,&x,16);
#elif PALETTE_NEON
    float32x4_t x=vld1q_f32(a.f32),y=vld1q_f32(b.f32); vst1q_f32(a.f32,vmulq_f32(x,y));
#else
    for(unsigned i=0;i<4;++i) a.f32[i]*=b.f32[i];
#endif
    return a;
}
static XMM add(XMM a,XMM b) {
#if PALETTE_SSE
    __m128 x,y; memcpy(&x,&a,16); memcpy(&y,&b,16); x=_mm_add_ps(x,y); memcpy(&a,&x,16);
#elif PALETTE_NEON
    vst1q_f32(a.f32,vaddq_f32(vld1q_f32(a.f32),vld1q_f32(b.f32)));
#else
    for(unsigned i=0;i<4;++i) a.f32[i]+=b.f32[i];
#endif
    return a;
}
static XMM splat(float f) { XMM v; for(unsigned i=0;i<4;++i) v.f32[i]=f; return v; }
/* Original order is (Y*A1 + X*A0) + Z*A2, then W*A3 (opposite add operand
 * order for rows 1..3). No FMA or algebraic reassociation is permitted. */
static matrix product(matrix a,matrix b) {
    matrix o;
    for(unsigned i=0;i<4;++i) {
        XMM v=add(mul(splat(b.row[i].f32[1]),a.row[1]),mul(splat(b.row[i].f32[0]),a.row[0]));
        v=add(v,mul(splat(b.row[i].f32[2]),a.row[2]));
        XMM w=mul(splat(b.row[i].f32[3]),a.row[3]);
        o.row[i]=i?add(w,v):add(v,w);
    }
    return o;
}
static matrix rotation(const palette_input *p,XMM q) {
    double x=q.f32[0],y=q.f32[1],z=q.f32[2],w=q.f32[3],t=p->two,one=p->double_one;
    /* Match each guest binary64 multiply -> binary32 round, followed by the
     * original binary64 sums/subtractions -> binary32 round. */
    float xx=(float)((x*t)*x),xy=(float)((x*t)*y),xz=(float)((x*t)*z),xw=(float)((x*t)*w);
    float yy=(float)((y*t)*y),yz=(float)((y*t)*z),yw=(float)((y*t)*w);
    float zz=(float)((z*t)*z),zw=(float)((z*t)*w);
    matrix m={0};
    m.row[0].f32[0]=(float)((one-(double)yy)-(double)zz);
    m.row[0].f32[1]=(float)((double)zw+(double)xy);
    m.row[0].f32[2]=(float)((double)xz-(double)yw);
    m.row[1].f32[0]=(float)((double)xy-(double)zw);
    m.row[1].f32[1]=(float)((one-(double)xx)-(double)zz);
    m.row[1].f32[2]=(float)((double)yz+(double)xw);
    m.row[2].f32[0]=(float)((double)yw+(double)xz);
    m.row[2].f32[1]=(float)((double)yz-(double)xw);
    m.row[2].f32[2]=(float)((one-(double)xx)-(double)yy);
    m.row[3].f32[3]=p->one;
    return m;
}
static void store_matrix(uint32_t a,const matrix *m) {
    for(unsigned i=0;i<4;++i) wrxm(a+16*i,m->row[i]);
}
static void store_transpose(uint32_t a,matrix m) {
#if PALETTE_SSE
    __m128 x,y,z,w; memcpy(&x,&m.row[0],16); memcpy(&y,&m.row[1],16);
    memcpy(&z,&m.row[2],16); memcpy(&w,&m.row[3],16); _MM_TRANSPOSE4_PS(x,y,z,w);
    memcpy(&m.row[0],&x,16); memcpy(&m.row[1],&y,16); memcpy(&m.row[2],&z,16); memcpy(&m.row[3],&w,16);
#elif PALETTE_NEON
    float32x4x2_t a01=vtrnq_f32(vld1q_f32(m.row[0].f32),vld1q_f32(m.row[1].f32));
    float32x4x2_t a23=vtrnq_f32(vld1q_f32(m.row[2].f32),vld1q_f32(m.row[3].f32));
    vst1q_f32(m.row[0].f32,vcombine_f32(vget_low_f32(a01.val[0]),vget_low_f32(a23.val[0])));
    vst1q_f32(m.row[1].f32,vcombine_f32(vget_low_f32(a01.val[1]),vget_low_f32(a23.val[1])));
    vst1q_f32(m.row[2].f32,vcombine_f32(vget_high_f32(a01.val[0]),vget_high_f32(a23.val[0])));
    vst1q_f32(m.row[3].f32,vcombine_f32(vget_high_f32(a01.val[1]),vget_high_f32(a23.val[1])));
#else
    for(unsigned i=0;i<4;++i) for(unsigned j=i+1;j<4;++j) {
        uint32_t tmp=m.row[i].u32[j]; m.row[i].u32[j]=m.row[j].u32[i]; m.row[j].u32[i]=tmp;
    }
#endif
    store_matrix(a,&m);
}
static int input_span(ac1_region_plan *plan,const palette_input *p,uint32_t a,size_t n) {
    if(!span(a,n,p->entry_sp-0x1400u,0x1410u) ||
       ((uint64_t)a+n>p->device+0x790u && (uint64_t)p->device+0x790u+p->count*64u>a) ||
       ((uint64_t)a+n>p->device+0x2a70u && (uint64_t)p->device+0x2a78u>a))
        return reject(plan,a,"input is unmapped, wraps, or aliases region writes");
    return 1;
}
int ac1_palette_prepare(const CPU *c,ac1_region_plan *plan) {
    palette_input *p=plan->context;
    memset(p,0,sizeof(*p)); /* Canonical padding/unused entries for exact input comparison. */
    if(c->esp<0x1400u || c->esp>UINT32_MAX-16u) return reject(plan,c->esp,"palette stack wraps");
    if(c->df) return reject(plan,c->esp,"palette DF is set");
    if(c->mmx_active) return reject(plan,c->esp,"palette x87 entry while MMX active");
    /* The translated floating stores/conversions use C casts, and therefore
     * the same host arithmetic as these kernels, for every guest CW/MXCSR
     * rounding field. Do not reject the game's truncation control state.
     * The architectural control fields are retained by the residual. */
    p->entry_sp=c->esp; p->stack=((c->esp-4u)&~15u)-0x10f0u;
    p->count=0; p->device=0; p->pose=0; p->tag=0; p->last=UINT32_MAX;
    if(!span(c->esp-0x1400u,0x1410u,0,0) || !span(c->ecx,0x14,0,0)) return reject(plan,c->esp,"palette stack/record unmapped");
    uint32_t context=rd32(c->esp+4),data=rd32(c->esp+8),groups=rd32(c->esp+12);
    if(!span(context,8,0,0) || !span(data,0xb0,0,0) || !span(groups,4,0,0)) return reject(plan,context,"palette argument header unmapped");
    p->device=rd32(context+4);
    uint64_t group=(uint64_t)rd32(groups)+(uint32_t)rd8(c->ecx+0x10)*0x104u;
    if(group>UINT32_MAX || p->device>UINT32_MAX-0x2a78u)
        return reject(plan,p->device,"group/device address wraps");
    p->group=(uint32_t)group;
    if(!span(p->group,0x104,0,0) || !span(p->device+0x2a70u,8,0,0)) return reject(plan,p->group,"palette group/device fields unmapped");
    p->count=rd32(p->group);
    if(p->count>PALETTE_BONES) return reject(plan,p->group,"bone group exceeds 32-bone guest frame");
    if(!span(p->device+0x790u,p->count*64u,0,0)) return reject(plan,p->device,"palette destination unmapped");
    if(!span(p->device+0x790u,p->count*64u,c->esp-0x1400u,0x1410u) ||
       !span(p->device+0x2a70u,8,c->esp-0x1400u,0x1410u))
        return reject(plan,p->device,"device writes overlap guest stack");
    if(!input_span(plan,p,context,8) || !input_span(plan,p,data,0xb0) ||
       !input_span(plan,p,groups,4) || !input_span(plan,p,p->group,0x104) || !input_span(plan,p,c->ecx,0x14)) return 0;
    p->pose=rd32(data+0x9c); p->scale=rdss(data+0xac);
    p->one=rdss(0x01680f90u); p->double_one=rdsd(0x01680f98u); p->two=rdsd(0x01682fa0u);
    if(!ordinary(p->scale) || p->one!=1.0f || p->double_one!=1.0 || p->two!=2.0)
        return reject(plan,data+0xac,"scale/constants outside exact palette arithmetic contract");
    const uint32_t expected_masks[4]={0,0,0,0xffffffffu};
    for(unsigned i=0;i<4;++i) if(rd32(0x01682ec0u+i*4)!=expected_masks[i]) return reject(plan,0x01682ec0u,"translation mask changed");
    p->scaled=fabsf((float)((double)p->scale-p->double_one))>rdss(0x01683308u);
    if(p->pose) {
        if(!input_span(plan,p,p->pose,0x64)) return 0;
        p->tag=rd32(p->pose+0x58);
        if(p->tag>2) return reject(plan,p->pose+0x58,"unsupported pose representation tag");
    }
    for(unsigned i=0;i<p->count;++i) {
        bone_input *b=&p->bone[i]; b->bind_address=rd32(p->group+4+4*i); b->transform_address=0;
        if(!b->bind_address || !p->pose) continue;
        p->last=i;
        if(!input_span(plan,p,b->bind_address,64)) return 0;
        for(unsigned row=0;row<4;++row) {
            b->bind.row[row]=rdxm(b->bind_address+16*row);
            for(unsigned lane=0;lane<4;++lane) if(!ordinary(b->bind.row[row].f32[lane])) return reject(plan,b->bind_address,"nonordinary bind matrix lane");
        }
        uint32_t index=rd32(p->group+0x84+4*i),storage=rd32(p->pose+0x60),offset;
        if(p->tag==1) {
            uint32_t table=rd32(p->pose+0x5c);
            if(!input_span(plan,p,table,0x58)) return 0;
            uint32_t map=rd32(table+0x34),remap=rd32(table+0x54),records=rd32(table+4);
            if((uint64_t)map+2ull*index>UINT32_MAX) return reject(plan,map,"indexed map address wraps");
            if(!input_span(plan,p,map+2*index,2)) return 0;
            uint32_t id=((uint32_t)rd16(map+2*index)+1u)&0xffffu;
            if((uint64_t)remap+2*id>UINT32_MAX) return reject(plan,remap,"indexed remap address wraps");
            if(!input_span(plan,p,remap+2*id,2)) return 0;
            id=rd16(remap+2*id);
            if((uint64_t)records+12*id+6>UINT32_MAX) return reject(plan,records,"indexed record address wraps");
            if(!input_span(plan,p,records+12*id+6,2)) return 0;
            offset=rd16(records+12*id+6);
        } else {
            if(index>UINT32_MAX/(p->tag==2?16u:32u)) return reject(plan,index,"transform index overflows");
            offset=index*(p->tag==2?16u:32u);
        }
        if((uint64_t)storage+offset>UINT32_MAX) return reject(plan,storage,"transform address wraps");
        b->transform_address=storage+offset;
        if(!input_span(plan,p,b->transform_address,p->tag==2?16:32)) return 0;
        if(p->tag==2) {
            double factor=rdsd(0x01688858u); float step=rdss(0x01688860u);
            if(!isfinite(factor) || !ordinary(step)) return reject(plan,0x01688858u,"packed quantization constants invalid");
            for(unsigned k=0;k<4;++k) b->quaternion.f32[k]=(float)((double)(int16_t)rd16(b->transform_address+8+2*k)*factor);
            XMM position={{0}},scale={{0}};
            for(unsigned k=0;k<3;++k) { position.f32[k]=(float)(int16_t)rd16(b->transform_address+2*k); scale.f32[k]=step; }
            b->position=add(mul(position,scale),rdxm(p->pose));
        } else {
            b->quaternion=rdxm(b->transform_address+16); b->position=rdxm(b->transform_address);
            if(p->tag==1) b->position=add(b->position,rdxm(p->pose));
        }
        b->position.f32[3]=p->one;
        for(unsigned k=0;k<4;++k) if(!ordinary(b->quaternion.f32[k]) || !ordinary(b->position.f32[k]))
            return reject(plan,b->transform_address,"nonordinary decoded quaternion/position");
    }
    ac1_native_add_range(&plan->writes,c->esp-0x1400u,0x1410u);
    ac1_native_add_range(&plan->writes,p->device+0x790u,p->count*64u);
    ac1_native_add_range(&plan->writes,p->device+0x2a70u,8);
    return 1;
}
void ac1_palette_native(CPU *c,const ac1_region_plan *plan) {
    const palette_input *p=plan->context;
    int hit=0; palette_cache_entry *cache=cache_lookup(p,&hit);
    matrix scale={0}; for(unsigned i=0;i<3;++i) scale.row[i].f32[i]=p->scale; scale.row[3].f32[3]=p->one;
    for(unsigned i=0;i<p->count;++i) {
        /* The residual necessarily computes this bone to reconstruct the
         * architectural scratch. Do not compute it twice, including N=1. */
        if(i==p->last) continue;
        const bone_input *b=&p->bone[i]; matrix result=hit?cache->result[i]:scale;
        if(p->pose) {
            if(b->bind_address) {
                matrix pose;
                if(hit) pose=cache->decoded[i];
                else { pose=rotation(p,b->quaternion); pose.row[3]=b->position; }
                store_matrix(p->stack+0x8f0+64*i,&pose);
                if(!hit) {
                    matrix bind=p->scaled?product(scale,b->bind):b->bind;
                    result=product(pose,bind);
                    if(cache) cache->decoded[i]=pose;
                }
            }
            store_matrix(p->stack+0xf0+64*i,&result);
        }
        store_transpose(p->device+0x790+64*i,result);
        if(cache && !hit) cache->result[i]=result;
    }
    ac1_region_residual_00AE11A0(c,plan);
    if(p->last!=UINT32_MAX) {
        matrix result;
        for(unsigned row=0;row<4;++row) result.row[row]=rdxm(p->stack+0xf0+64*p->last+16*row);
        if(p->last+1!=p->count) store_transpose(p->device+0x790+64*p->last,result);
        if(cache && !hit) {
            cache->result[p->last]=result;
            for(unsigned row=0;row<4;++row) cache->decoded[p->last].row[row]=rdxm(p->stack+0x8f0+64*p->last+16*row);
        }
    }
    if(cache) cache->valid=1;
}
int ac1_palette_should_replay(const CPU *c,const ac1_region_plan *plan,uint32_t site) {
    const palette_input *p=plan->context;
    switch(site) {
    case 0x00ae12bf: case 0x00ae14ae: case 0x00ae1340: case 0x00ae159f: return c->esi==p->last*64u;
    case 0x00ae13ad: case 0x00ae152b: return c->edi==p->last*64u;
    case 0x00ae1442: case 0x00ae1638: case 0x00ae1655: return c->esi==p->last;
    default: ac1_native_region_forbidden((CPU *)c,site); return 0;
    }
}
void ac1_palette_residual_hook(CPU *c,const ac1_region_plan *plan,uint32_t site) {
    const palette_input *p=plan->context;
    if(!p->count) return;
    uint32_t last=p->count-1;
    if(site==0x00ae1690 && !c->edx) { c->edx=last; c->eax+=64*last; c->ecx+=64*last; }
    else if(site==0x00ae16f0 && !c->eax) { c->eax=last; c->edx+=64*last; }
}
void ac1_palette_pending(const CPU *c,ac1_pending_upload *out) {
    uint32_t d=rd32(c->esp+0x1c);
    out->kind=1; out->device=rd32(d); out->first=rd32(d+0x2a70); out->count=rd32(d+0x2a74);
    out->data=d+16*(out->first+1); out->bytes=16*out->count;
}
