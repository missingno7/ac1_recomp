/* Audited guest leaves. These bodies implement the guest register/stack ABI,
 * including scratch state that an ordinary host C ABI would discard. */
#include "ac1_native.h"
#include <float.h>
#if defined(__SSE2__) && !defined(AC1_NATIVE_PORTABLE)
#include <emmintrin.h>
#define AC1_NATIVE_SSE2 1
#endif

static XMM vector_mul(XMM a, XMM b) {
#ifdef AC1_NATIVE_SSE2
    __m128 x, y; memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    x = _mm_mul_ps(x, y); memcpy(&a, &x, 16);
#else
    for (unsigned i=0; i<4; ++i) a.f32[i] *= b.f32[i];
#endif
    return a;
}
static XMM vector_add(XMM a, XMM b) {
#ifdef AC1_NATIVE_SSE2
    __m128 x, y; memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    x = _mm_add_ps(x, y); memcpy(&a, &x, 16);
#else
    for (unsigned i=0; i<4; ++i) a.f32[i] += b.f32[i];
#endif
    return a;
}
static XMM vector_sub(XMM a, XMM b) {
#ifdef AC1_NATIVE_SSE2
    __m128 x, y; memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    x = _mm_sub_ps(x, y); memcpy(&a, &x, 16);
#else
    for (unsigned i=0; i<4; ++i) a.f32[i] -= b.f32[i];
#endif
    return a;
}
static XMM broadcast(XMM a, unsigned lane) {
    XMM out; for (unsigned i=0; i<4; ++i) out.u32[i] = a.u32[lane]; return out;
}

/* The guest sums (z+x) and (w+y), then combines those pairs. Retain both
 * operand orders in the dead upper lanes as well as the low dot product. */
static XMM dot_pairs(XMM product) {
    XMM high = {{0}}, low = {{0}};
    high.u64[0] = high.u64[1] = product.u64[1];
    low.u64[0] = low.u64[1] = product.u64[0];
    return vector_add(high, low);
}
static XMM dot_finish(XMM pairs) {
    XMM swapped = pairs;
    for (unsigned i=0; i<4; ++i) swapped.u32[i] = pairs.u32[i^1u];
    return vector_add(swapped, pairs);
}
static int ordinary_bits(uint32_t bits) {
    uint32_t magnitude = bits & 0x7fffffffu;
    /* |x| <= 2^20 also keeps quaternion intermediates finite when output
     * aliases scale/unit. Avoid NaN-payload differences from host reordering
     * after overflow; unusual inputs retain the translated implementation. */
    return magnitude <= 0x49800000u && (!magnitude || magnitude >= 0x00800000u);
}
static unsigned extraordinary_lane(uint32_t address) {
    XMM v = rdxm(address);
    for (unsigned i=0; i<4; ++i) if (!ordinary_bits(v.u32[i])) return i+1;
    return 0;
}
static int decline(ac1_native_plan *plan, uint32_t a, const char *reason) {
    if (plan) { plan->decline_address=a; plan->decline_reason=reason; }
    return 0;
}
static int check_vector(ac1_native_plan *plan, uint32_t a) {
    unsigned bad=extraordinary_lane(a);
    if (bad) return decline(plan,a+4*(bad-1),"float is nonfinite, subnormal, or has magnitude above 2^20");
    return 1;
}
static int overlaps(uint32_t a, size_t n, uint32_t b, size_t m) {
    return (uint64_t)a+n > b && (uint64_t)b+m > a;
}

int ac1_native_01176DE0_plan(const CPU *c, ac1_native_plan *plan) {
    if (c->esp < 80 || c->esp > UINT32_MAX-16u) return decline(plan,c->esp,"ESP cannot hold the native stack contract");
    uint32_t base = ((c->esp-4u)&~15u)-48u;
    for (unsigned k=4; k<=12; k+=4) {
        uint32_t a = rd32(c->esp+k);
        if (a > UINT32_MAX-16u) return decline(plan,a,"16-byte vector wraps the guest address space");
        if (overlaps(a,16,base,c->esp-base)) return decline(plan,a,"input vector aliases stack scratch/saved EBP");
        if (!check_vector(plan,a)) return 0;
    }
    if (!check_vector(plan,0x01A44C90u)) return 0;
    if (!ordinary_bits(rd32(0x01680F90u))) return decline(plan,0x01680F90u,"homogeneous constant outside native float contract");
    if (plan) {
        ac1_native_add_range(plan, base, 48);
        ac1_native_add_range(plan, c->esp-4, 4);
    }
    return 1;
}

