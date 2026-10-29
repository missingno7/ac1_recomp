#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#if !defined(__GNUC__) || !defined(__x86_64__)
#error This fixture requires the MinGW x86-64 MMX assembler syntax.
#endif

static uint64_t movq_load(const void *address)
{
    uint64_t value = 0;
    __asm__ volatile("movq (%1), %%mm0\n\t"
                     "movq %%mm0, %0\n\t"
                     "emms\n\t"
                     : "=m"(value) : "r"(address) : "mm0");
    return value;
}

static void movq_store(void *address, uint64_t value)
{
    __asm__ volatile("movq %1, %%mm0\n\t"
                     "movq %%mm0, (%0)\n\t"
                     "emms\n\t"
                     : : "r"(address), "m"(value) : "mm0", "memory");
}

static uint64_t pcmpgtw(uint64_t a, uint64_t b)
{
    uint64_t value = 0;
    __asm__ volatile("movq %1, %%mm0\n\t"
                     "movq %2, %%mm1\n\t"
                     "pcmpgtw %%mm1, %%mm0\n\t"
                     "movq %%mm0, %0\n\t"
                     "emms\n\t"
                     : "=m"(value) : "m"(a), "m"(b) : "mm0", "mm1");
    return value;
}

static uint64_t paddw(uint64_t a, uint64_t b)
{
    uint64_t value = 0;
    __asm__ volatile("movq %1, %%mm0\n\t"
                     "movq %2, %%mm1\n\t"
                     "paddw %%mm1, %%mm0\n\t"
                     "movq %%mm0, %0\n\t"
                     "emms\n\t"
                     : "=m"(value) : "m"(a), "m"(b) : "mm0", "mm1");
    return value;
}

static uint64_t psubw(uint64_t a, uint64_t b)
{
    uint64_t value = 0;
    __asm__ volatile("movq %1, %%mm0\n\t"
                     "movq %2, %%mm1\n\t"
                     "psubw %%mm1, %%mm0\n\t"
                     "movq %%mm0, %0\n\t"
                     "emms\n\t"
                     : "=m"(value) : "m"(a), "m"(b) : "mm0", "mm1");
    return value;
}

static uint64_t pand(uint64_t a, uint64_t b)
{
    uint64_t value = 0;
    __asm__ volatile("movq %1, %%mm0\n\t"
                     "movq %2, %%mm1\n\t"
                     "pand %%mm1, %%mm0\n\t"
                     "movq %%mm0, %0\n\t"
                     "emms\n\t"
                     : "=m"(value) : "m"(a), "m"(b) : "mm0", "mm1");
    return value;
}

static uint64_t dup16(uint16_t word)
{
    return uint64_t(word) | (uint64_t(word) << 16) |
           (uint64_t(word) << 32) | (uint64_t(word) << 48);
}
static uint64_t alternating16(uint16_t even, uint16_t odd)
{
    return uint64_t(even) | (uint64_t(odd) << 16) |
           (uint64_t(even) << 32) | (uint64_t(odd) << 48);
}

// The exact reached Havok MMX path: 16-byte records, 12-byte/6-word params.
// Helper calls each execute native MMX instructions and EMMS, keeping this
// fixture's state scope explicit while validating operation and lane semantics.
static void oracle_01023410(uint8_t *records, uint32_t count,
                            const uint16_t params[6])
{
    const uint64_t t01 = alternating16(static_cast<uint16_t>(params[0] - 1u),
                                       static_cast<uint16_t>(params[1] - 1u));
    const uint64_t t23 = alternating16(static_cast<uint16_t>(params[2] - 2u),
                                       static_cast<uint16_t>(params[3] - 2u));
    const uint64_t t4 = dup16(static_cast<uint16_t>(params[4] - 1u));
    const uint64_t t5 = dup16(static_cast<uint16_t>(params[5] - 2u));
    constexpr uint64_t low_two_words = UINT64_C(0x00000000ffffffff);
    for (uint32_t i = 0; i < count; ++i, records += 16) {
        const uint64_t a = movq_load(records);
        const uint64_t b = movq_load(records + 8);
        const uint64_t ma = paddw(pcmpgtw(a, t01), pcmpgtw(a, t23));
        const uint64_t mb = pand(paddw(pcmpgtw(b, t4), pcmpgtw(b, t5)),
                                 low_two_words);
        movq_store(records, psubw(a, ma));
        movq_store(records + 8, psubw(b, mb));
    }
    __asm__ volatile("emms" ::: "memory");
}

