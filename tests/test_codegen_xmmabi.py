"""Whole-program XMM contracts: XMM locals sync only what callees and callers read."""
import re
import shutil
import subprocess
import unittest
import test_codegen_flags as common

lift = common.lift


def function(address, *instructions):
    """One guest instruction per line, each with the address comment the lifter emits."""
    lines = [f'void L_{address:08X}(CPU *c)', '{']
    for i, text in enumerate(instructions):
        lines.extend([f'    AC1_SET_GUEST_EIP(c, 0x{address+i:08X}u);', f'    {text} /* {address+i:08X}: x */'])
    return '\n'.join(lines + ['}'])


def call(target):
    return f'AC1_CALL(c, L_{target:08X}, 0x{target:08X}u);'


def emitted(bodies, address, pinned=()):
    contracts, report = lift.xmm_contract_program(bodies, pinned)
    return lift.local_xmm_function(bodies[address], contracts.get(address)), contracts, report


def around_call(body):
    match = re.search(r'\{ ([^{}]*?)AC1_CALL\([^;]*;([^{}]*)\}', body)
    return re.findall(r'c->xmm\[(\d)\] = _ac1_xmm', match[1]), re.findall(r'_ac1_xmm(\d) = c->xmm', match[2])


class XmmContractTests(unittest.TestCase):
    def test_call_to_xmm_free_callee_syncs_nothing(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', call(0x401100),
                          'wrxm(c->ecx, c->xmm[0]);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->eax = rd32(c->esp);', 'c->esp += 4; return;')
        body, contracts, _ = emitted({0x401000: caller, 0x401100: callee}, 0x401000)
        # The callee never touches xmm0, so the caller's value survives in its
        # local; the callee passes it through, so it is stored and reloaded.
        self.assertEqual(contracts[0x401100]['entry'], 1)
        self.assertEqual(around_call(body), (['0'], ['0']))

    def test_dead_after_call_is_neither_stored_nor_reloaded(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', 'wrxm(c->ecx, c->xmm[0]);', call(0x401100),
                          'c->esp += 4; return;')
        callee = function(0x401100, 'c->xmm[0] = rdxm(c->esp);', 'wrxm(c->edx, c->xmm[0]);', 'c->esp += 4; return;')
        body, contracts, _ = emitted({0x401000: caller, 0x401100: callee}, 0x401000)
        self.assertEqual((contracts[0x401100]['entry'], contracts[0x401100]['ret']), (0, 0))
        self.assertIsNone(re.search(r'\{ [^{}]*AC1_CALL', body) and re.search(r'c->xmm\[0\] = _ac1_xmm0; AC1_CALL', body))
        self.assertEqual(around_call(body), ([], []))
        # Nothing is live at entry: the local starts from zero, not the struct.
        self.assertIn('_ac1_xmm0 __attribute__((unused)) = {{0}}', body)

    def test_callee_result_read_by_caller_joins_return_demand(self):
        caller = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[1]);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->xmm[1] = rdxm(c->esp);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: caller, 0x401100: callee})
        self.assertEqual((contracts[0x401100]['entry'], contracts[0x401100]['ret']), (0, 2))
        callee_body = lift.local_xmm_function(callee, contracts[0x401100])
        self.assertIn('{ c->xmm[1] = _ac1_xmm1; return; }', callee_body)

    def test_lane_writes_read_the_register(self):
        callee = function(0x401100, 'c->xmm[2].vf32 = ac1_lane_vf32(c->xmm[2].vf32, 0, 1.0f);',
                          'wrxm(c->edx, c->xmm[2]);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401100: callee})
        self.assertEqual(contracts[0x401100]['entry'], 4)

    def test_guarded_writes_do_not_kill(self):
        callee = function(0x401100, 'if (c->eax) c->xmm[3] = rdxm(c->esp);', 'wrxm(c->edx, c->xmm[3]);',
                          'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401100: callee})
        self.assertEqual(contracts[0x401100]['entry'], 8)

    def test_indirect_calls_use_every_possible_target(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', '{ uint32_t _ct = c->edx; dispatch(c, _ct); }',
                          'wrxm(c->ecx, c->xmm[0]);', 'c->esp += 4; return;')
        target = function(0x401100, 'wrxm(c->edx, c->xmm[5]);', 'c->esp += 4; return;')
        contracts, report = lift.xmm_contract_program({0x401000: caller, 0x401100: target},
                                                      call_targets={0x401100}, jump_targets={0x401100})
        # The target reads xmm5; xmm0, read after the call, is not its concern:
        # the caller stores it before the call and it survives in the struct.
        self.assertEqual(report['indirect_entry_demand'], 0x20)
        self.assertEqual(report['indirect_return_demand'], 1)
        self.assertEqual(contracts[0x401100]['ret'], 0)
        site = contracts[0x401000]['sites'][0x401001]
        self.assertEqual(site[:2], (0x21, 1))

    def test_indirect_targets_write_back_only_what_they_modify(self):
        caller = function(0x401000, '{ uint32_t _ct = c->edx; dispatch(c, _ct); }', 'wrxm(c->ecx, c->xmm[0]);',
                          'c->esp += 4; return;')
        writer = function(0x401100, 'c->xmm[0] = rdxm(c->esp);', 'c->xmm[3] = rdxm(c->esp);', 'c->esp += 4; return;')
        reader = function(0x401200, 'wrxm(c->edx, c->xmm[2]);', 'c->esp += 4; return;')
        contracts, report = lift.xmm_contract_program({0x401000: caller, 0x401100: writer, 0x401200: reader},
                                                      call_targets={0x401100, 0x401200}, jump_targets=set())
        self.assertEqual(contracts[0x401100]['ret'], 1)            # modified and read after an indirect call
        self.assertEqual((contracts[0x401200]['entry'], contracts[0x401200]['ret']), (4, 0))
        self.assertEqual(report['indirect_entry_demand'], 4)

    def test_indirect_jump_stores_target_and_return_demand(self):
        top = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[1]);', 'c->esp += 4; return;')
        thunk = function(0x401100, 'c->xmm[1] = rdxm(c->eax);', '{ uint32_t _ct = c->edx; dispatch_jmp(c, _ct); return; }')
        target = function(0x401200, 'wrxm(c->edx, c->xmm[6]);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: top, 0x401100: thunk, 0x401200: target},
                                                 call_targets=set(), jump_targets={0x401200})
        self.assertEqual(contracts[0x401100]['ret'], 2)
        self.assertEqual(contracts[0x401100]['sites'][0x401101][0], 0x42)
        body = lift.local_xmm_function(thunk, contracts[0x401100])
        self.assertIn('{ c->xmm[1] = _ac1_xmm1; dispatch_jmp(c, _ct);', body)   # xmm6 is untouched here
        self.assertNotIn('c->xmm[0]', body)

    def test_bound_functions_use_their_translated_body(self):
        # The native code is verified equal to the translated body, whose
        # analysis gives its contract; the bound body itself keeps a full sync.
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', call(0x401100), 'wrxm(c->ecx, c->xmm[0]);',
                          'c->esp += 4; return;')
        native = function(0x401100, 'c->esp += 4; return;')
        body, contracts, _ = emitted({0x401000: caller, 0x401100: native}, 0x401000, pinned={0x401100})
        self.assertNotIn(0x401100, contracts)
        self.assertEqual(around_call(body), (['0'], ['0']))   # passed through the callee
        caller2 = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', 'wrxm(c->ecx, c->xmm[0]);', call(0x401100),
                           'c->esp += 4; return;')
        body, _, _ = emitted({0x401000: caller2, 0x401100: native}, 0x401000, pinned={0x401100})
        self.assertEqual(around_call(body), ([], []))

    def test_multi_transfer_groups_read_their_callees_entry_demand(self):
        # Group of two calls: the second callee reads xmm2 at entry, which the
        # first plain call (to A, which writes xmm2) must therefore reload.
        caller = function(0x401000, call(0x401100),
                          'AC1_CALL(c, L_00401200, 0x00401200u); AC1_CALL(c, L_00401300, 0x00401300u);',
                          'c->esp += 4; return;')
        a = function(0x401100, 'c->xmm[2] = rdxm(c->esp);', 'c->esp += 4; return;')
        b = function(0x401200, 'wrxm(c->edx, c->xmm[2]);', 'c->esp += 4; return;')
        leaf = function(0x401300, 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: caller, 0x401100: a, 0x401200: b, 0x401300: leaf})
        self.assertEqual(contracts[0x401100]['ret'] & 4, 4)
        self.assertEqual(contracts[0x401000]['sites'][0x401000][1] & 4, 4)

    def test_pinned_translated_bodies_pass_demand_to_their_callees(self):
        # A region's translated fallback reads registers after its calls.
        region = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[6]);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->xmm[6] = rdxm(c->esp);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: region, 0x401100: callee}, pinned={0x401000})
        self.assertEqual(contracts[0x401100]['ret'], 0x40)

    def test_tail_call_reloads_and_returns_the_return_demand(self):
        top = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[4]);', 'c->esp += 4; return;')
        mid = function(0x401100, 'c->xmm[4] = rdxm(c->eax);', 'wrxm(c->ecx, c->xmm[4]);',
                       'AC1_CALL(c, L_00401200, 0x00401200u); return;')
        leaf = function(0x401200, 'c->xmm[4] = rdxm(c->esp);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: top, 0x401100: mid, 0x401200: leaf})
        self.assertEqual(contracts[0x401200]['ret'], 0x10)
        body = lift.local_xmm_function(mid, contracts[0x401100])
        self.assertIn('AC1_CALL(c, L_00401200, 0x00401200u); _ac1_xmm4 = c->xmm[4]; }', body)

    @unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
    def test_values_survive_indirect_calls_when_executed(self):
        """Compiled with contracts: a register the indirect callee leaves alone, one it
        writes, and one a direct callee of an XMM-free indirect target writes."""
        top = function(0x401000, 'c->xmm[0] = rdxm(c->esi);',
                       '{ uint32_t _ct = c->edx; dispatch(c, _ct); }',       # 0x401100: writes xmm1 only
                       'wrxm(c->ecx, c->xmm[0]);', 'wrxm(c->ecx + 16u, c->xmm[1]);',
                       '{ uint32_t _ct = c->edi; dispatch(c, _ct); }',       # 0x401200: reads xmm0, calls 0x401300
                       'wrxm(c->ecx + 32u, c->xmm[0]);', 'return;')
        t = function(0x401100, 'c->xmm[1] = rdxm(c->esi + 16u);', 'return;')
        v = function(0x401200, 'wrxm(c->ecx + 48u, c->xmm[0]);', call(0x401300), 'return;')
        g = function(0x401300, 'c->xmm[0] = rdxm(c->esi + 32u);', 'return;')
        bodies = {0x401000: top, 0x401100: t, 0x401200: v, 0x401300: g}
        contracts, _ = lift.xmm_contract_program(bodies, call_targets={0x401100, 0x401200}, jump_targets=set())
        generated = [lift.declare_local_memory_base(lift.local_xmm_function(
            lift.local_register_function(b), contracts[a])) for a, b in bodies.items()]
        self.assertTrue(all('_ac1_xmm' in g for g in generated[:3]))
        runtime = lift.prepare_test_runtime()
        work = common.ROOT/'work/xmm-contract-tests'
        work.mkdir(parents=True, exist_ok=True)
        source = ('#include "cpu.h"\n#include "ac1_local_memory.h"\n#define AC1_SET_GUEST_EIP(c, va) ((void)0)\n' +
                  ''.join(f'void L_{a:08X}(CPU *c);\n' for a in bodies) + '\n'.join(generated) + r"""
#include <stdio.h>
static unsigned char memory[0x10000];
uint8_t *ac1_direct_arena = memory;
void ac1_scheduler_return_checkpoint(CPU *c) { (void)c; }
void dispatch(CPU *c, uint32_t t) {
    if (t == 0x401100u) L_00401100(c); else if (t == 0x401200u) L_00401200(c); else if (t == 0x401300u) L_00401300(c);
    else { printf("bad target %x\n", t); exit(2); }
}
int main(void) {
    CPU c; memset(&c, 0, sizeof c);
    c.esi = 0x100; c.ecx = 0x200; c.edx = 0x401100; c.edi = 0x401200;
    for (int i = 0; i < 48; ++i) memory[0x100 + i] = (unsigned char)(0x10 * (i / 16 + 1) + i % 16);
    L_00401000(&c);
    static const int from[4] = {0, 1, 2, 0};   /* P, S, Q, then P as 0x401200 saw it */
    int bad = 0;
    for (int k = 0; k < 4; ++k)
        if (memcmp(memory + 0x200 + 16 * k, memory + 0x100 + 16 * from[k], 16)) { printf("slot %d wrong\n", k); bad = 1; }
    return bad;
}
""")
        (work/'indirect.c').write_text(source)
        exe = work/'indirect.exe'
        build = subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-DAC1_DIRECT_MEMORY',
                                '-I', str(runtime), str(work/'indirect.c'), '-o', str(exe)],
                               capture_output=True, text=True, timeout=120)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_no_contract_keeps_the_full_sync(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', call(0x401100), 'wrxm(c->ecx, c->xmm[0]);',
                          'c->esp += 4; return;')
        self.assertEqual(lift.local_xmm_function(caller), lift.local_xmm_function(caller, None))
        self.assertEqual(around_call(lift.local_xmm_function(caller)), (['0'], ['0']))


if __name__ == '__main__':
    unittest.main()
