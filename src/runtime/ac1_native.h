#ifndef AC1_NATIVE_H
#define AC1_NATIVE_H
#include "cpu.h"

/* A binding replaces a complete guest entry, including RET. Both implementations
 * receive the same CPU ABI. Plans must cover EVERY possible guest write, including
 * saved registers below ESP. Only audited leaf bodies may use the verifier: no
 * imports, callbacks, scheduling, host state or irreversible side effects. */
#define AC1_NATIVE_MAX_RANGES 8
typedef struct { uint32_t address; size_t size; } ac1_native_range;
typedef struct {
    ac1_native_range ranges[AC1_NATIVE_MAX_RANGES];
    unsigned count;
    const char *decline_reason;
    uint32_t decline_address;
} ac1_native_plan;
typedef struct {
    uint32_t va;
    const char *name;
    void (*body)(CPU *);
    int (*plan)(const CPU *, ac1_native_plan *);
    int enabled;
    uint64_t calls, checked, fallbacks;
} ac1_native_binding;

/* Called once per process run, before guest threads start / after they join. */
void ac1_native_init(void);
void ac1_native_shutdown(void);
void ac1_native_invoke(CPU *, ac1_native_binding *, void (*translated)(CPU *));
void ac1_native_add_range(ac1_native_plan *, uint32_t, size_t);

/* Implementations and their plans live in ac1_native_leaf.c. The lifter emits
 * bindings from config and checks the hash of every recovered instruction. */
#endif