static void oracle_01023520(uint8_t *records, uint32_t count,
                            const uint16_t params[6])
{
    const uint64_t t01 = alternating16(params[0], params[1]);
    const uint64_t t23 = alternating16(params[2], params[3]);
    const uint64_t t4 = dup16(params[4]);
    const uint64_t t5 = dup16(params[5]);
    constexpr uint64_t low_two_words = UINT64_C(0x00000000ffffffff);
    for (uint32_t i = 0; i < count; ++i, records += 16) {
        const uint64_t a = movq_load(records);
        const uint64_t b = movq_load(records + 8);
        const uint64_t ma = paddw(pcmpgtw(a, t01), pcmpgtw(a, t23));
        const uint64_t mb = pand(paddw(pcmpgtw(b, t4), pcmpgtw(b, t5)),
                                 low_two_words);
        movq_store(records, paddw(a, ma));
        movq_store(records + 8, paddw(b, mb));
    }
    __asm__ volatile("emms" ::: "memory");
}

static int32_t s16(uint16_t v)
{
    return (v & 0x8000u) ? static_cast<int32_t>(v) - 65536
                         : static_cast<int32_t>(v);
}
static uint64_t pack_words(const uint16_t w[4])
{
    return uint64_t(w[0]) | (uint64_t(w[1]) << 16) |
           (uint64_t(w[2]) << 32) | (uint64_t(w[3]) << 48);
}

static uint16_t adjustment(uint16_t value, uint16_t t0, uint16_t t1)
{
    return static_cast<uint16_t>((s16(value) > s16(t0) ? 1 : 0) +
                                 (s16(value) > s16(t1) ? 1 : 0));
}
static void reference_01023410(uint8_t *records, uint32_t count,
                               const uint16_t params[6])
{
    for (uint32_t i = 0; i < count; ++i, records += 16) {
        uint16_t a[4], b[4];
        std::memcpy(a, records, 8); std::memcpy(b, records + 8, 8);
        for (unsigned lane = 0; lane < 4; ++lane) {
            a[lane] = static_cast<uint16_t>(a[lane] + adjustment(
                a[lane], static_cast<uint16_t>(params[(lane & 1u)] - 1u),
                static_cast<uint16_t>(params[2u + (lane & 1u)] - 2u)));
            if (lane < 2) b[lane] = static_cast<uint16_t>(b[lane] + adjustment(
                b[lane], static_cast<uint16_t>(params[4] - 1u),
                static_cast<uint16_t>(params[5] - 2u)));
        }
        std::memcpy(records, a, 8); std::memcpy(records + 8, b, 8);
    }
}
static void reference_01023520(uint8_t *records, uint32_t count,
                               const uint16_t params[6])
{
    for (uint32_t i = 0; i < count; ++i, records += 16) {
        uint16_t a[4], b[4];
        std::memcpy(a, records, 8); std::memcpy(b, records + 8, 8);
        for (unsigned lane = 0; lane < 4; ++lane) {
            a[lane] = static_cast<uint16_t>(a[lane] - adjustment(
                a[lane], params[lane & 1u], params[2u + (lane & 1u)]));
            if (lane < 2) b[lane] = static_cast<uint16_t>(b[lane] - adjustment(
                b[lane], params[4], params[5]));
        }
        std::memcpy(records, a, 8); std::memcpy(records + 8, b, 8);
    }
}

static uint32_t rng_state = 0x6d2b79f5u;
static uint32_t next_random()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static bool check(bool ok, const char *message)
{
    if (!ok) std::fprintf(stderr, "FAIL: %s\n", message);
    return ok;
}