/* Plane versus AABB classification: 2 outside, 3 intersecting, 1 inside.
 * cdecl args: plane, low corner, high corner. No x87 state or calls. */
void ac1_native_01176DE0(CPU *c) {
    uint32_t sp = c->esp, base = ((sp-4u)&~15u)-48u;
    uint32_t plane_address = rd32(sp+4), low_address = rd32(sp+8), high_address = rd32(sp+12);
    XMM plane = rdxm(plane_address), zero = rdxm(0x01A44C90u);
    XMM low = rdxm(low_address), high = rdxm(high_address);
    XMM near_corner, far_corner, compare, one = {{0}};
    one.u32[0] = rd32(0x01680F90u);
    for (unsigned i=0; i<4; ++i) {
        uint32_t mask = plane.f32[i] < zero.f32[i] ? UINT32_MAX : 0;
        near_corner.u32[i] = (low.u32[i]&~mask) | (high.u32[i]&mask);
        far_corner.u32[i] = (high.u32[i]&~mask) | (low.u32[i]&mask);
    }
    XMM far_original = far_corner;
    near_corner.u32[3] = far_corner.u32[3] = one.u32[0];
    XMM near_dot = dot_finish(dot_pairs(vector_mul(plane, near_corner)));
    XMM far_pairs = dot_pairs(vector_mul(plane, far_corner));
    XMM far_dot = dot_finish(far_pairs);
    XMM distances = {{0}};
    distances.u32[0] = near_dot.u32[0]; distances.u32[1] = far_dot.u32[0];
    for (unsigned i=0; i<4; ++i) compare.u32[i] = zero.f32[i] < distances.f32[i] ? UINT32_MAX : 0;
    wr32(sp-4u, c->ebp);
    wrxm(base, distances); wrxm(base+16, compare); wrxm(base+32, near_dot);
    c->xmm[0] = distances; c->xmm[1] = far_pairs; c->xmm[2] = far_dot;
    c->xmm[3] = compare; c->xmm[4] = near_dot; c->xmm[5] = one; c->xmm[6] = far_original;
    c->ecx = low_address; c->edx = high_address;
    flags_sub(c, compare.u32[compare.u32[0] ? 0 : 1], 0, 4);
    c->eax = compare.u32[0] ? 2u : compare.u32[1] ? 3u : 1u;
    c->esp = sp+4;
#ifdef AC1_TRACE_EIP
    c->eip = compare.u32[0] ? 0x01176EDEu : 0x01176EEFu;
#endif
}

int ac1_native_00653400_plan(const CPU *c, ac1_native_plan *plan) {
    if (c->esp < 64 || c->esp > UINT32_MAX-16u) return decline(plan,c->esp,"ESP cannot hold the native stack contract");
    uint32_t base = ((c->esp-4u)&~15u)-32u;
    for (unsigned k=4; k<=12; k+=4) {
        uint32_t a = rd32(c->esp+k);
        if (a > UINT32_MAX-64u) return decline(plan,a,"64-byte matrix wraps the guest address space");
        if (overlaps(a,64,base,c->esp+16u-base)) return decline(plan,a,"matrix aliases stack scratch/arguments/saved EBP");
        if (k != 4) for (unsigned row=0; row<4; ++row) if (!check_vector(plan,a+row*16)) return 0;
    }
    if (plan) {
        ac1_native_add_range(plan, base, 32);
        ac1_native_add_range(plan, c->esp-4, 4);
        ac1_native_add_range(plan, rd32(c->esp+4), 64);
    }
    return 1;
}

/* cdecl (out, A, B). Each B row combines the four A rows; out may alias
 * either matrix. Keep the guest's Y*row1 + X*row0, then Z, then W order. */
