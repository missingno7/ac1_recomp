#ifndef AC1_REGION_JOURNAL_H
#define AC1_REGION_JOURNAL_H
#include "ac1_native.h"
/* Closed generated regions only. Every guest write must use these adapters;
 * imports, scheduling, atomics and unpinned transfers are excluded. A declined
 * transfer unwinds before execution, restores all writes/CPU, then translates. */
void ac1_region_journal_record(uint32_t,size_t);
void ac1_native_journal_invoke(CPU *,ac1_native_region *,void (*)(CPU *),void (*)(CPU *));
extern void (*ac1_region_transfer_handler)(CPU *,uint32_t);
uint64_t ac1_native_region_verify_count(void);
#ifdef AC1_REGION_JOURNAL_ADAPTERS
static inline void ac1_j_wr8(uint32_t a,uint8_t v) { ac1_region_journal_record(a,1);wr8(a,v); }
static inline void ac1_j_wr16(uint32_t a,uint16_t v) { ac1_region_journal_record(a,2);wr16(a,v); }
static inline void ac1_j_wr32(uint32_t a,uint32_t v) { ac1_region_journal_record(a,4);wr32(a,v); }
static inline void ac1_j_wr64(uint32_t a,uint64_t v) { ac1_region_journal_record(a,8);wr64(a,v); }
static inline void ac1_j_wrss(uint32_t a,float v) { ac1_region_journal_record(a,4);wrss(a,v); }
static inline void ac1_j_wrsd(uint32_t a,double v) { ac1_region_journal_record(a,8);wrsd(a,v); }
static inline void ac1_j_wrxm(uint32_t a,XMM v) { ac1_region_journal_record(a,16);wrxm(a,v); }
static inline void ac1_j_push32(CPU *c,uint32_t v) { c->esp-=4;ac1_j_wr32(c->esp,v); }
static inline void ac1_j_wrf32(uint32_t a,double v) { ac1_region_journal_record(a,4);wrf32(a,v); }
static inline void ac1_j_wrf64(uint32_t a,double v) { ac1_region_journal_record(a,8);wrf64(a,v); }
static inline void ac1_j_wrf80(uint32_t a,double v) { ac1_region_journal_record(a,10);wrf80(a,v); }
static inline void ac1_j_wri16(uint32_t a,double v) { ac1_region_journal_record(a,2);wri16(a,v); }
static inline void ac1_j_wri32(uint32_t a,double v) { ac1_region_journal_record(a,4);wri32(a,v); }
static inline void ac1_j_wri64(uint32_t a,double v) { ac1_region_journal_record(a,8);wri64(a,v); }
static inline void ac1_j_env(CPU *c,uint32_t a) { ac1_region_journal_record(a,28);ac1_x87_store_env(c,a); }
static inline void ac1_j_save(CPU *c,uint32_t a) { ac1_region_journal_record(a,108);ac1_x87_save(c,a); }
#define wr8 ac1_j_wr8
#define wr16 ac1_j_wr16
#define wr32 ac1_j_wr32
#define wr64 ac1_j_wr64
#define wrss ac1_j_wrss
#define wrsd ac1_j_wrsd
#define wrxm ac1_j_wrxm
#define push32 ac1_j_push32
#define wrf32 ac1_j_wrf32
#define wrf64 ac1_j_wrf64
#define wrf80 ac1_j_wrf80
#define wri16 ac1_j_wri16
#define wri32 ac1_j_wri32
#define wri64 ac1_j_wri64
#define ac1_x87_store_env ac1_j_env
#define ac1_x87_save ac1_j_save
#endif
#endif
