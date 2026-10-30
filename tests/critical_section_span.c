/* Real import adapter, including a deterministic replay of the captured VA.
 * Compiled in isolation with function sections; no translated game code. */
#include "../src/runtime/ac1_runtime.c"
#include <assert.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static const uint32_t section_address = 0x2574fff0u;

static int call_section(const char *name, uint32_t address)
{
    CPU cpu = {0};
    import_record import = {0};
    strcpy(import.dll, "KERNEL32.dll");
    strcpy(import.name, name);
    import.token = 0xf100020cu;
    cpu.esp = 0x10000u;
    cpu.eip = 0x7a568eu;
    wr32(cpu.esp, 0x7a5694u);
    wr32(cpu.esp + 4u, address);
    wr32(cpu.esp + 8u, 0x1000u);
    runtime_stop_ready = 1;
    if (setjmp(stop_runtime)) { runtime_stop_ready = 0; return 0; }
    CHECK(handle_critical_section_import(&cpu, &import));
    runtime_stop_ready = 0;
    CHECK(cpu.eip == 0x7a5694u);
    CHECK(cpu.esp == 0x10000u + (!strcmp(name, "InitializeCriticalSectionAndSpinCount") ? 12u : 8u));
    return 1;
}

static void span_contract(int direct, int expect_rejection)
{
    guest_address_space *space = direct ? guest_address_space_create_direct() : guest_address_space_create();
    CHECK(space);
    guest_address_space_select(space);
    CHECK(guest_memory_allocate(0x10000u, 0x1000u));
    /* Reverse insertion exercises the sorted-region transform. */
    CHECK(guest_memory_allocate(0x25750000u, 0x10000u));
    CHECK(guest_memory_allocate(0x25740000u, 0x10000u));
    CHECK(!guest_memory_try_ptr(section_address, 24u));
    for (unsigned i = 0; i < 24u; i += 4u) wr32(section_address + i, 0xa5a5a5a5u);
    int initialized = call_section("InitializeCriticalSectionAndSpinCount", section_address);
    if (expect_rejection) {
        CHECK(!initialized);
        CHECK(strstr(stop_reason, "critical-section object is unmapped"));
        printf("before-fix rejection reproduced: direct=%d address=0x%08x; both regions live\n", direct, section_address);
    } else {
        CHECK(initialized);
        CHECK(rd32(section_address + 4u) == UINT32_MAX);
        CHECK(rd32(section_address + 20u) == 0x1000u);
        CHECK(call_section("EnterCriticalSection", section_address));
        CHECK(call_section("TryEnterCriticalSection", section_address));
        CHECK(rd32(section_address + 8u) == 2u);
        CHECK(call_section("LeaveCriticalSection", section_address));
        CHECK(call_section("LeaveCriticalSection", section_address));
        CHECK(rd32(section_address + 4u) == UINT32_MAX);
        CHECK(rd32(section_address + 12u) == 0u);
        CHECK(call_section("DeleteCriticalSection", section_address));
        CHECK(rd32(section_address + 20u) == 0u);
        CHECK(call_section("InitializeCriticalSection", section_address));
        CHECK(call_section("DeleteCriticalSection", section_address));

        /* A freed tail must still stop every API, without partial writes. */
        guest_memory_release(0x25750000u);
        for (unsigned i = 0; i < 16u; i += 4u) wr32(section_address + i, 0xdeadbeefu);
        const char *names[] = {"InitializeCriticalSection", "InitializeCriticalSectionAndSpinCount",
            "EnterCriticalSection", "TryEnterCriticalSection", "LeaveCriticalSection", "DeleteCriticalSection"};
        for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
            CHECK(!call_section(names[i], section_address));
            CHECK(strstr(stop_reason, "critical-section object is unmapped"));
            CHECK(!call_section(names[i], 0xfffffff0u));
        }
        for (unsigned i = 0; i < 16u; i += 4u) CHECK(rd32(section_address + i) == 0xdeadbeefu);
        printf("CS span lifecycle/hole/wrap passed: direct=%d\n", direct);
    }
    guest_memory_release(0x25740000u);
    guest_memory_release(0x25750000u);
    guest_memory_release(0x10000u);
    guest_address_space_destroy(space);
}

int main(int argc, char **argv)
{
    int before = argc == 2 && !strcmp(argv[1], "--expect-rejection");
    /* Native Windows accepts a CS across two independent reservations. */
    uint8_t *base = VirtualAlloc(NULL, 0x20000u, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(base);
    CHECK(VirtualFree(base, 0, MEM_RELEASE));
    CHECK(VirtualAlloc(base, 0x10000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == base);
    CHECK(VirtualAlloc(base + 0x10000u, 0x10000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == base + 0x10000u);
    CRITICAL_SECTION *native = (CRITICAL_SECTION *)(base + 0xfff0u);
    CHECK(InitializeCriticalSectionAndSpinCount(native, 0x1000u));
    EnterCriticalSection(native);
    CHECK(TryEnterCriticalSection(native));
    CHECK(native->RecursionCount == 2);
    LeaveCriticalSection(native); LeaveCriticalSection(native);
    DeleteCriticalSection(native);
    CHECK(VirtualFree(base, 0, MEM_RELEASE));
    CHECK(VirtualFree(base + 0x10000u, 0, MEM_RELEASE));
    puts("native Windows boundary-spanning CS oracle passed");
    span_contract(0, before);
    span_contract(1, before);
    printf("CS span contract: %u assertions passed\n", checks);
    return 0;
}
