#include "ac1_native.h"
#include <setjmp.h>
#include <stdio.h>
#include <assert.h>

static unsigned char memory[4096];
static jmp_buf trap;
static char diagnostic[512];
static unsigned translated_calls, native_calls;
void guest_execution_read(uint32_t a, void *o, size_t n) { assert(a+n <= sizeof(memory)); memcpy(o, memory+a, n); }
void guest_execution_write(uint32_t a, const void *v, size_t n) { assert(a+n <= sizeof(memory)); memcpy(memory+a, v, n); }
void ac1_unsupported_instruction(uint32_t va, const char *s) {
    snprintf(diagnostic, sizeof(diagnostic), "%08x: %s", va, s); longjmp(trap, 1);
}
static int plan(const CPU *c, ac1_native_plan *p) {
    (void)c;
    /* Alias/merge/adjacency and disjoint stack writes. */
    ac1_native_add_range(p, 100, 8); ac1_native_add_range(p, 104, 12);
    ac1_native_add_range(p, 116, 4); ac1_native_add_range(p, 300, 4);
    assert(p->count == 2);
    return 1;
}
static void operation(CPU *c) {
    uint32_t v = rd32(100); wr32(104, v+1); wr32(300, c->edi);
    c->eax = v; c->esp += 4; c->zf = 1; c->xmm[7].u32[3] = 0x12345678;
}
static void translated(CPU *c) { ++translated_calls; operation(c); }
static void native(CPU *c) { ++native_calls; operation(c); }
static void bad_cpu(CPU *c) { operation(c); c->esp += 4; }
static void bad_memory(CPU *c) { operation(c); wr8(107, 0xaa); }
static int decline(const CPU *c, ac1_native_plan *p) { (void)c; (void)p; return 0; }
static ac1_native_binding binding = {.va=0x401000, .name="fixture", .body=native, .plan=plan};
ac1_native_binding *const ac1_native_bindings[] = {&binding};
const size_t ac1_native_binding_count = 1;
static void setting(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
int main(void) {
    CPU c = {0}; c.esp = 1000; c.edi = 0x99887766;
    wr32(100, 0x42);
    setting("AC1_NATIVE_OVERRIDES", "1"); setting("AC1_NATIVE_VERIFY", "3"); ac1_native_init();
    for (unsigned i = 0; i < 5; ++i) ac1_native_invoke(&c, &binding, translated);
    assert(binding.checked == 3 && translated_calls == 3 && native_calls == 5 && c.esp == 1020);
    assert(rd32(104) == 0x43 && rd32(300) == c.edi);
    setting("AC1_NATIVE_OVERRIDES", "0"); ac1_native_init();
    ac1_native_invoke(&c, &binding, translated); assert(native_calls == 5 && translated_calls == 4);
    setting("AC1_NATIVE_OVERRIDES", "0x00401000"); ac1_native_init(); assert(binding.enabled);
    binding.plan = decline; ac1_native_invoke(&c, &binding, translated); assert(binding.fallbacks == 1);
    binding.plan = plan; binding.body = bad_cpu;
    if (!setjmp(trap)) { ac1_native_invoke(&c, &binding, translated); assert(!"missing CPU mismatch"); }
    assert(strstr(diagnostic, "CPU byte") && strstr(diagnostic, "esp="));
    binding.body = bad_memory;
    if (!setjmp(trap)) { ac1_native_invoke(&c, &binding, translated); assert(!"missing memory mismatch"); }
    assert(strstr(diagnostic, "memory[0x0000006b]"));
    setting("AC1_NATIVE_OVERRIDES", "deadbeef");
    if (!setjmp(trap)) { ac1_native_init(); assert(!"missing unknown VA diagnostic"); }
    assert(strstr(diagnostic, "deadbeef") && strstr(diagnostic, "unbound"));
    puts("native contract: rollback, aliases, limits, switches, CPU/memory mismatch diagnostics passed");
    return 0;
}
