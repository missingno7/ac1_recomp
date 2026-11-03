"""Loop fences: every branch back to an equal or lower address keeps guest loads in the loop."""
import shutil
import subprocess
import unittest
import test_codegen_flags as common

lift = common.lift


def polling_body():
    """`L: mov eax,[ecx]; test eax,eax; je L; ret` as the lifter emits it, with address comments.

    Flags live in the function-local `_ac1_f`, as after the flag contracts: with
    no CPU-struct store in the loop, nothing but the fence keeps the load there."""
    return '\n'.join([
        'void L_00401000(CPU *c)', '{',
        '    ac1_flags _ac1_f = {0};',
        'L_00401000:',
        '    AC1_SET_GUEST_EIP(c, 0x00401000u);',
        '    c->eax = (uint32_t)(rd32((c->ecx))); /* 00401000: mov eax, dword ptr [ecx] */',
        '    AC1_SET_GUEST_EIP(c, 0x00401002u);',
        '    flags_logicz_m_l(&_ac1_f, c->eax & c->eax, 4, 0x08u); /* 00401002: test eax, eax */',
        '    AC1_SET_GUEST_EIP(c, 0x00401004u);',
        '    if (_ac1_f.zf) goto L_00401000; /* 00401004: je 0x401000 */',
        '    AC1_SET_GUEST_EIP(c, 0x00401006u);',
        '    c->esp += 4; return; /* 00401006: ret */',
        '}'])


class LoopFenceTests(unittest.TestCase):
    def test_backward_branches_are_fenced_forward_ones_are_not(self):
        body = '\n'.join([
            'void L_00401000(CPU *c)', '{',
            'L_00401000:',
            '    c->eax = rd32(c->ecx); /* 00401000: mov eax, [ecx] */',
            '    if (c->eax) goto L_00401010; /* 00401002: jne 0x401010 */',
            '    if (!c->zf) goto L_00401000; /* 00401004: jne 0x401000 */',
            '    goto L_00401006; /* 00401006: jmp 0x401006 */',
            'L_00401010:', '    return;', '}'])
        out = lift.fence_backward_branches(body).splitlines()
        self.assertEqual(out[4], body.splitlines()[4])
        self.assertIn('{ ' + lift.LOOP_FENCE + 'goto L_00401000; }', out[5])
        self.assertIn('{ ' + lift.LOOP_FENCE + 'goto L_00401006; }', out[6])

    def test_already_fenced_lines_are_unchanged(self):
        line = '    { ' + lift.LOOP_FENCE + 'goto L_00401000; } /* 00401004: jmp 0x401000 */'
        body = '\n'.join(['void L_00401000(CPU *c)', '{', line, '}'])
        self.assertEqual(lift.fence_backward_branches(body), body)

    def test_backward_mode_drops_only_forward_fences(self):
        body = '\n'.join([
            'void L_00401000(CPU *c)', '{',
            'L_00401000:',
            '    AC1_SET_GUEST_EIP(c, 0x00401000u);',
            '    c->eax = rd32(c->ecx); /* 00401000: mov eax, [ecx] */',
            '    AC1_SET_GUEST_EIP(c, 0x00401002u);',
            '    if (c->eax) goto L_00401008; /* 00401002: jne 0x401008 */',
            '    AC1_SET_GUEST_EIP(c, 0x00401004u);',
            '    goto L_00401000; /* 00401004: jmp 0x401000 */',
            'L_00401008:',
            '    AC1_SET_GUEST_EIP(c, 0x00401008u);',
            '    c->esp += 4; return; /* 00401008: ret */', '}'])
        everywhere = lift.local_register_function(body, 'all')
        backward = lift.fence_backward_branches(lift.local_register_function(body, 'backward'))
        self.assertEqual(everywhere.count(lift.LOOP_FENCE), 2)
        self.assertEqual(backward.count(lift.LOOP_FENCE), 1)
        fenced = [line for line in backward.splitlines() if lift.LOOP_FENCE in line]
        self.assertIn('goto L_00401000;', fenced[0])

    @unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
    def test_polling_loop_sees_a_store_from_another_thread(self):
        """The fenced backward branch keeps the load in the loop at -O2."""
        runtime = lift.prepare_test_runtime()
        work = common.ROOT/'work/fence-tests'
        work.mkdir(parents=True, exist_ok=True)
        generated = lift.declare_local_memory_base(
            lift.fence_backward_branches(lift.local_register_function(polling_body(), 'backward')))
        self.assertIn(lift.LOOP_FENCE, generated)
        source = ('#include "cpu.h"\n#include "ac1_local_memory.h"\n#define AC1_SET_GUEST_EIP(c, va) ((void)0)\n' +
                  generated + r'''
#include <pthread.h>
#include <stdio.h>
#include <time.h>
static unsigned char memory[0x10000];
uint8_t *ac1_direct_arena = memory;
static void *writer(void *p) {
    struct timespec t = {0, 50000000}; nanosleep(&t, NULL);
    __atomic_store_n((uint32_t *)(memory + 0x100), 7u, __ATOMIC_RELAXED);
    return p;
}
int main(void) {
    CPU c; memset(&c, 0, sizeof c); c.ecx = 0x100; c.esp = 0x8000;
    pthread_t t; pthread_create(&t, NULL, writer, NULL);
    L_00401000(&c);
    pthread_join(t, NULL);
    printf("polled value %u\n", c.eax);
    return c.eax != 7;
}
''')
        (work/'polling.c').write_text(source)
        exe = work/'polling.exe'
        subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-DAC1_DIRECT_MEMORY',
                        '-I', str(runtime), str(work/'polling.c'), '-lpthread', '-o', str(exe)],
                       check=True, capture_output=True, timeout=120)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('polled value 7', run.stdout)


if __name__ == '__main__':
    unittest.main()
