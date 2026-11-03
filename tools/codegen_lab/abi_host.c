/* Host side of abi_chain.c, built as ordinary AAPCS64 code (no reserved registers).
 *
 * host_import is an import with a callback: it pops its return address, calls
 * guest D back with ecx = esi and adds 7 to its result. Host code enters guest
 * code only through enter_A / enter_D, which live in the guest translation unit
 * (PROTO 1 needs them there: they save the host's x20..x27 by hand). With
 * PROTO 1, dispatch_aapcs is the broken intermediary: an ordinary function that
 * saves and restores x20..x27 around a guest call. */
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t r[8]; uint32_t eip; } cpu_t;
enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
extern uint8_t guest_memory[1 << 20];
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define N(x) CAT(PFX, x)

void N(enter_D)(cpu_t *c);

void N(host_import)(cpu_t *c) {
    c->r[ESP] += 4;                                      /* the import's ret */
    uint32_t ret = 0x403000u;
    c->r[ESP] -= 4; memcpy(guest_memory + (c->r[ESP] & 0xFFFFF), &ret, 4);
    c->r[ECX] = c->r[ESI];
    N(enter_D)(c);
    c->r[EAX] += 7u;
}

#if PROTO == 1
void N(dispatch)(cpu_t *c, uint32_t t);
void N(dispatch_aapcs)(cpu_t *c, uint32_t t) {
    /* Any AAPCS function that uses x20..x27 saves them on entry and restores them on return. */
    __asm__ __volatile__("" ::: "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27");
    N(dispatch)(c, t);
    /* Work after the guest call (as the runtime's checkpoint does) keeps this
     * from becoming a tail call, whose early epilogue would hide the restore. */
    ++c->eip;
}
#endif
