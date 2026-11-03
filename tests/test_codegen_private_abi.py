"""Private call ABI: guest registers passed in host registers between translated functions."""
import shutil
import subprocess
import sys
import unittest
import test_codegen_flags as common

lift = common.lift


def function(address, *instructions):
    lines = [f'void L_{address:08X}(CPU *c)', '{']
    for va, text in instructions:
        if text.endswith(':'):
            lines.append(text); continue
        lines.extend([f'    AC1_SET_GUEST_EIP(c, 0x{va:08X}u);', f'    {text} /* {va:08X}: x */'])
    return '\n'.join(lines + ['}'])


def push(value):
    return f'{{ uint32_t _ac1_push = {value}; c->esp -= 4u; wr32(c->esp, _ac1_push); }}'


def program():
    # A: push esi; mov esi, ecx; push eax; call B; mov ebx, [esi+4]; add ebx, eax; push ebx;
    #    call B; call edx (C, right after a private call: the struct must be exact for it);
    #    xor eax, ebx; pop esi; ret
    a = function(0x401000,
                 (0x401000, push('c->esi')), (0x401001, 'c->esi = (uint32_t)(c->ecx);'), (0x401003, push('c->eax')),
                 (0x401004, push('0x00401009u') + ' AC1_CALL(c, L_00401100, 0x00401100u);'),
                 (0x401009, 'c->ebx = (uint32_t)(rd32((c->esi + 0x00000004u)));'),
                 (0x40100C, 'c->ebx = c->ebx + c->eax;'), (0x40100E, push('c->ebx')),
                 (0x40100F, push('0x00401014u') + ' AC1_CALL(c, L_00401100, 0x00401100u);'),
                 (0x401014, '{ uint32_t _ct = c->edx; ' + push('0x00401016u') + ' dispatch(c, _ct); }'),
                 (0x401016, 'c->eax = c->eax ^ c->ebx;'),
                 (0x401018, '{ uint32_t _ac1_pop = rd32(c->esp); c->esp += 4u; c->esi = _ac1_pop; }'),
                 (0x401019, 'c->esp += 4; return;'))
    # B (stdcall, 1 argument): push esi; mov esi, [esp+8]; mov eax, esi; add eax, [ecx+8];
    #    test al, 1; je skip; call edx (C, through the dispatcher); skip: pop esi; ret 4
    b = function(0x401100,
                 (0x401100, push('c->esi')), (0x401101, 'c->esi = (uint32_t)(rd32((c->esp + 0x00000008u)));'),
                 (0x401105, 'c->eax = (uint32_t)(c->esi);'),
                 (0x401107, 'c->eax = c->eax + rd32((c->ecx + 0x00000008u));'),
                 (0x40110A, 'if (!(c->eax & 1u)) goto L_00401114;'),
                 (0x40110C, '{ uint32_t _ct = c->edx; ' + push('0x00401114u') + ' dispatch(c, _ct); }'),
                 (0, 'L_00401114:'),
                 (0x401114, '{ uint32_t _ac1_pop = rd32(c->esp); c->esp += 4u; c->esi = _ac1_pop; }'),
                 (0x401115, 'c->esp += 8; return;'))
    # C: lea eax, [eax+1]; mov [ecx+0xc], eax; ret
    cc = function(0x401200, (0x401200, 'c->eax = c->eax + 1u;'), (0x401203, 'wr32((c->ecx + 0x0000000Cu), c->eax);'),
                  (0x401206, 'c->esp += 4; return;'))
    return {0x401000: a, 0x401100: b, 0x401200: cc}


def lowered(mask):
    bodies = program()
    private_set = set(bodies) if mask else set()
    out = []
    for address, body in bodies.items():
        private = dict(mask=mask, callees=private_set, self=True) if mask else None
        text = lift.declare_local_memory_base(lift.local_register_function(body, 'all', None, private))
        out.append(lift.private_abi_finish(text, address, mask) if mask else text)
    protos = ''.join(f'void L_{a:08X}(CPU *c);\n' for a in bodies)
    if mask:
        protos += ''.join(f'AC1_PRIVATE ac1_pregs P_{a:08X}(CPU *, ' + ', '.join('uint32_t' for _ in mask) + ');\n'
                          for a in bodies)
    return protos + '\n'.join(out)