static bool check_movq_guard_page()
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    auto *memory = static_cast<uint8_t *>(VirtualAlloc(nullptr, page * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return check(false, "VirtualAlloc guard fixture");
    DWORD old_protect = 0;
    if (!VirtualProtect(memory + page, page, PAGE_NOACCESS, &old_protect)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return check(false, "VirtualProtect guard page");
    }
    uint8_t *edge = memory + page - 8;
    const uint64_t expected = UINT64_C(0x8877665544332211);
    std::memcpy(edge, &expected, 8);
    if (!check(movq_load(edge) == expected, "MOVQ load reads exactly the final 8 readable bytes")) {
        VirtualFree(memory, 0, MEM_RELEASE); return false;
    }
    const uint64_t replacement = UINT64_C(0x0123456789abcdef);
    movq_store(edge, replacement);
    uint64_t observed = 0;
    std::memcpy(&observed, edge, 8);
    const bool ok = check(observed == replacement,
                          "MOVQ store writes exactly the final 8 readable bytes");
    VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

int main()
{
    bool ok = true;
    const uint16_t lhs_words[4] = {0x8000u, 0xffffu, 0u, 0x7fffu};
    const uint16_t rhs_words[4] = {0x7fffu, 0u, 0xffffu, 0x8000u};
    const uint16_t cmp_expected[4] = {0u, 0u, 0xffffu, 0xffffu};
    ok &= check(pcmpgtw(pack_words(lhs_words), pack_words(rhs_words)) ==
                pack_words(cmp_expected), "PCMPGTW signed boundaries and lane order");
    const uint16_t add_a[4] = {0x7fffu, 0x8000u, 0u, 0xffffu};
    const uint16_t add_b[4] = {1u, 0xffffu, 0xffffu, 1u};
    const uint16_t add_expected[4] = {0x8000u, 0x7fffu, 0xffffu, 0u};
    ok &= check(paddw(pack_words(add_a), pack_words(add_b)) ==
                pack_words(add_expected), "PADDW wraps each 16-bit lane");
    const uint16_t sub_a[4] = {0x8000u, 0u, 1u, 0u};
    const uint16_t sub_b[4] = {1u, 1u, 2u, 0xffffu};
    const uint16_t sub_expected[4] = {0x7fffu, 0xffffu, 0xffffu, 1u};
    ok &= check(psubw(pack_words(sub_a), pack_words(sub_b)) ==
                pack_words(sub_expected), "PSUBW wraps each 16-bit lane");
    ok &= check(pand(UINT64_C(0xffff80007fff0001), UINT64_C(0x00000000ffffffff)) ==
                UINT64_C(0x000000007fff0001), "PAND preserves only low two words");
    ok &= check(check_movq_guard_page(), "MOVQ guard-page tests");

    constexpr uint32_t cases = 20000;
    std::vector<uint8_t> initial(cases * 16 + 32);
    for (auto &v : initial) v = static_cast<uint8_t>(next_random());
    const uint16_t edge_params[][6] = {
        {0x0000, 0x7fff, 0x0001, 0x8000, 0xffff, 0x0000},
        {0x7fff, 0xffff, 0x8000, 0x0000, 0x0001, 0xffff},
        {0x8000, 0x0000, 0xffff, 0x7fff, 0x8001, 0x7ffe},
    };
    for (unsigned p = 0; p < 3; ++p) {
        std::vector<uint8_t> expected = initial;
        std::vector<uint8_t> actual = initial;
        reference_01023410(expected.data() + 8, cases, edge_params[p]);
        oracle_01023410(actual.data() + 8, cases, edge_params[p]);
        ok &= check(actual == expected, "01023410 lane transform, thresholds, tails and sentinels");
        expected = initial; actual = initial;
        reference_01023520(expected.data() + 8, cases, edge_params[p]);
        oracle_01023520(actual.data() + 8, cases, edge_params[p]);
        ok &= check(actual == expected, "01023520 lane transform, thresholds, tails and sentinels");
    }
    for (unsigned p = 0; p < 128; ++p) {
        uint16_t params[6];
        for (auto &v : params) v = static_cast<uint16_t>(next_random());
        std::vector<uint8_t> expected(cases * 16), actual;
        for (auto &v : expected) v = static_cast<uint8_t>(next_random());
        actual = expected;
        std::vector<uint8_t> original = expected;
        reference_01023410(expected.data(), cases, params);
        oracle_01023410(actual.data(), cases, params);
        ok &= check(actual == expected, "01023410 randomized records");
        expected = original; actual = original;
        reference_01023520(expected.data(), cases, params);
        oracle_01023520(actual.data(), cases, params);
        ok &= check(actual == expected, "01023520 randomized records");
        if (!ok) return 1;
    }
    if (!ok) return 1;
    std::printf("native MMX oracle passed: PCMPGTW/PADDW/PSUBW/PAND, MOVQ 8-byte guard boundary, two Havok record transforms; %u randomized parameter sets x %u records each\n",
                128u, cases);
    return 0;
}
