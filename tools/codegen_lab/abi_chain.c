/* Guest-register calling protocols on a real call chain: exactness and cost.
 *
 * One small guest program, written once in lifted style, compiled under each
 * protocol (PROTO) and register subset (MASK, bit i = eax ecx edx ebx esp ebp
 * esi edi):
 *   0  struct: every register crosses calls in the CPU struct (today);
 *   1  pinned: MASK registers live in host x20..x27 across guest calls. Built
 *      with -ffixed-x20..x27 so this code never allocates them; host code
 *      (AAPCS) is entered and left through adapters that sync the struct;
 *   2  swiftcall: MASK registers are arguments and swiftcall return values
 *      (x0-x3), the rest go through the struct.
 * Chain: entry -> A (loop, push/pop ebx/esi) -> B (stdcall ret 4) -> either C
 * through the runtime dispatcher (indirect) or the host import H, which calls
 * guest D back through the host-to-guest adapter. PFX names the copy.
 * BROKEN_DISPATCH=1 routes PROTO 1's indirect call through an ordinary AAPCS
 * function that saves and restores x20..x27, which must break exactness. */
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t r[8]; uint32_t eip; } cpu_t;      /* eax ecx edx ebx esp ebp esi edi */
enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
extern uint8_t guest_memory[1 << 20];
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, guest_memory + (a & 0xFFFFF), 4); return v; }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(guest_memory + (a & 0xFFFFF), &v, 4); }

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define N(x) CAT(PFX, x)
#define PIN(i) ((MASK) >> (i) & 1)
#define NOINLINE __attribute__((noinline))
#define FOR_REGS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define LOCALS uint32_t g[8]

#if PROTO == 0
#define SIG(name) NOINLINE void name(cpu_t *c)
#define ENTER() FOR_REGS(LOAD_ONE)
#define LOAD_ONE(i) g[i] = c->r[i];
#define STORE_ONE(i) c->r[i] = g[i];
#define LEAVE() do { FOR_REGS(STORE_ONE) return; } while (0)
#define CALL(fn) do { FOR_REGS(STORE_ONE) fn(c); FOR_REGS(LOAD_ONE) } while (0)
#define CALL_INDIRECT(t) do { FOR_REGS(STORE_ONE) N(dispatch)(c, t); FOR_REGS(LOAD_ONE) } while (0)
#elif PROTO == 1
/* Pinned registers: x20+i holds guest register i (w-view). */
#define SIG(name) NOINLINE void name(cpu_t *c)
#define PGET(i) __asm__ __volatile__("mov %w0, w" #i : "=r"(g[i - 20]))
#define PSET(i) __asm__ __volatile__("mov w" #i ", %w0" : : "r"(g[i - 20]))
#define LOAD_ONE(i) if (PIN(i)) PGET_##i(); else g[i] = c->r[i];
#define STORE_ONE(i) if (PIN(i)) PSET_##i(); else c->r[i] = g[i];
#define PGET_0() PGET(20)
#define PGET_1() PGET(21)
#define PGET_2() PGET(22)
#define PGET_3() PGET(23)
#define PGET_4() PGET(24)
#define PGET_5() PGET(25)
#define PGET_6() PGET(26)
#define PGET_7() PGET(27)
#define PSET_0() PSET(20)
#define PSET_1() PSET(21)
#define PSET_2() PSET(22)
#define PSET_3() PSET(23)
#define PSET_4() PSET(24)
#define PSET_5() PSET(25)
#define PSET_6() PSET(26)
#define PSET_7() PSET(27)
#define ENTER() FOR_REGS(LOAD_ONE)
#define LEAVE() do { FOR_REGS(STORE_ONE) return; } while (0)
#define CALL(fn) do { FOR_REGS(STORE_ONE) fn(c); FOR_REGS(LOAD_ONE) } while (0)
#if BROKEN_DISPATCH
void N(dispatch_aapcs)(cpu_t *c, uint32_t t);
#define CALL_INDIRECT(t) do { FOR_REGS(STORE_ONE) N(dispatch_aapcs)(c, t); FOR_REGS(LOAD_ONE) } while (0)
#else
#define CALL_INDIRECT(t) do { FOR_REGS(STORE_ONE) N(dispatch)(c, t); FOR_REGS(LOAD_ONE) } while (0)
#endif
#elif PROTO == 2
/* MASK registers in four 64-bit slots: one per slot for up to four, else pairs. */
typedef struct { uint64_t s[4]; } r4;
#define SW __attribute__((swiftcall))
#define SIG(name) NOINLINE SW r4 name(cpu_t *c, r4 in)
#define NPIN __builtin_popcount(MASK)
static inline int slot_of(int i) { int k = 0; for (int j = 0; j < i; ++j) k += PIN(j); return k; }
#define GETV(v, i) (NPIN <= 4 ? (uint32_t)(v).s[slot_of(i)] : (uint32_t)((v).s[slot_of(i) / 2] >> (32 * (slot_of(i) & 1))))
#define PUTV(v, i, x) do { if (NPIN <= 4) (v).s[slot_of(i)] = (x); \
    else (v).s[slot_of(i) / 2] = ((v).s[slot_of(i) / 2] & ~(0xFFFFFFFFull << (32 * (slot_of(i) & 1)))) \
                                 | ((uint64_t)(x) << (32 * (slot_of(i) & 1))); } while (0)