void ac1_native_00653400(CPU *c) {
    uint32_t sp=c->esp, aligned=(sp-4u)&~15u, base=aligned-32u;
    uint32_t out=rd32(sp+4), aa=rd32(sp+8), bb=rd32(sp+12);
    XMM a[4], b[4], partial[4], w[4], z[4], result[4];
    for (unsigned i=0; i<4; ++i) { a[i]=rdxm(aa+16*i); b[i]=rdxm(bb+16*i); }
    for (unsigned i=0; i<4; ++i) {
        XMM y=vector_mul(broadcast(b[i],1),a[1]);
        XMM x=vector_mul(broadcast(b[i],0),a[0]);
        z[i]=vector_mul(broadcast(b[i],2),a[2]);
        partial[i]=vector_add(vector_add(y,x),z[i]);
        w[i]=vector_mul(broadcast(b[i],3),a[3]);
        result[i]=i ? vector_add(w[i],partial[i]) : vector_add(partial[i],w[i]);
    }
    wr32(sp-4,c->ebp); wrxm(base,w[3]); wrxm(base+16,partial[3]);
    for (unsigned i=0; i<4; ++i) wrxm(out+16*i,result[i]);
    c->xmm[0]=result[3]; c->xmm[1]=w[0]; c->xmm[2]=partial[1]; c->xmm[3]=result[1];
    c->xmm[4]=partial[2]; c->xmm[5]=result[2]; c->xmm[6]=result[0]; c->xmm[7]=z[0];
    c->eax=out; c->ecx=bb; c->esp=sp+4;
    flags_sub(c,aligned,32,4);
#ifdef AC1_TRACE_EIP
    c->eip=0x00653547u;
#endif
}

int ac1_native_0042EE70_plan(const CPU *c, ac1_native_plan *plan) {
    if (c->esp < 32 || c->esp > UINT32_MAX-24u) return decline(plan,c->esp,"ESP cannot hold the native stack contract");
    for (unsigned k=4; k<=20; k+=4) {
        uint32_t a=rd32(c->esp+k);
        if (a > UINT32_MAX-16u) return decline(plan,a,"16-byte vector wraps the guest address space");
        if (overlaps(a,16,c->esp-16,40)) return decline(plan,a,"vector aliases the stack argument/saved EBP area");
        if (k != 4 && !check_vector(plan,a)) return 0;
    }
    if (plan) {
        ac1_native_add_range(plan,c->esp-4,4);
        ac1_native_add_range(plan,rd32(c->esp+4),16);
    }
    return 1;
}

/* cdecl (out, quaternion, vector, scale4, unit4). Preserve the intermediate
 * cross-product write: scale4/unit4 may alias out and are read AFTER it. */
void ac1_native_0042EE70(CPU *c) {
    uint32_t sp=c->esp, out=rd32(sp+4), qa=rd32(sp+8), va=rd32(sp+12);
    uint32_t sa=rd32(sp+16), ua=rd32(sp+20);
    XMM q=rdxm(qa), v=rdxm(va), product=vector_mul(v,q);
    XMM shift1={{0}}, shift2={{0}};
    for (unsigned i=0;i<3;++i) shift1.u32[i]=product.u32[i+1];
    for (unsigned i=0;i<2;++i) shift2.u32[i]=product.u32[i+2];
    XMM dot=vector_add(vector_add(shift1,product),shift2);
    XMM qc9, qd2, vc9, vd2;
    static const unsigned c9[4]={1,2,0,3}, d2[4]={2,0,1,3};
    for (unsigned i=0;i<4;++i) {
        qc9.u32[i]=q.u32[c9[i]]; qd2.u32[i]=q.u32[d2[i]];
        vc9.u32[i]=v.u32[c9[i]]; vd2.u32[i]=v.u32[d2[i]];
    }
    XMM cross=vector_sub(vector_mul(qc9,vd2),vector_mul(qd2,vc9));
    XMM qw=broadcast(q,3);
    wr32(sp-4,c->ebp); wrxm(out,cross);
    XMM sum=vector_add(vector_mul(broadcast(dot,0),q),vector_mul(cross,qw));
    XMM unit=rdxm(ua);
    sum=vector_add(sum,vector_mul(vector_sub(vector_mul(qw,qw),unit),v));
    XMM scale=rdxm(sa);
    XMM result=vector_mul(sum,scale);
    wrxm(out,result);
    c->xmm[0]=scale; c->xmm[1]=unit; c->xmm[2]=v; c->xmm[3]=qw; c->xmm[4]=dot; c->xmm[5]=result;
    c->eax=out; c->ecx=sa; c->edx=ua; c->esp=sp+4;
    flags_logicz(c,(sp-4u)&~15u,4);
#ifdef AC1_TRACE_EIP
    c->eip=0x0042EF09u;
#endif
}

static int ordinary_double(uint64_t bits) {
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    return magnitude <= UINT64_C(0x4130000000000000) &&
           (!magnitude || magnitude >= UINT64_C(0x0010000000000000));
}

