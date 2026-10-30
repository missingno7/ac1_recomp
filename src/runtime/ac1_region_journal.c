#include "ac1_region_journal.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
enum { BLOCKS=4096, SLOTS=8192 };
typedef struct { uint32_t address; unsigned char before[16],expected[16]; } journal_block;
typedef struct { uint32_t generation,address,index; } journal_slot;
typedef struct {
    journal_block blocks[BLOCKS]; journal_slot slots[SLOTS];
    uint32_t generation,count,decline_target,decline_caller;
    const char *reason;
    CPU initial;
    jmp_buf decline;
} region_journal;
static region_journal *journal;
static int active,stats;
/* Cheap rejection before entering the transactional cone. The journal remains
 * authoritative for late lazy-evaluation/propagation/math-error exits. This
 * preflight only avoids duplicating a known mixed callback batch. */
static int modifier_preflight(const CPU *c,uint32_t *target,const char **reason) {
    uint32_t s=c->ecx;
    if(!guest_memory_try_ptr(s,0xa04)) { *target=s;*reason="unmapped SkeletonComponent";return 0; }
    if(!rd8(0x01a25840u) && !(rd8(s+0x14c)&0x10) && !(rd8(s+0x9f8)&4)) return 1;
    uint32_t mode=rd8(s+0x14c)&7u;
    if(!mode) return 1;
    uint32_t list=rd32(s+0x20c+8*mode),count=rd32(s+0x210+8*mode)&0x3fffu;
    if(count>1024 || (count && !guest_memory_try_ptr(list,count*4))) { *target=list;*reason="modifier list outside bounded contract";return 0; }
    if(count && rd8(s+0x19c)) { *target=0x004e75d0;*reason="pending lazy pose evaluation";return 0; }
    for(unsigned i=0;i<count;++i) {
        uint32_t m=rd32(list+4*i);
        if(!guest_memory_try_ptr(m,0x14)) { *target=m;*reason="unmapped modifier";return 0; }
        if(!(rd8(m+0x10)&1)) continue;
        uint32_t v=rd32(m);
        if(v>UINT32_MAX-0x3c || !guest_memory_try_ptr(v+0x38,4)) { *target=v;*reason="unmapped modifier vtable";return 0; }
        v=rd32(v+0x38);
        if(v!=0x00654090 && v!=0x006c4ff0 && v!=0x00609bf0 && v!=0x00648d80) {
            *target=v;*reason="callback outside closed Hinge/Compress plus translated Roll/LookAt set";return 0;
        }
    }
    return 1;
}
static void release_journal(void) { free(journal);journal=NULL; }
static void decline(CPU *c,uint32_t target) {
    journal->decline_target=target; journal->decline_caller=c?c->eip:0;
    journal->reason="transfer outside pinned region";
    longjmp(journal->decline,1);
}
void ac1_region_journal_record(uint32_t address,size_t size) {
    if(!active) { ac1_unsupported_instruction(address,"region journal write outside active transaction");abort(); }
    if((uint64_t)address+size>UINT64_C(0x100000000)) { journal->reason="journal write wraps";longjmp(journal->decline,1); }
    for(uint64_t a=address&~15u;a<(uint64_t)address+size;a+=16) {
        uint32_t slot=((uint32_t)(a>>4)*2654435761u)&(SLOTS-1);
        while(journal->slots[slot].generation==journal->generation && journal->slots[slot].address!=a) slot=(slot+1)&(SLOTS-1);
        journal_slot *s=&journal->slots[slot];
        if(s->generation==journal->generation) continue;
        if(journal->count==BLOCKS) { journal->reason="journal exceeds bounded 64-KiB write footprint";longjmp(journal->decline,1); }
        const void *memory=guest_memory_try_ptr((uint32_t)a,16);
        if(!memory) { journal->reason="journal write block is not fully mapped";journal->decline_target=(uint32_t)a;longjmp(journal->decline,1); }
        journal_block *b=&journal->blocks[journal->count]; b->address=(uint32_t)a;
        memcpy(b->before,memory,16); memcpy(b->expected,memory,16);
        *s=(journal_slot){journal->generation,(uint32_t)a,journal->count++};
    }
}
static void restore(void) {
    for(unsigned i=0;i<journal->count;++i) guest_execution_write(journal->blocks[i].address,journal->blocks[i].before,16);
}
void ac1_native_journal_invoke(CPU *c,ac1_native_region *b,void (*translated)(CPU *),void (*oracle)(CPU *)) {
    if(!b->enabled) { translated(c);return; }
    if(active || ac1_region_transfer_handler) { ac1_unsupported_instruction(b->va,"nested native journal transaction");abort(); }
    if(b->va==0x004e6820u) {
        uint32_t target=0;const char *reason=NULL;
        if(!modifier_preflight(c,&target,&reason)) {
            if(!b->fallbacks++) { fprintf(stderr,"[region-journal-fallback] va=%08x target=%08x reason=%s\n",b->va,target,reason);fflush(stderr); }
            translated(c);return;
        }
    }
    if(!journal) {
        journal=calloc(1,sizeof(*journal));
        if(!journal) { ++b->fallbacks;translated(c);return; }
        const char *v=getenv("AC1_NATIVE_STATS");stats=v && strcmp(v,"0");atexit(release_journal);
    }
    if(!++journal->generation) { memset(journal->slots,0,sizeof(journal->slots));++journal->generation; }
    journal->count=0;journal->decline_target=journal->decline_caller=0;journal->reason=NULL;
    memcpy(&journal->initial,c,sizeof(*c));active=1;ac1_region_transfer_handler=decline;
    if(setjmp(journal->decline)) {
        restore();memcpy(c,&journal->initial,sizeof(*c));active=0;ac1_region_transfer_handler=NULL;
        if(!b->fallbacks++ || (stats && !(b->fallbacks%10000))) {
            fprintf(stderr,"[region-journal-fallback] va=%08x count=%" PRIu64 " caller=%08x target=%08x reason=%s\n",
                b->va,b->fallbacks,journal->decline_caller,journal->decline_target,journal->reason);fflush(stderr);
        }
        translated(c);return;
    }
    int checking=b->checked<ac1_native_region_verify_count();
    CPU expected;
    if(checking) {
        memcpy(&expected,c,sizeof(expected));oracle(&expected);
        for(unsigned i=0;i<journal->count;++i) {
            journal_block *r=&journal->blocks[i];guest_execution_read(r->address,r->expected,16);
        }
        restore();
    }
    b->body(c,NULL);active=0;ac1_region_transfer_handler=NULL;++b->calls;
    char error[256]={0};
    if(checking) {
        for(size_t i=0;i<sizeof(CPU);++i) if(((unsigned char *)c)[i]!=((unsigned char *)&expected)[i]) {
            snprintf(error,sizeof(error),"journal region %s call=%" PRIu64 " CPU byte +%zu expected=%02x actual=%02x",b->name,b->calls,i,((unsigned char *)&expected)[i],((unsigned char *)c)[i]);break;
        }
        for(unsigned i=0;!error[0] && i<journal->count;++i) {
            journal_block *r=&journal->blocks[i];unsigned char now[16];guest_execution_read(r->address,now,16);
            for(unsigned j=0;j<16;++j) if(now[j]!=r->expected[j]) {
                snprintf(error,sizeof(error),"journal region %s call=%" PRIu64 " memory[%08x] expected=%02x actual=%02x",b->name,b->calls,r->address+j,r->expected[j],now[j]);break;
            }
        }
        if(error[0]) { ac1_unsupported_instruction(b->va,error);abort(); }
        ++b->checked;
        if(b->checked==1 || !(b->checked%1000) || b->checked==ac1_native_region_verify_count()) {
            fprintf(stderr,"[region-journal-verify] va=%08x passed=%" PRIu64 " blocks=%u fallbacks=%" PRIu64 "\n",b->va,b->checked,journal->count,b->fallbacks);fflush(stderr);
        }
    }
    if(stats && (b->calls==1 || !(b->calls%10000))) {
        fprintf(stderr,"[region-journal-progress] va=%08x calls=%" PRIu64 " checked=%" PRIu64 " fallbacks=%" PRIu64 "\n",b->va,b->calls,b->checked,b->fallbacks);fflush(stderr);
    }
}
