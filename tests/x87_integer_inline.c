/* Differential oracle for the declaration-only x87 integer inline gate. */
#include "cpu.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>

uint64_t baseline_integer(CPU *, double, unsigned, unsigned, uint32_t);
uint64_t inline_integer(CPU *, double, unsigned, unsigned, uint32_t);

static CPU *observed;
static struct observation {
    unsigned count;
    uint32_t va;
    CPU cpu;
    char reason[96];
} diagnostic;

void ac1_unsupported_instruction(uint32_t va, const char *reason) {
    ++diagnostic.count;
    diagnostic.va = va;
    memcpy(&diagnostic.cpu, observed, sizeof(CPU));
    snprintf(diagnostic.reason, sizeof diagnostic.reason, "%s", reason);
}

static uint64_t state = UINT64_C(0x184230781734abef);
static uint64_t random64(void) {
    state ^= state << 13; state ^= state >> 7;
    return state ^= state << 17;
}

int main(void) {
    static const uint64_t corners[] = {
        0, UINT64_C(0x8000000000000000), 1, UINT64_C(0x8000000000000001),
        UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000001), UINT64_C(0xfff8000000000123),
        UINT64_C(0x7ff0000000000001), UINT64_C(0xfff0000000000001),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff)
    };
    double values[1200]; unsigned n = 0;
    for (unsigned i = 0; i < sizeof corners / sizeof *corners; ++i)
        memcpy(&values[n++], &corners[i], sizeof(double));
    static const double small[] = {
        -2.5, -2.0, -1.5, -1.0, -0.5, -0.25, 0.25, 0.5, 1.0, 1.5, 2.0, 2.5
    };
    for (unsigned i = 0; i < sizeof small / sizeof *small; ++i) values[n++] = small[i];
    static const unsigned widths[] = {16, 32, 64};
    for (unsigned i = 0; i < 3; ++i) {
        double limit = ldexp(1.0, widths[i] - 1);
        double boundary[] = {
            limit, nextafter(limit, 0.0), nextafter(limit, INFINITY),
            -limit, nextafter(-limit, 0.0), nextafter(-limit, -INFINITY),
            limit - 0.5, -limit - 0.5
        };
        for (unsigned j = 0; j < sizeof boundary / sizeof *boundary; ++j) values[n++] = boundary[j];
    }
    for (unsigned i = 0; i < 1024; ++i) {
        uint64_t raw = random64(); memcpy(&values[n++], &raw, sizeof(double));
    }
    static const int rounding[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    unsigned cases = 0;
    for (unsigned host = 0; host < 4; ++host) {
        assert(!fesetround(rounding[host]));
        for (unsigned rc = 0; rc < 4; ++rc)
        for (unsigned truncate_mode = 0; truncate_mode < 2; ++truncate_mode)
        for (unsigned width = 0; width < 3; ++width)
        for (unsigned masks = 0; masks < 4; ++masks)
        for (unsigned i = 0; i < n; ++i) {
            CPU seed, expected, actual;
            memset(&seed, 0xa5, sizeof seed);
            seed.fpu_cw = (uint16_t)((0x037fu & ~0x21u) | (rc << 10) |
                ((masks & 1) ? 1u : 0u) | ((masks & 2) ? 0x20u : 0u));
            seed.fpu_sw = (uint16_t)random64();
            memcpy(&expected, &seed, sizeof seed);
            memcpy(&actual, &seed, sizeof seed);
            uint32_t va = 0x00401000u + cases;
            memset(&diagnostic, 0, sizeof diagnostic); observed = &expected;
            assert(!feclearexcept(FE_ALL_EXCEPT));
            uint64_t a = baseline_integer(&expected, values[i], widths[width], truncate_mode, va);
            int baseline_exceptions = fetestexcept(FE_ALL_EXCEPT);
            struct observation saved; memcpy(&saved, &diagnostic, sizeof saved);
            memset(&diagnostic, 0, sizeof diagnostic); observed = &actual;
            assert(!feclearexcept(FE_ALL_EXCEPT));
            uint64_t b = inline_integer(&actual, values[i], widths[width], truncate_mode, va);
            int inline_exceptions = fetestexcept(FE_ALL_EXCEPT);
            assert(a == b && !memcmp(&expected, &actual, sizeof actual));
            assert(!memcmp(&saved, &diagnostic, sizeof saved));
            assert(baseline_exceptions == inline_exceptions);
            ++cases;
        }
    }
    assert(!fesetround(FE_TONEAREST));
    printf("%u bit-exact x87 integer inline cases (CPU, diagnostics, host fenv) passed\n", cases);
    return 0;
}
