/* Lab-only: guest calls without their cold paths, for comparing hot-path machine code.
 *
 * Force-included after cpu.h (-include cpu.h -include hot_path.h) by
 * tools/codegen_lab/hot_static.py. The dispatcher fallback and the scheduler
 * checkpoint are rare at run time but each call site carries their code, so
 * static counts of both call protocols include it; this header drops them
 * from AC1_CALL and AC1_PCALLn alike. Never used in a game build. */
#undef AC1_CALL
#define AC1_CALL(c, fn, target) fn(c)
#undef AC1_PCALL1
#undef AC1_PCALL2
#undef AC1_PCALL3
#undef AC1_PCALL4
#define AC1_PCALL1(c, fn, t, a0) do { ac1_pregs _o = fn((c), _ac1_regs.a0); _ac1_regs.a0 = (uint32_t)_o.r0; } while (0)
#define AC1_PCALL2(c, fn, t, a0, a1) do { ac1_pregs _o = fn((c), _ac1_regs.a0, _ac1_regs.a1); \
    _ac1_regs.a0 = (uint32_t)_o.r0; _ac1_regs.a1 = (uint32_t)_o.r1; } while (0)
#define AC1_PCALL3(c, fn, t, a0, a1, a2) do { ac1_pregs _o = fn((c), _ac1_regs.a0, _ac1_regs.a1, _ac1_regs.a2); \
    _ac1_regs.a0 = (uint32_t)_o.r0; _ac1_regs.a1 = (uint32_t)_o.r1; _ac1_regs.a2 = (uint32_t)_o.r2; } while (0)
#define AC1_PCALL4(c, fn, t, a0, a1, a2, a3) do { ac1_pregs _o = fn((c), _ac1_regs.a0, _ac1_regs.a1, _ac1_regs.a2, _ac1_regs.a3); \
    _ac1_regs.a0 = (uint32_t)_o.r0; _ac1_regs.a1 = (uint32_t)_o.r1; _ac1_regs.a2 = (uint32_t)_o.r2; _ac1_regs.a3 = (uint32_t)_o.r3; } while (0)
