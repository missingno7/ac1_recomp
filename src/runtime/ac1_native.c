#include "ac1_native.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

/* Generated only for the fingerprint-pinned main image, never the Bink image. */
extern ac1_native_binding *const ac1_native_bindings[];
extern const size_t ac1_native_binding_count;
static uint64_t verify_limit;
static int report_stats;
static int initialized;
static void region_init(void);
static void region_shutdown(void);

/* Checked scalar copies accept at most 16 bytes, while audited write sets
 * can span multiple vectors and adjacent guest mappings. */
static void read_span(uint32_t address, unsigned char *out, size_t size) {
    for (size_t n=0; n<size;) {
        size_t width = size-n < 16 ? size-n : 16;
        guest_execution_read(address+(uint32_t)n, out+n, width); n+=width;
    }
}
static void write_span(uint32_t address, const unsigned char *in, size_t size) {
    for (size_t n=0; n<size;) {
        size_t width = size-n < 16 ? size-n : 16;
        guest_execution_write(address+(uint32_t)n, in+n, width); n+=width;
    }
}

static void failure(uint32_t va, const char *reason) {
    ac1_unsupported_instruction(va, reason);
    abort(); /* Isolated fixtures need not provide the runtime's longjmp. */
}

void ac1_native_init(void) {
    initialized = 0;
    const char *select = getenv("AC1_NATIVE_OVERRIDES");
    const char *verify = getenv("AC1_NATIVE_VERIFY");
    const char *stats = getenv("AC1_NATIVE_STATS");
    verify_limit = 0;
    report_stats = stats && strcmp(stats, "0");
    if (verify && *verify) {
        char *end; errno = 0;
        verify_limit = strtoull(verify, &end, 10);
        if (errno || *end || *verify == '-') failure(0, "AC1_NATIVE_VERIFY must be a nonnegative call count");
    }
    /* Opt-in: no measured FPS win yet, so unset keeps the translated bodies. */
    int all = select && !strcmp(select, "1");
    int none = !select || !strcmp(select, "0");
    for (size_t i = 0; i < ac1_native_binding_count; ++i) {
        ac1_native_binding *b = ac1_native_bindings[i];
        b->enabled = all; b->calls = b->checked = b->fallbacks = 0;
    }
    if (!all && !none) {
        const char *p = select;
        if (!p || !*p) failure(0, "empty AC1_NATIVE_OVERRIDES; use 0, 1, or comma-separated guest VAs");
        while (*p) {
            char *end; errno = 0;
            unsigned long va = strtoul(p, &end, 16);
            if (errno || end == p || va > UINT32_MAX || (*end && *end != ','))
                failure(0, "invalid AC1_NATIVE_OVERRIDES; expected comma-separated hexadecimal guest VAs");
            size_t i;
            for (i = 0; i < ac1_native_binding_count; ++i)
                if (ac1_native_bindings[i]->va == va) { ac1_native_bindings[i]->enabled = 1; break; }
            if (i == ac1_native_binding_count) failure((uint32_t)va, "AC1_NATIVE_OVERRIDES selected an unbound guest VA");
            p = *end ? end+1 : end;
            if (*end && !*p) failure(0, "trailing comma in AC1_NATIVE_OVERRIDES");
        }
    }
    initialized = 1;
    region_init();
}

void ac1_native_shutdown(void) {
    if (!initialized) return;
    initialized = 0;
    region_shutdown();
    if (!report_stats && !verify_limit) return;
    for (size_t i = 0; i < ac1_native_binding_count; ++i) {
        ac1_native_binding *b = ac1_native_bindings[i];
        fprintf(stderr, "[native] va=0x%08" PRIX32 " name=%s enabled=%d calls=%" PRIu64
                " checked=%" PRIu64 " fallbacks=%" PRIu64 "\n",
                b->va, b->name, b->enabled, b->calls, b->checked, b->fallbacks);
    }
}

/* Weak empty registry keeps leaf-only embedders/fixtures source-compatible.
 * The generated main registry supplies the strong definitions. */
extern ac1_native_region *const ac1_native_regions[];
extern const size_t ac1_native_region_count;
static uint64_t region_verify_limit;
void (*ac1_region_transfer_handler)(CPU *,uint32_t);
uint64_t ac1_native_region_verify_count(void) { return region_verify_limit; }