int ac1_native_00428540_plan(const CPU *c, ac1_native_plan *plan) {
    if (c->esp < 80 || c->esp > UINT32_MAX-16u) return decline(plan,c->esp,"ESP cannot hold the native stack contract");
    uint32_t base = ((c->esp-4u)&~15u)-48u, out = rd32(c->esp+4);
    if (c->ecx > UINT32_MAX-16u) return decline(plan,c->ecx,"quaternion wraps the guest address space");
    if (overlaps(c->ecx,16,c->esp-4,4)) return decline(plan,c->ecx,"quaternion aliases the saved EBP");
    if (!check_vector(plan,c->ecx)) return 0;
    if (out > UINT32_MAX-64u) return decline(plan,out,"64-byte matrix wraps the guest address space");
    /* Scratch, saved EBP, return address and argument are read after the
     * first matrix store, as is the homogeneous 1.0f. */
    if (overlaps(out,64,base,c->esp+8u-base)) return decline(plan,out,"matrix aliases stack scratch/saved EBP/return address");
    if (overlaps(out,64,0x01680F90u,4)) return decline(plan,out,"matrix aliases the homogeneous constant");
    uint64_t two, one;
    two = (uint64_t)rd32(0x01682FA0u) | (uint64_t)rd32(0x01682FA4u) << 32;
    one = (uint64_t)rd32(0x01680F98u) | (uint64_t)rd32(0x01680F9Cu) << 32;
    if (!ordinary_double(two) || !ordinary_double(one)) return decline(plan,0x01680F98u,"double constant outside native contract");
    if (plan) {
        ac1_native_add_range(plan, base, 48);
        ac1_native_add_range(plan, c->esp-4, 4);
        ac1_native_add_range(plan, out, 64);
    }
    return 1;
}

static XMM single_lane(float f) { XMM x={{0}}; x.f32[0]=f; return x; }
static XMM double_lane(double d) { XMM x={{0}}; x.f64[0]=d; return x; }
static XMM widen_low_pair(XMM s) { XMM d; d.f64[0]=s.f32[0]; d.f64[1]=s.f32[1]; return d; }
static XMM narrow_pair(XMM s) { XMM d={{0}}; d.f32[0]=(float)s.f64[0]; d.f32[1]=(float)s.f64[1]; return d; }

/* thiscall (ecx quaternion; out matrix), ret 4. The guest copies the
 * quaternion to two stack slots before every scalar load (18 16-byte stores);
 * here the lanes come from registers and each slot is stored once, with the
 * same final contents. Register lanes, binary64 products, binary32 rounding
 * points and operand order follow the guest instruction by instruction. */