DRIVER = r'''
#include <stdio.h>
static unsigned char memory[0x10000];
uint8_t *ac1_direct_arena = memory;
static uint64_t hash = 1469598103934665603ull;
static void mix(const void *p, size_t n) { const unsigned char *b = p; while (n--) hash = (hash ^ *b++) * 1099511628211ull; }
static void mix_state(CPU *c) { uint32_t r[8] = {c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi}; mix(r, sizeof r); }
static unsigned checkpoints;
/* The runtime observes the struct here: it must be exact. */
void ac1_scheduler_return_checkpoint(CPU *c) { ++checkpoints; mix_state(c); c->calls->returns = 0; }
void dispatch(CPU *c, uint32_t t) {
    mix_state(c);
    if (t == 0x401100u) L_00401100(c); else if (t == 0x401200u) L_00401200(c);
    else { printf("bad target %08x\n", t); exit(2); }
}
static uint32_t seed = 0x9E3779B9u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
int main(void) {
    for (unsigned k = 0; k < 3000; ++k) {
        ac1_call_state calls; memset(&calls, 0, sizeof calls);
        calls.stack_floor = 0; calls.return_threshold = k % 4;       /* 0: a checkpoint after every return */
        for (unsigned i = 0x1000; i < 0x3000; ++i) memory[i] = (unsigned char)rnd();
        CPU c; memset(&c, 0, sizeof c);
        c.eax = rnd(); c.ebx = rnd(); c.ebp = rnd(); c.esi = rnd(); c.edi = rnd();
        c.ecx = 0x1000u + (rnd() & 0xFF0u); c.edx = 0x401200u; c.esp = 0x8000u - 4u * (rnd() & 15u);
        c.calls = (k % 3 == 0) ? NULL : &calls;                    /* NULL: every call takes the dispatcher */
        L_00401000(&c);
        mix_state(&c); mix(memory, sizeof memory);
    }
    printf("%016llx %u\n", (unsigned long long)hash, checkpoints);
    return 0;
}
'''


@unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
class PrivateAbiTests(unittest.TestCase):
    def test_private_calls_pass_registers_and_keep_the_struct_adapter(self):
        text = lowered(['esp', 'esi'])
        self.assertIn('AC1_PCALL2(c, P_00401100, 0x00401100u, esp, esi);', text)
        self.assertIn('AC1_PRIVATE ac1_pregs P_00401100(CPU *c, uint32_t _ac1_p_esp, uint32_t _ac1_p_esi)', text)
        self.assertIn('void L_00401100(CPU *c)\n{\n    ac1_pregs _ac1_o = P_00401100(c, c->esp, c->esi);', text)
        self.assertIn('dispatch(c, _ct);', text)          # the indirect call keeps the ordinary path

    def run_variant(self, name, mask):
        runtime = lift.prepare_test_runtime()
        work = common.ROOT/'work/private-abi-tests'
        work.mkdir(parents=True, exist_ok=True)
        source = ('#include "cpu.h"\n#include "ac1_local_memory.h"\n#define AC1_SET_GUEST_EIP(c, va) ((void)0)\n'
                  + lowered(mask) + DRIVER)
        (work/f'{name}.c').write_text(source)
        exe = work/f'{name}.exe'
        build = subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-DAC1_DIRECT_MEMORY',
                                '-I', str(runtime), str(work/f'{name}.c'), '-o', str(exe)],
                               capture_output=True, text=True, timeout=120)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run.stdout.strip()

    def test_every_register_set_matches_the_struct_protocol(self):
        reference = self.run_variant('struct', [])
        self.assertNotIn(' 0', reference + ' ')        # checkpoints were exercised
        for mask in (['esp'], ['esp', 'esi'], ['ecx', 'esp', 'esi'], ['ecx', 'ebx', 'esp', 'esi']):
            with self.subTest(mask=mask):
                self.assertEqual(self.run_variant('private_' + '_'.join(mask), mask), reference)


if __name__ == '__main__':
    unittest.main()
