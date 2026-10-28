#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Thread_local uint32_t g_image_delta;
void L_00F9FB80(CPU *cpu);
void dispatch(CPU *cpu, uint32_t target) {
    fprintf(stderr, "unexpected external dispatch: target=%08x eip=%08x\n", target, cpu->eip);
    exit(2);
}
void dispatch_jmp(CPU *cpu, uint32_t target) { dispatch(cpu, target); }
void ac1_unsupported_instruction(uint32_t address, const char *reason) {
    fprintf(stderr, "unsupported instruction %08x: %s\n", address, reason);
    exit(3);
}

static unsigned cases;
static int check_case(uint8_t *data, unsigned src, unsigned dst, unsigned length) {
    uint8_t expected[16384];
    for (unsigned i = 0; i < sizeof(expected); ++i)
        data[i] = (uint8_t)((i * 73u + i / 7u + cases * 17u) & 255u);
    memcpy(expected, data, sizeof(expected));
    memmove(expected + dst, expected + src, length);
    CPU cpu = {0};
    cpu.esp = 0x7FE0FF00u;
    cpu.ebp = 0x12345678u;
    cpu.esi = 0x23456789u;
    cpu.edi = 0x3456789Au;
    cpu.ebx = 0x456789ABu;
    cpu.eflags_misc = 2u;
    wr32(cpu.esp, 0xDEADBEEFu);
    wr32(cpu.esp + 4u, 0x20000000u + dst);
    wr32(cpu.esp + 8u, 0x20000000u + src);
    wr32(cpu.esp + 12u, length);
    L_00F9FB80(&cpu);
    if (memcmp(data, expected, sizeof(expected)) ||
        cpu.eax != 0x20000000u + dst || cpu.esp != 0x7FE0FF04u ||
        rd32(0x7FE0FF00u) != 0xDEADBEEFu || cpu.ebp != 0x12345678u ||
        cpu.esi != 0x23456789u || cpu.edi != 0x3456789Au ||
        cpu.ebx != 0x456789ABu || cpu.df) {
        fprintf(stderr, "memmove mismatch: src=%u dst=%u length=%u eax=%08x esp=%08x eip=%08x df=%u\n",
                src, dst, length, cpu.eax, cpu.esp, cpu.eip, cpu.df);
        return 0;
    }
    ++cases;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: memmove_semantics <original-jump-table-page>\n");
        return 4;
    }
    guest_address_space *space = guest_address_space_create();
    if (!space) return 4;
    guest_address_space_select(space);
    uint8_t *data = guest_memory_allocate(0x20000000u, 16384u);
    uint8_t *tables = guest_memory_allocate(0x00F9F000u, 4096u);
    if (!data || !guest_memory_allocate(0x7FE00000u, 65536u) ||
        !guest_memory_allocate(0x01A44000u, 4096u) || !tables) return 4;
    FILE *original_data = fopen(argv[1], "rb");
    if (!original_data || fread(tables, 1, 4096, original_data) != 4096) return 4;
    fclose(original_data);
    /* Select the original scalar copy path; SIMD has a separate entry. */
    wr32(0x01A44E90u, 0u);
    unsigned lengths[64], count = 0;
    for (unsigned n = 0; n <= 40; ++n) lengths[count++] = n;
    const unsigned extra[] = {63,64,65,127,128,129,255,256,257,511,512,513,1023};
    for (unsigned i = 0; i < sizeof(extra)/sizeof(extra[0]); ++i) lengths[count++] = extra[i];
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned a = 0; a < 4; ++a) {
            for (unsigned b = 0; b < 4; ++b) {
                if (!check_case(data, 1024+a, 8192+b, lengths[i]) ||
                    !check_case(data, 8192+a, 1024+b, lengths[i])) return 1;
            }
            for (int delta = -7; delta <= 7; ++delta)
                if (!check_case(data, 2048+a, (unsigned)(2048+(int)a+delta), lengths[i])) return 1;
        }
    }
    guest_address_space_select(NULL);
    guest_address_space_destroy(space);
    printf("original lifted scalar memmove passed: %u alignment/length/overlap cases\n", cases);
    return 0;
}