static void region_init(void) {
    const char *select=getenv("AC1_NATIVE_REGIONS");
    const char *verify=getenv("AC1_NATIVE_REGION_VERIFY");
    region_verify_limit=verify_limit;
    if(verify && *verify) {
        char *end; errno=0; region_verify_limit=strtoull(verify,&end,10);
        if(errno || *end || *verify=='-') failure(0,"AC1_NATIVE_REGION_VERIFY must be a nonnegative call count");
    }
    int all=select && !strcmp(select,"1");
    for(size_t i=0;i<ac1_native_region_count;++i) {
        ac1_native_region *b=ac1_native_regions[i];
        b->enabled=all; b->calls=b->checked=b->fallbacks=0;
#if defined(_WIN32) && defined(__x86_64__) && defined(AC1_DIRECT_MEMORY) && defined(AC1_TRACE_EIP) && !defined(AC1_NATIVE_PORTABLE)
        /* Qualified by 20 interleaved gameplay pairs and five clean replays.
         * Other build configurations remain opt-in; explicit 0 disables all. */
        if(!select && b->va==0x00ae11a0u) b->enabled=1;
#endif
    }
    if(!select || !strcmp(select,"0") || all) return;
    if(!*select) failure(0,"empty AC1_NATIVE_REGIONS; use 0, 1 or comma-separated hexadecimal guest VAs");
    for(const char *p=select;*p;) {
        char *end; errno=0; unsigned long va=strtoul(p,&end,16);
        if(errno || end==p || va>UINT32_MAX || (*end && *end!=',')) failure(0,"invalid AC1_NATIVE_REGIONS list");
        size_t i;
        for(i=0;i<ac1_native_region_count;++i) if(ac1_native_regions[i]->va==va) { ac1_native_regions[i]->enabled=1; break; }
        if(i==ac1_native_region_count) failure((uint32_t)va,"AC1_NATIVE_REGIONS selected an unbound guest VA");
        p=*end ? end+1 : end;
        if(*end && !*p) failure(0,"trailing comma in AC1_NATIVE_REGIONS");
    }
}
static void region_shutdown(void) {
    if(!report_stats && !region_verify_limit) return;
    for(size_t i=0;i<ac1_native_region_count;++i) {
        ac1_native_region *b=ac1_native_regions[i];
        fprintf(stderr,"[region] va=%08x name=%s enabled=%d calls=%" PRIu64 " checked=%" PRIu64 " fallbacks=%" PRIu64 "\n",
                b->va,b->name,b->enabled,b->calls,b->checked,b->fallbacks);
    }
}
void ac1_native_region_forbidden(CPU *c,uint32_t target) {
    if(ac1_region_transfer_handler) ac1_region_transfer_handler(c,target);
    char message[160];
    snprintf(message,sizeof(message),"region closed oracle attempted unpinned transfer target=0x%08x caller=0x%08x",target,c->eip);
    failure(target,message);
}
void ac1_native_region_invoke(CPU *c,ac1_native_region *b,void (*translated)(CPU *),
                              void (*oracle)(CPU *),void (*continuation)(CPU *)) {
    if(!b->enabled) { translated(c); return; }
    enum { CONTEXT_CAP=8192, MEMORY_CAP=16*1024*1024 };
    union { max_align_t alignment; unsigned char bytes[CONTEXT_CAP]; } context;
    ac1_region_plan p;
    memset(&p.writes,0,sizeof(p.writes)); p.context=context.bytes; p.context_size=b->context_size;
    if(b->context_size>CONTEXT_CAP) failure(b->va,"region context exceeds 8192-byte bounded storage");
    if(!b->prepare(c,&p)) {
        if(!b->fallbacks++) {
            fprintf(stderr,"[region-fallback] va=%08x %s address=%08x reason=%s\n",b->va,b->name,
                    p.writes.decline_address,p.writes.decline_reason?p.writes.decline_reason:"outside audited contract");
            fflush(stderr);
        }
        translated(c); return;
    }
    ++b->calls;
    if(report_stats && (b->calls==1 || !(b->calls%100000u))) {
        fprintf(stderr,"[region-progress] va=%08x calls=%" PRIu64 " checked=%" PRIu64 " fallbacks=%" PRIu64 "\n",
                b->va,b->calls,b->checked,b->fallbacks); fflush(stderr);
    }
    if(b->checked>=region_verify_limit) { b->body(c,&p); if(continuation) continuation(c); return; }
    size_t size=0;
    for(unsigned i=0;i<p.writes.count;++i) {
        if(p.writes.ranges[i].size>MEMORY_CAP-size) failure(b->va,"region write snapshot exceeds 16 MiB");
        size+=p.writes.ranges[i].size;
    }
    unsigned char *before=malloc(size?size:1), *expected=malloc(size?size:1), *upload=NULL;
    if(!before || !expected) { free(before); free(expected); failure(b->va,"region snapshot allocation failed"); }
    size_t at=0;
    for(unsigned i=0;i<p.writes.count;++i) {
        ac1_native_range r=p.writes.ranges[i]; read_span(r.address,before+at,r.size); at+=r.size;
    }
    CPU reference; memcpy(&reference,c,sizeof(reference));
    oracle(&reference);
    ac1_pending_upload want={0},got={0};
    if(b->pending) {
        b->pending(&reference,&want);
        if(want.bytes>MEMORY_CAP) failure(b->va,"region pending upload exceeds 16 MiB");
        upload=malloc(want.bytes?want.bytes:1);
        if(!upload) failure(b->va,"region pending-upload allocation failed");
        read_span(want.data,upload,want.bytes);
    }
    at=0;
    for(unsigned i=0;i<p.writes.count;++i) {
        ac1_native_range r=p.writes.ranges[i]; read_span(r.address,expected+at,r.size);
        write_span(r.address,before+at,r.size); at+=r.size;
    }
    b->body(c,&p);
    char message[320]={0};
    if(b->pending) {
        b->pending(c,&got);
        if(memcmp(&want,&got,sizeof(want))) snprintf(message,sizeof(message),
            "region %s call=%" PRIu64 " pending upload mismatch kind/device/start/count/data/bytes expected=%u/%08x/%u/%u/%08x/%u actual=%u/%08x/%u/%u/%08x/%u",
            b->name,b->calls,want.kind,want.device,want.first,want.count,want.data,want.bytes,
            got.kind,got.device,got.first,got.count,got.data,got.bytes);
        for(size_t i=0;!message[0] && i<want.bytes;++i) {
            unsigned char v; guest_execution_read(want.data+(uint32_t)i,&v,1);
            if(v!=upload[i]) snprintf(message,sizeof(message),"region %s call=%" PRIu64 " pending upload byte +%zu expected=%02x actual=%02x",b->name,b->calls,i,upload[i],v);
        }
    }
    for(size_t i=0;!message[0] && i<sizeof(CPU);++i) if(((unsigned char *)c)[i]!=((unsigned char *)&reference)[i])
        snprintf(message,sizeof(message),"region %s call=%" PRIu64 " CPU byte +%zu expected=%02x actual=%02x eip=%08x/%08x esp=%08x/%08x",
                 b->name,b->calls,i,((unsigned char *)&reference)[i],((unsigned char *)c)[i],reference.eip,c->eip,reference.esp,c->esp);
    at=0;
    for(unsigned i=0;!message[0] && i<p.writes.count;++i) {
        ac1_native_range r=p.writes.ranges[i]; read_span(r.address,before+at,r.size);
        for(size_t j=0;j<r.size;++j) if(before[at+j]!=expected[at+j]) {
            snprintf(message,sizeof(message),"region %s call=%" PRIu64 " memory[%08x] expected=%02x actual=%02x range=%08x+%zu",
                     b->name,b->calls,r.address+(uint32_t)j,expected[at+j],before[at+j],r.address,r.size); break;
        }
        at+=r.size;
    }
    free(upload); free(expected); free(before);
    if(message[0]) failure(b->va,message);
    ++b->checked;
    if(b->checked==1 || b->checked==region_verify_limit || !(b->checked%10000)) {
        fprintf(stderr,"[region-verify] va=%08x passed=%" PRIu64 "/%" PRIu64 " CPU=%zu memory=%zu pending=%u\n",b->va,b->checked,region_verify_limit,sizeof(CPU),size,want.bytes); fflush(stderr);
    }
    if(continuation) continuation(c);
}

