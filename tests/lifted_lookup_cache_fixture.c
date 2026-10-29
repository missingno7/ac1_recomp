#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stddef.h>
typedef struct CPU { uint32_t eax; } CPU;
typedef void (*ac1_lifted_fn)(CPU *);
#include "ac1_lookup_cache.h"

static void stub_0(CPU *cpu) { cpu->eax = 0u; }
static void stub_1(CPU *cpu) { cpu->eax = 1u; }
static void stub_2(CPU *cpu) { cpu->eax = 2u; }
static void stub_3(CPU *cpu) { cpu->eax = 3u; }
static void stub_4(CPU *cpu) { cpu->eax = 4u; }
static void stub_5(CPU *cpu) { cpu->eax = 5u; }
static void stub_6(CPU *cpu) { cpu->eax = 6u; }
static void stub_7(CPU *cpu) { cpu->eax = 7u; }
static void stub_8(CPU *cpu) { cpu->eax = 8u; }
static void stub_9(CPU *cpu) { cpu->eax = 9u; }
static void stub_10(CPU *cpu) { cpu->eax = 10u; }
static void stub_11(CPU *cpu) { cpu->eax = 11u; }
static void stub_12(CPU *cpu) { cpu->eax = 12u; }
static void stub_13(CPU *cpu) { cpu->eax = 13u; }
static void stub_14(CPU *cpu) { cpu->eax = 14u; }
static void stub_15(CPU *cpu) { cpu->eax = 15u; }
static ac1_lifted_fn const stubs[16] = {
    stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7,
    stub_8, stub_9, stub_10, stub_11, stub_12, stub_13, stub_14, stub_15,
};

#define FIXTURE_FUNCTION_COUNT 64059u
typedef struct { uint32_t guest_va; ac1_lifted_fn function; } fixture_entry;
static fixture_entry fixture_functions[FIXTURE_FUNCTION_COUNT];
static unsigned fallback_calls;

static ac1_lifted_fn ac1_lifted_lookup(uint32_t target)
{
    ++fallback_calls;
    size_t lo = 0u, hi = FIXTURE_FUNCTION_COUNT;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        if (fixture_functions[mid].guest_va < target) lo = mid + 1u;
        else hi = mid;
    }
    return lo < FIXTURE_FUNCTION_COUNT &&
           fixture_functions[lo].guest_va == target
        ? fixture_functions[lo].function : NULL;
}

static ac1_lifted_lookup_cache_slot cache[AC1_LIFTED_LOOKUP_CACHE_SLOTS];

static int check(int passed, const char *message)
{
    if (!passed) fprintf(stderr, "FAIL: %s\n", message);
    return passed;
}

int main(void)
{
    int ok = 1;
    for (unsigned i = 0u; i < FIXTURE_FUNCTION_COUNT; ++i) {
        fixture_functions[i].guest_va = 0x00400000u + i * 0x30u;
        fixture_functions[i].function = stubs[i % 16u];
    }
    memset(cache, 0, sizeof(cache));

    const uint32_t first = fixture_functions[101u].guest_va;
    const unsigned first_slot = (first * UINT32_C(0x9e3779b1)) >> 22;
    const ac1_lifted_fn first_expected = fixture_functions[101u].function;
    ok &= check(ac1_lifted_lookup_cached(first, 1, cache, ac1_lifted_lookup) == first_expected,
                "first exact lookup uses binary-search fallback");
    const unsigned calls_after_first = fallback_calls;
    ok &= check(ac1_lifted_lookup_cached(first, 1, cache, ac1_lifted_lookup) == first_expected,
                "repeated exact address hits cache");
    ok &= check(fallback_calls == calls_after_first,
                "cache hit skips binary-search fallback");

    unsigned collision_index = FIXTURE_FUNCTION_COUNT;
    for (unsigned i = 102u; i < FIXTURE_FUNCTION_COUNT; ++i) {
        const uint32_t address = fixture_functions[i].guest_va;
        if (((address * UINT32_C(0x9e3779b1)) >> 22) == first_slot &&
            fixture_functions[i].function != first_expected) {
            collision_index = i;
            break;
        }
    }
    ok &= check(collision_index < FIXTURE_FUNCTION_COUNT,
                "fixture contains a distinct-function hash collision");
    if (collision_index < FIXTURE_FUNCTION_COUNT) {
        const uint32_t collision = fixture_functions[collision_index].guest_va;
        const ac1_lifted_fn expected = fixture_functions[collision_index].function;
        ok &= check(ac1_lifted_lookup_cached(collision, 1, cache, ac1_lifted_lookup) == expected,
                    "collision resolves through exact fallback");
        ok &= check(ac1_lifted_lookup_cached(first, 1, cache, ac1_lifted_lookup) == first_expected,
                    "collision cannot return the wrong function pointer");
    }

    const unsigned calls_before_unknown = fallback_calls;
    const uint32_t interior = fixture_functions[101u].guest_va + 1u;
    ok &= check(ac1_lifted_lookup_cached(interior, 1, cache, ac1_lifted_lookup) == NULL,
                "interior target remains unresolved");
    ok &= check(ac1_lifted_lookup_cached(interior, 1, cache, ac1_lifted_lookup) == NULL,
                "unknown targets are not negatively cached");
    ok &= check(fallback_calls == calls_before_unknown + 2u,
                "repeated unknown targets both use fallback");
    ok &= check(ac1_lifted_lookup_cached(0x18000000u, 1, cache, ac1_lifted_lookup) == NULL,
                "Bink-range target is not a main-module cache result");

    puts("AC1 main lookup cache fixture passed");
    return ok ? 0 : 1;
}
