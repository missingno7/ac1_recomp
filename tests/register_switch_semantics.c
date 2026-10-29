#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

_Thread_local uint32_t g_image_delta;
static uint32_t fallback_target, fallback_eip;
void L_00401000(CPU *cpu);
void dispatch(CPU *cpu, uint32_t target) { (void)cpu; (void)target; abort(); }
void dispatch_jmp(CPU *cpu, uint32_t target) {
    fallback_target = target;
    fallback_eip = cpu->eip;
}
void ac1_unsupported_instruction(uint32_t va, const char *reason) {
    fprintf(stderr, "unexpected unsupported %08x: %s\n", va, reason);
    abort();
}

int main(void) {
    guest_address_space *space = guest_address_space_create();
    if (!space) return 2;
    guest_address_space_select(space);
    if (!guest_memory_allocate(0x00402000u, 4096u) ||
        !guest_memory_allocate(0x20000000u, 4096u)) return 2;
    const uint32_t inputs[] = {0, 1, 2, 3, 14, 0x7fffffffu, 0xffffffffu};
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
            CPU cpu = {0};
            cpu.esp = 0x20000800u;
            cpu.ecx = 0x12345678u;
            wr32(cpu.esp, 0xabcdef01u);
            wr32(cpu.esp + 4u, inputs[i]);
            wr32(cpu.esp + 8u, 0x76543210u);
            for (unsigned k = 0; k < 3; ++k) wr32(0x00402000u + 4u*k, 0x00401100u + 0x20u*k);
            fallback_target = fallback_eip = 0;
            L_00401000(&cpu);
            uint32_t expected = inputs[i] < 3u ? (inputs[i]+1u)*11u : 0u;
            if (cpu.eax != expected || cpu.esp != 0x2000080cu || fallback_target ||
                rd32(0x20000800u) != 0xabcdef01u || cpu.ecx != 0x12345678u) return 1;
        }
    }
    CPU cpu = {0};
    cpu.esp = 0x20000800u;
    wr32(cpu.esp + 4u, 1u);
    wr32(0x00402004u, 0xdead1234u);
    L_00401000(&cpu);
    if (fallback_target != 0xdead1234u || fallback_eip != FIXTURE_JUMP ||
        cpu.edx != 0xdead1234u || cpu.esp != 0x20000800u) return 1;
    guest_address_space_select(NULL);
    guest_address_space_destroy(space);
    puts("register switch semantics passed: 700 cases, default, RET 8, loaded value and unknown fallback");
    return 0;
}