void ac1_native_add_range(ac1_native_plan *p, uint32_t address, size_t size) {
    if (!size) return;
    if ((uint64_t)address + size > UINT64_C(0x100000000))
        failure(address, "native verification range wraps the guest address space");
    /* Merge aliases before snapshotting: restoring one range must not corrupt
     * another range's expected result. */
    uint64_t lo = address, hi = lo+size;
    for (unsigned i = 0; i < p->count;) {
        uint64_t a = p->ranges[i].address, z = a+p->ranges[i].size;
        if (lo <= z && a <= hi) {
            if (a < lo) lo = a;
            if (z > hi) hi = z;
            p->ranges[i] = p->ranges[--p->count];
            i = 0;
        } else ++i;
    }
    if (p->count == AC1_NATIVE_MAX_RANGES) failure(address, "too many native verification write ranges");
    p->ranges[p->count++] = (ac1_native_range){(uint32_t)lo, (size_t)(hi-lo)};
}

void ac1_native_invoke(CPU *c, ac1_native_binding *b, void (*translated)(CPU *)) {
    if (!b->enabled) { translated(c); return; }
    int checking = b->checked < verify_limit;
    ac1_native_plan plan;
    if (checking) memset(&plan, 0, sizeof(plan));
    if (!b->plan(c, checking ? &plan : NULL)) {
        if (!checking && !b->fallbacks) {
            memset(&plan, 0, sizeof(plan));
            (void)b->plan(c, &plan); /* Collect the first rejection's detail. */
        }
        if (!b->fallbacks++) fprintf(stderr, "[native] 0x%08" PRIX32 " %s: %s at 0x%08" PRIX32 "; using translated body\n",
                b->va, b->name, plan.decline_reason ? plan.decline_reason : "input outside native contract",
                plan.decline_address);
        translated(c); return;
    }
    ++b->calls;
    if (!checking) { b->body(c); return; }
    size_t size = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        if (plan.ranges[i].size > 16u*1024u*1024u-size)
            failure(b->va, "native verification write set exceeds 16 MiB");
        size += plan.ranges[i].size;
    }
    unsigned char *before = malloc(size ? size : 1), *expected = malloc(size ? size : 1);
    if (!before || !expected) {
        free(expected); free(before);
        failure(b->va, "native verification snapshot allocation failed");
    }
    size_t offset = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        read_span(plan.ranges[i].address, before+offset, plan.ranges[i].size);
        offset += plan.ranges[i].size;
    }
    CPU reference;
    memcpy(&reference, c, sizeof(reference)); /* Preserve padding bytes too. */
    translated(&reference);
    offset = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        read_span(plan.ranges[i].address, expected+offset, plan.ranges[i].size);
        write_span(plan.ranges[i].address, before+offset, plan.ranges[i].size);
        offset += plan.ranges[i].size;
    }
    b->body(c);
    /* Compare the complete CPU, not just the documented return registers: flags,
     * scratch registers, dead x87 slots, XMM state, EIP and ESP are observable. */
    const unsigned char *actual_cpu = (const unsigned char *)c;
    const unsigned char *expected_cpu = (const unsigned char *)&reference;
    for (size_t i = 0; i < sizeof(CPU); ++i) if (actual_cpu[i] != expected_cpu[i]) {
        char message[256];
        snprintf(message, sizeof(message), "native %s call=%" PRIu64 " CPU byte +%zu expected=%02x actual=%02x (eax=%08x/%08x esp=%08x/%08x)",
                 b->name, b->calls, i, expected_cpu[i], actual_cpu[i], reference.eax, c->eax, reference.esp, c->esp);
        free(expected); free(before);
        failure(b->va, message);
    }
    offset = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        ac1_native_range r = plan.ranges[i];
        read_span(r.address, before+offset, r.size);
        for (size_t k = 0; k < r.size; ++k) if (before[offset+k] != expected[offset+k]) {
            char message[256];
            snprintf(message, sizeof(message), "native %s call=%" PRIu64 " memory[0x%08x] expected=%02x actual=%02x range=0x%08x+%zu",
                     b->name, b->calls, r.address+(uint32_t)k, expected[offset+k], before[offset+k], r.address, r.size);
            free(expected); free(before);
            failure(b->va, message);
        }
        offset += r.size;
    }
    ++b->checked;
    if (b->checked == 1 || b->checked == verify_limit) {
        fprintf(stderr, "[native-verify] 0x%08" PRIX32 " %s passed=%" PRIu64 "/%" PRIu64 " CPU=%zu bytes ranges=%u memory=%zu bytes\n",
                b->va, b->name, b->checked, verify_limit, sizeof(CPU), plan.count, size);
        fflush(stderr);
    }
    free(expected); free(before);
}
