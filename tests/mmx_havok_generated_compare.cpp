#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>

extern "C" {
#define _Thread_local thread_local
#include "cpu.h"
#undef _Thread_local
void L_01023410(CPU *);
void L_01023520(CPU *);
void ac1_unsupported_instruction(uint32_t, const char *);
void guest_execution_read(uint32_t, void *, size_t);
void guest_execution_write(uint32_t, const void *, size_t);
}

#define main mmx_oracle_selftest_main
#include "mmx_havok_native_oracle.cpp"
#undef main

static constexpr uint32_t GUEST_BASE = 0x00100000u;
static std::array<uint8_t, 0x20000> guest_mem{};

extern "C" void guest_execution_read(uint32_t address, void *output, size_t width)
{
    if (address < GUEST_BASE || uint64_t(address - GUEST_BASE) + width > guest_mem.size()) {
        std::fprintf(stderr, "out-of-range guest read %08x width %zu\n", address, width);
        std::abort();
    }
    std::memcpy(output, guest_mem.data() + (address - GUEST_BASE), width);
}

extern "C" void guest_execution_write(uint32_t address, const void *input, size_t width)
{
    if (address < GUEST_BASE || uint64_t(address - GUEST_BASE) + width > guest_mem.size()) {
        std::fprintf(stderr, "out-of-range guest write %08x width %zu\n", address, width);
        std::abort();
    }
    std::memcpy(guest_mem.data() + (address - GUEST_BASE), input, width);
}

extern "C" void ac1_unsupported_instruction(uint32_t eip, const char *reason)
{
    std::fprintf(stderr, "unexpected translated trap at %08x: %s\n", eip, reason);
    std::abort();
}

static uint32_t rng = 0x67a95321u;
static uint32_t next_u32()
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static bool run_case(void (*translated)(CPU *),
                     void (*oracle)(uint8_t *, uint32_t, const uint16_t *),
                     const char *name, uint32_t count, const uint16_t params[6],
                     uint32_t sequence)
{
    constexpr uint32_t stack = GUEST_BASE + 0x1000u;
    constexpr uint32_t data = GUEST_BASE + 0x4000u;
    constexpr uint32_t parameter_address = GUEST_BASE + 0x8000u;
    constexpr size_t guard = 32;
    const size_t data_size = size_t(count) * 16u + guard * 2u;
    std::vector<uint8_t> expected(data_size);
    for (auto &byte : expected) byte = uint8_t(next_u32());
    std::vector<uint8_t> initial = expected;
    oracle(expected.data() + guard, count, params);

    guest_mem.fill(0);
    std::memcpy(guest_mem.data() + (data - GUEST_BASE), initial.data(), initial.size());
    std::memcpy(guest_mem.data() + (parameter_address - GUEST_BASE), params, 12);
    const uint32_t entry_esp = stack;
    uint32_t call_frame[] = {0xdec0ad01u, data + uint32_t(guard), count, parameter_address};
    std::memcpy(guest_mem.data() + (entry_esp - GUEST_BASE), call_frame, sizeof call_frame);

    CPU cpu{};
    cpu.eax = 0x11111111u; cpu.ecx = 0x22222222u; cpu.edx = 0x33333333u;
    cpu.ebx = 0x44444444u; cpu.esi = 0x55555555u; cpu.edi = 0x66666666u;
    cpu.esp = entry_esp; cpu.ebp = 0x77777777u;
    cpu.cf = 1; cpu.zf = 1; cpu.sf = 0; cpu.of = 1; cpu.pf = 0; cpu.af = 1;
    cpu.df = 1; cpu.fpu_top = 5;
    translated(&cpu);

    const uint8_t *actual = guest_mem.data() + (data - GUEST_BASE);
    if (!std::equal(expected.begin(), expected.end(), actual)) {
        std::fprintf(stderr, "%s count=%u case=%u record/sentinel bytes differ\n", name, count, sequence);
        for (size_t i = 0; i < expected.size(); ++i) if (expected[i] != actual[i]) {
            std::fprintf(stderr, "first difference +%zu expected=%02x actual=%02x\n", i, expected[i], actual[i]);
            break;
        }
        if (count) {
            uint16_t before[8], want[8], got[8];
            std::memcpy(before, initial.data() + guard, 16);
            std::memcpy(want, expected.data() + guard, 16);
            std::memcpy(got, actual + guard, 16);
            std::fprintf(stderr, "params=%04x,%04x,%04x,%04x,%04x,%04x\n", params[0],params[1],params[2],params[3],params[4],params[5]);
            for (unsigned i = 0; i < 8; ++i) std::fprintf(stderr, " lane%u=%04x want=%04x got=%04x\n", i,before[i],want[i],got[i]);
        }
        std::fprintf(stderr, "esp=%08x ebp=%08x eip=%08x eax=%08x ecx=%08x edx=%08x\n",
                     cpu.esp, cpu.ebp, cpu.eip, cpu.eax, cpu.ecx, cpu.edx);
        return false;
    }
    if (cpu.esp != entry_esp + 16u || cpu.ebp != 0x77777777u ||
        cpu.ebx != 0x44444444u || cpu.esi != 0x55555555u || cpu.edi != 0x66666666u) {
        std::fprintf(stderr, "%s count=%u case=%u ABI register/stack mismatch\n", name, count, sequence);
        return false;
    }
    if (cpu.df != 1u || cpu.fpu_top != 0) {
        std::fprintf(stderr, "%s count=%u case=%u DF/EMMS state mismatch\n", name, count, sequence);
        return false;
    }
    return true;
}

int main()
{
    const uint32_t counts[] = {0, 1, 2, 7, 31};
    const uint16_t params[][6] = {
        {0x0000, 0x7fff, 0x0001, 0x8000, 0xffff, 0x0000},
        {0x7fff, 0xffff, 0x8000, 0x0000, 0x0001, 0xffff},
        {0x8000, 0x0000, 0xffff, 0x7fff, 0x8001, 0x7ffe},
        {0xffff, 0x8001, 0x0002, 0x7fff, 0x0000, 0xffff},
    };
    unsigned cases = 0;
    for (unsigned p = 0; p < sizeof(params) / sizeof(params[0]); ++p) {
        for (uint32_t count : counts) {
            if (!run_case(L_01023410, oracle_01023410, "01023410", count, params[p], cases++)) return 1;
            if (!run_case(L_01023520, oracle_01023520, "01023520", count, params[p], cases++)) return 1;
        }
    }
    for (unsigned p = 0; p < 256; ++p) {
        uint16_t random_params[6];
        for (auto &word : random_params) word = uint16_t(next_u32());
        const uint32_t count = counts[p % (sizeof(counts) / sizeof(counts[0]))];
        if (!run_case(L_01023410, oracle_01023410, "01023410", count, random_params, cases++)) return 1;
        if (!run_case(L_01023520, oracle_01023520, "01023520", count, random_params, cases++)) return 1;
    }
    std::printf("generated MMX comparison passed: %u calls, zero/small/multiple counts, edge and randomized params, full-buffer sentinels, stack and nonvolatile registers\n", cases);
    return 0;
}

