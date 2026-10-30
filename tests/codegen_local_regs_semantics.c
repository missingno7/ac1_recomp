/* Synthetic import tail: a leading donor comment must not hide CPU reads. */
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x40000];
static unsigned calls;
void guest_execution_read(uint32_t address, void *output, size_t width)
{
    assert((uint64_t)address + width <= sizeof memory);
    memcpy(output, memory + address, width);
}
void guest_execution_write(uint32_t address, const void *input, size_t width)
{
    assert((uint64_t)address + width <= sizeof memory);
    memcpy(memory + address, input, width);
}
void dispatch_jmp(CPU *c, uint32_t target)
{
    assert(target == 0xf1000184u);
    assert(c->ecx == 0x10200u);
    assert(c->esp == 0x10108u);
    assert(rd32(c->esp + 4u) == 0x10200u);
    c->eax ^= 0xc0de1234u;
    c->esp += 8u;
    ++calls;
}
void L_00401000(CPU *);
int main(void)
{
    for (unsigned i = 0; i < 1000; ++i) {
        CPU c = {0};
        c.eax = 0x12345678u + i;
        c.ecx = 0xbad0u;
        c.esp = 0x10100u;
        wr32(0x10000u, 0xf1000184u);
        L_00401000(&c);
        assert(c.eax == ((0x12345678u + i) ^ 0xc0de1234u));
        assert(c.ecx == 0x10200u && c.esp == 0x10110u);
    }
    assert(calls == 1000u);
    puts("local registers: 1000 leading-comment import tails passed");
    return 0;
}