void ac1_native_00428540(CPU *c) {
    uint32_t sp=c->esp, aligned=(sp-4u)&~15u, base=aligned-48u, out=rd32(sp+4);
    XMM q=rdxm(c->ecx), x0=q, x1, x2, x3, x4, x5, x6, x7;
    float s0, s1, s2, s3, m[16];
    x1=double_lane(rdsd(0x01682FA0u));                                         /* 428568 */
    x2=widen_low_pair(single_lane(q.f32[0])); x3=widen_low_pair(single_lane(q.f32[0]));
    x2.f64[0]*=x1.f64[0]; x2.f64[0]*=x3.f64[0]; x2.f32[0]=(float)x2.f64[0];    /* xx */
    s2=x2.f32[0];
    x3=widen_low_pair(single_lane(q.f32[1])); x2=widen_low_pair(single_lane(q.f32[0]));
    x2.f64[0]*=x1.f64[0]; x2.f64[0]*=x3.f64[0];                                /* xy */
    x4=widen_low_pair(single_lane(q.f32[2])); x3=widen_low_pair(single_lane(q.f32[0]));
    x3.f64[0]*=x1.f64[0]; x3.f64[0]*=x4.f64[0];                                /* xz */
    x5=widen_low_pair(single_lane(q.f32[3])); x4=widen_low_pair(single_lane(q.f32[0]));
    x4.f64[0]*=x1.f64[0]; x4.f64[0]*=x5.f64[0];                                /* xw */
    x6=widen_low_pair(single_lane(q.f32[1])); x5=widen_low_pair(single_lane(q.f32[1]));
    x5.f64[0]*=x1.f64[0]; x5.f64[0]*=x6.f64[0];                                /* yy */
    x6=widen_low_pair(single_lane(q.f32[1])); x6.f64[0]*=x1.f64[0];
    x7=widen_low_pair(single_lane(q.f32[2])); x6.f64[0]*=x7.f64[0];
    x6=narrow_pair(x6); s3=x6.f32[0];                                          /* yz */
    x6=widen_low_pair(single_lane(q.f32[1])); x6.f64[0]*=x1.f64[0];
    x7=widen_low_pair(single_lane(q.f32[3])); x6.f64[0]*=x7.f64[0];
    x6=narrow_pair(x6); s1=x6.f32[0];                                          /* yw */
    x6=widen_low_pair(single_lane(q.f32[2])); x6.f64[0]*=x1.f64[0];
    x7=single_lane(q.f32[2]); x0=single_lane(q.f32[2]);                        /* 428695 */
    x7=widen_low_pair(x7); x6.f64[0]*=x7.f64[0];                               /* zz */
    x2=narrow_pair(x2); x3=narrow_pair(x3); x4=narrow_pair(x4);
    x5=narrow_pair(x5); x6=narrow_pair(x6);
    x0=widen_low_pair(x0); x0.f64[0]*=x1.f64[0];
    x1=widen_low_pair(single_lane(q.f32[3])); x0.f64[0]*=x1.f64[0];            /* zw */
    x1=double_lane(rdsd(0x01680F98u));
    x0=narrow_pair(x0); s0=x0.f32[0];
    x7=x1;
    x0.f64[0]=x5.f32[0]; x7.f64[0]-=x0.f64[0];
    x0.f64[0]=x6.f32[0]; x7.f64[0]-=x0.f64[0];
    x0=narrow_pair(x7); x7.f64[0]=s0; m[0]=x0.f32[0];                          /* 1-yy-zz */
    x0.f64[0]=x2.f32[0]; x0.f64[0]-=x7.f64[0]; x0=narrow_pair(x0);
    x2.f64[0]=x2.f32[0]; m[4]=x0.f32[0];                                       /* xy-zw */
    x0.f64[0]=s1; x7.f64[0]=x3.f32[0]; x0.f64[0]+=x7.f64[0];                   /* yw+xz */
    x7.f64[0]=s0; x7.f64[0]+=x2.f64[0]; x2=narrow_pair(x7); m[1]=x2.f32[0];    /* zw+xy */
    x2.f64[0]=s2; x1.f64[0]-=x2.f64[0];
    x2.f64[0]=x6.f32[0]; x6=x1; x6.f64[0]-=x2.f64[0];
    x2=narrow_pair(x6); m[5]=x2.f32[0];                                        /* 1-xx-zz */
    x2=single_lane(s3); x6=widen_low_pair(x2);
    x7.f64[0]=x4.f32[0]; x6.f64[0]-=x7.f64[0]; x6=narrow_pair(x6);
    x3.f64[0]=x3.f32[0]; m[9]=x6.f32[0];                                       /* yz-xw */
    x6.f64[0]=s1; x3.f64[0]-=x6.f64[0]; x3=narrow_pair(x3);
    x2=widen_low_pair(x2); m[2]=x3.f32[0];                                     /* xz-yw */
    x0=narrow_pair(x0);
    x3.f64[0]=x4.f32[0]; x2.f64[0]+=x3.f64[0]; x2=narrow_pair(x2);
    m[8]=x0.f32[0]; m[6]=x2.f32[0];                                            /* yw+xz, yz+xw */
    x0=single_lane(0.0f); x0.u32[0]=0;
    x2.f64[0]=x5.f32[0]; x1.f64[0]-=x2.f64[0];
    m[3]=m[7]=m[11]=m[12]=m[13]=m[14]=0.0f;
    x0.u32[0]=rd32(0x01680F90u);
    x1=narrow_pair(x1); m[10]=x1.f32[0];                                       /* 1-xx-yy */
    memcpy(&m[15],&x0.u32[0],4);
    wr32(sp-4,c->ebp);
    wrss(base,s0); wrss(base+4,s1); wrss(base+8,s2); wrss(base+12,s3);
    wrxm(base+16,q); wrxm(base+32,q);
    for (unsigned i=0;i<4;++i) { XMM row; memcpy(&row,&m[4*i],16); wrxm(out+16*i,row); }
    c->xmm[0]=x0; c->xmm[1]=x1; c->xmm[2]=x2; c->xmm[3]=x3;
    c->xmm[4]=x4; c->xmm[5]=x5; c->xmm[6]=x6; c->xmm[7]=x7;
    c->eax=out; c->esp=sp+8;
    flags_sub(c,aligned,48,4);
#ifdef AC1_TRACE_EIP
    c->eip=0x004287E3u;
#endif
}