#define LOAD_IN(i) g[i] = PIN(i) ? GETV(in, i) : c->r[i];
#define LOAD_OUT(i) g[i] = PIN(i) ? GETV(out, i) : c->r[i];
#define STORE_OUT(i) if (PIN(i)) PUTV(out, i, g[i]); else c->r[i] = g[i];
#define ENTER() FOR_REGS(LOAD_IN)
#define LEAVE() do { r4 out = {{0}}; FOR_REGS(STORE_OUT) return out; } while (0)
#define CALL(fn) do { r4 out = {{0}}; FOR_REGS(STORE_OUT) out = fn(c, out); FOR_REGS(LOAD_OUT) } while (0)
#define CALL_INDIRECT(t) do { r4 out = {{0}}; FOR_REGS(STORE_OUT) out = N(dispatch)(c, out, t); FOR_REGS(LOAD_OUT) } while (0)
#endif

#define LOAD_STRUCT(i) g[i] = c->r[i];
#define STORE_STRUCT(i) c->r[i] = g[i];
#define PUSH(v) do { g[ESP] -= 4; wr32(g[ESP], (v)); } while (0)
#define POP(r) do { g[r] = rd32(g[ESP]); g[ESP] += 4; } while (0)
#define RET(n) do { g[ESP] += 4 + (n); LEAVE(); } while (0)

/* The host import and its callback adapter (host code, AAPCS, no reserved registers). */
void N(host_import)(cpu_t *c);
SIG(N(A)); SIG(N(B)); SIG(N(C)); SIG(N(D));
#if PROTO == 2
NOINLINE SW r4 N(dispatch)(cpu_t *c, r4 in, uint32_t t) { return N(C)(c, in); (void)t; }
#else
NOINLINE void N(dispatch)(cpu_t *c, uint32_t t) { if (t == 0xC0DEu) N(C)(c); else __builtin_trap(); }
#endif

/* A: push ebx; push esi; mov esi, ecx; for edx times: push eax; call B; add ebx, eax; pop esi; pop ebx; ret */
SIG(N(A)) {
    LOCALS; ENTER();
    PUSH(g[EBX]); PUSH(g[ESI]); g[ESI] = g[ECX];
    for (uint32_t k = 0, n = g[EDX]; k < n; ++k) {
        PUSH(g[EAX]); PUSH(0x401010u); CALL(N(B));
        g[EBX] += g[EAX]; g[EAX] ^= k;
    }
    POP(ESI); POP(EBX); RET(0);
}
/* B(arg): push edi; mov edi, [esp+8]; eax = edi*3 + [esi+4]; odd: call [esi] (C); even: call host import; pop edi; ret 4 */
SIG(N(B)) {
    LOCALS; ENTER();
    PUSH(g[EDI]); g[EDI] = rd32(g[ESP] + 8);
    g[EAX] = g[EDI] * 3u + rd32(g[ESI] + 4);
    PUSH(0x402010u);
    if (g[EAX] & 1) CALL_INDIRECT(rd32(g[ESI]));
    else {
#if PROTO == 0
        FOR_REGS(STORE_ONE) N(host_import)(c); FOR_REGS(LOAD_ONE)
#elif PROTO == 1
        FOR_REGS(STORE_STRUCT) N(host_import)(c); FOR_REGS(LOAD_STRUCT)
#else
        FOR_REGS(STORE_STRUCT) N(host_import)(c); FOR_REGS(LOAD_STRUCT)
#endif
    }
    POP(EDI); RET(4);
}
/* C: lea eax, [eax+1]; mov [esi+8], eax; ret */
SIG(N(C)) { LOCALS; ENTER(); g[EAX] += 1; wr32(g[ESI] + 8, g[EAX]); RET(0); }
/* D (callback): push esi; mov esi, ecx; mov eax, [esi+12]; add eax, 5; pop esi; ret */
SIG(N(D)) { LOCALS; ENTER(); PUSH(g[ESI]); g[ESI] = g[ECX]; g[EAX] = rd32(g[ESI] + 12) + 5u; POP(ESI); RET(0); }

/* Host-to-guest adapters: the struct is the state outside guest code. */
#if PROTO == 0
void N(enter_A)(cpu_t *c) { N(A)(c); }
void N(enter_D)(cpu_t *c) { N(D)(c); }
#elif PROTO == 1
/* This unit never allocates the pinned registers, so its prologues do not save
 * them: the adapter saves the host's values of exactly those by hand (the
 * others may hold this unit's own values), loads the pinned registers from the
 * struct, runs guest code, stores them back and restores the host's values. */
#define HSAVE(i, x) if (PIN(i)) __asm__ __volatile__("str " #x ", [%0, #" #i "*8]" : : "r"(saved) : "memory");
#define HLOAD(i, x) if (PIN(i)) __asm__ __volatile__("ldr " #x ", [%0, #" #i "*8]" : : "r"(saved) : "memory");
#define HOST_REGS(X) X(0, x20) X(1, x21) X(2, x22) X(3, x23) X(4, x24) X(5, x25) X(6, x26) X(7, x27)
#define ADAPTER(entry, fn) \
void N(entry)(cpu_t *c) { \
    uint64_t saved[8]; \
    HOST_REGS(HSAVE) \
    { LOCALS; FOR_REGS(LOAD_STRUCT) FOR_REGS(STORE_ONE) fn(c); FOR_REGS(LOAD_ONE) FOR_REGS(STORE_STRUCT) } \
    HOST_REGS(HLOAD) \
}
ADAPTER(enter_A, N(A))
ADAPTER(enter_D, N(D))
#else
#define ADAPTER(entry, fn) \
void N(entry)(cpu_t *c) { LOCALS; FOR_REGS(LOAD_STRUCT) r4 out = {{0}}; FOR_REGS(STORE_OUT) out = fn(c, out); FOR_REGS(LOAD_OUT) FOR_REGS(STORE_STRUCT) }
ADAPTER(enter_A, N(A))
ADAPTER(enter_D, N(D))
#endif
