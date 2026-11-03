"""Value-speculative reloads of callee-saved GPR locals after guest calls."""
import shutil
import subprocess
import sys
import unittest
import test_codegen_flags as common

lift = common.lift
sys.path.insert(0, str(common.ROOT / 'tools'))
import function_facts as facts


def caller_body():
    """`mov esi, ecx; call 0x401100; mov eax, [esi+8]; ret` as the lifter emits it."""
    return '\n'.join([
        'void L_00401000(CPU *c)', '{',
        '    AC1_SET_GUEST_EIP(c, 0x00401000u);',
        '    c->esi = (uint32_t)(c->ecx); /* 00401000: mov esi, ecx */',
        '    AC1_SET_GUEST_EIP(c, 0x00401002u);',
        '    { uint32_t _ac1_push = 0x00401007u; c->esp -= 4u; wr32(c->esp, _ac1_push); } '
        'AC1_CALL(c, L_00401100, 0x00401100u); /* 00401002: call 0x401100 */',
        '    AC1_SET_GUEST_EIP(c, 0x00401007u);',
        '    c->eax = (uint32_t)(rd32((c->esi + 0x00000008u))); /* 00401007: mov eax, dword ptr [esi + 8] */',
        '    AC1_SET_GUEST_EIP(c, 0x0040100Au);',
        '    c->esp += 4; return; /* 0040100A: ret */',
        '}'])


ESI = 1 << 6


class ReloadSpeculationTests(unittest.TestCase):
    def test_liveness_after_calls(self):
        T = facts.Transfer
        body = [(0x401000, (), [
            (0x401000, 'mov', (0x02, ESI), None),                      # mov esi, ecx
            (0x401002, 'call', (0x10, 0x10), T('direct', 0x401100)),
            (0x401007, 'mov', (ESI, 0x01), None),                      # mov eax, [esi + 8]
            (0x40100A, 'call', (0x10, 0x10), T('indirect')),
            (0x40100C, 'mov', (0x00, ESI), None),                      # mov esi, 1: redefined
            (0x401011, 'ret', (0x10, 0x10), T('ret'))])]
        live = facts.gpr_live_after_calls(body)
        self.assertEqual(live[0x401002] & ESI, ESI)        # read before redefinition
        self.assertEqual(live[0x40100A] & ESI, 0)          # redefined: the reload is dead

    def test_only_live_callee_saved_registers_are_speculated(self):
        body = lift.local_register_function(caller_body(), 'all', {0x401002: ESI | 0x01})
        self.assertIn('__builtin_expect(_ac1_v != _ac1_regs.esi, 0)', body)
        self.assertNotIn('_ac1_v != _ac1_regs.eax', body)          # eax is not callee-saved
        self.assertEqual(lift.local_register_function(caller_body(), 'all', {0x401002: 0}),
                         lift.local_register_function(caller_body(), 'all'))

    @unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
    def test_speculation_is_exact_when_the_callee_keeps_or_changes_esi(self):
        runtime = lift.prepare_test_runtime()
        work = common.ROOT/'work/reload-speculation-tests'
        work.mkdir(parents=True, exist_ok=True)
        plain = lift.declare_local_memory_base(lift.local_register_function(caller_body(), 'all'))
        spec = lift.declare_local_memory_base(lift.local_register_function(caller_body(), 'all', {0x401002: ESI}))
        self.assertIn('_ac1_v != _ac1_regs.esi', spec)
        source = ('#include "cpu.h"\n#include "ac1_local_memory.h"\n#define AC1_SET_GUEST_EIP(c, va) ((void)0)\n'
                  'void L_00401100(CPU *c);\n' +
                  plain.replace('void L_00401000(', 'void plain_L_00401000(', 1) + '\n' +
                  spec.replace('void L_00401000(', 'void spec_L_00401000(', 1) + r'''
#include <stdio.h>
static unsigned char memory[0x10000];
uint8_t *ac1_direct_arena = memory;
static int callee_changes_esi;
void ac1_scheduler_return_checkpoint(CPU *c) { (void)c; }
/* push esi; mov esi, 0x5000; pop esi; ret   or   mov esi, edx; ret */
void L_00401100(CPU *c) {
    if (callee_changes_esi) { c->esi = c->edx; c->esp += 4; return; }
    c->esp -= 4; memcpy(memory + c->esp, &c->esi, 4); c->esi = 0x5000;
    memcpy(&c->esi, memory + c->esp, 4); c->esp += 4; c->esp += 4;
}
void dispatch(CPU *c, uint32_t t) { if (t != 0x401100u) { printf("bad target\n"); exit(2); } L_00401100(c); }
int main(void) {
    for (int changes = 0; changes < 2; ++changes) {
        CPU a, b; unsigned char ma[0x10000];
        callee_changes_esi = changes;
        for (int i = 0; i < 0x10000; ++i) memory[i] = (unsigned char)(i * 7 ^ i >> 8);
        memset(&a, 0, sizeof a); a.ecx = 0x1000; a.edx = 0x2000; a.esp = 0x8000; b = a;
        plain_L_00401000(&a); memcpy(ma, memory, sizeof ma);
        for (int i = 0; i < 0x10000; ++i) memory[i] = (unsigned char)(i * 7 ^ i >> 8);
        spec_L_00401000(&b);
        if (memcmp(&a, &b, sizeof a) || memcmp(ma, memory, sizeof ma)) { printf("differs (callee changes esi: %d)\n", changes); return 1; }
        printf("same state, callee changes esi: %d, eax %08x esi %08x\n", changes, b.eax, b.esi);
    }
    return 0;
}
''')
        (work/'speculation.c').write_text(source)
        exe = work/'speculation.exe'
        build = subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-DAC1_DIRECT_MEMORY',
                                '-I', str(runtime), str(work/'speculation.c'), '-o', str(exe)],
                               capture_output=True, text=True, timeout=120)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('callee changes esi: 1', run.stdout)


if __name__ == '__main__':
    unittest.main()
