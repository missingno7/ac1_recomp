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
    int all = !select || !strcmp(select, "1");
    int none = select && !strcmp(select, "0");
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
}

void ac1_native_shutdown(void) {
    if (!initialized) return;
    initialized = 0;
    if (!report_stats && !verify_limit) return;
    for (size_t i = 0; i < ac1_native_binding_count; ++i) {
        ac1_native_binding *b = ac1_native_bindings[i];
        fprintf(stderr, "[native] va=0x%08" PRIX32 " name=%s enabled=%d calls=%" PRIu64
                " checked=%" PRIu64 " fallbacks=%" PRIu64 "\n",
                b->va, b->name, b->enabled, b->calls, b->checked, b->fallbacks);
    }
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
    ac1_native_plan plan = {0};
    if (!b->plan(c, &plan)) {
        if (!b->fallbacks++) fprintf(stderr, "[native] 0x%08" PRIX32 " %s: input outside native contract; using translated body\n", b->va, b->name);
        translated(c); return;
    }
    ++b->calls;
    if (b->checked >= verify_limit) { b->body(c); return; }
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
        guest_execution_read(plan.ranges[i].address, before+offset, plan.ranges[i].size);
        offset += plan.ranges[i].size;
    }
    CPU reference;
    memcpy(&reference, c, sizeof(reference)); /* Preserve padding bytes too. */
    translated(&reference);
    offset = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        guest_execution_read(plan.ranges[i].address, expected+offset, plan.ranges[i].size);
        guest_execution_write(plan.ranges[i].address, before+offset, plan.ranges[i].size);
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
        guest_execution_read(r.address, before+offset, r.size);
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
