"""16-byte stack loads take the values of the 4-byte lane stores just before them."""
import shutil
import subprocess
import unittest
from unittest.mock import patch

import test_lift_direct_calls as direct

lift, ROOT = direct.lift, direct.ROOT

FIXTURE = [
    ('f30f11442410', 'movss [esp+0x10], xmm0'),
    ('f30f114c2414', 'movss [esp+0x14], xmm1'),
    ('f30f11542418', 'movss [esp+0x18], xmm2'),
    ('8944241c', 'mov [esp+0x1c], eax'),
    ('0f285c2410', 'movaps xmm3, [esp+0x10]: all four lanes forwarded'),
    ('c7442420 0000803f', 'mov dword [esp+0x20], 0x3f800000'),
    ('51', 'push ecx'),
    ('0f10642424', 'movups xmm4, [esp+0x24]: lane 0 forwarded across the push'),
    ('59', 'pop ecx'),
    ('8908', 'mov [eax], ecx: a heap store ends forwarding'),
    ('0f286c2410', 'movaps xmm5, [esp+0x10]: plain load'),
    ('f30f11742430', 'movss [esp+0x30], xmm6'),
    ('6689542432', 'mov [esp+0x32], dx: overlaps the lane'),
    ('0f597c2430', 'mulps xmm7, [esp+0x30]: plain load'),
    ('f30f11442440', 'movss [esp+0x40], xmm0'),
    ('0f104c243e', 'movups xmm1, [esp+0x3e]: misaligned, plain load'),
    ('f30f11542450', 'movss [esp+0x50], xmm2'),
    ('0f59542450', 'mulps xmm2, [esp+0x50]: a packed op takes the forwarded lane'),
    ('c3', 'ret'),
]


class StackLaneForwardingTests(unittest.TestCase):
    def lifted(self):
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        from lift32_cpu import Lifter
        raw = bytes.fromhex(''.join(code.replace(' ', '') for code, _ in FIXTURE))
        read = lambda va, n: raw[va - 0x401000:va - 0x401000 + n]
        lifter = Lifter('synthetic stack-lane fixture', 0x1000, read)
        instructions = list(lifter.md.disasm(raw, 0x401000))
        self.assertEqual(len(instructions), len(FIXTURE))
        spec = {'address': 0x401000, 'size': len(raw), 'blocks': [{'start': 0x401000, 'end': 0x401000 + len(raw),
                'instructions': [[i.address, i.size] for i in instructions]}]}
        forwarded = lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM)
        with patch.object(lift.StackLaneForwarding, 'forward', lambda self, insn, load, lines: lines):
            plain = lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM)
        return plain, forwarded

    def test_only_safe_loads_are_forwarded(self):
        plain, forwarded = self.lifted()
        self.assertNotIn('ac1_xmm_from_u32', plain)
        self.assertEqual(forwarded.count('ac1_xmm_from_u32('), 3)
        for va in ('0040102C', '0040103C', '00401047'):     # after a heap store, overlapped lane, misaligned
            line = next(l for l in forwarded.splitlines() if f'/* {va}:' in l and 'rdxm(' in l)
            self.assertNotIn('ac1_xmm_from_u32', line)
        self.assertIn('ac1_xmm_from_u32(_ac1_lane_0040101B, rd32((c->esp + 0x00000028u)), '
                      'rd32((c->esp + 0x0000002Cu)), rd32((c->esp + 0x00000030u)))', forwarded)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_forwarded_loads_match_plain_loads(self):
        plain, forwarded = self.lifted()
        source = '#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        source += plain.replace('void L_00401000(', 'void plain_L_00401000(', 1) + '\n'
        source += forwarded.replace('void L_00401000(', 'void forwarded_L_00401000(', 1) + '\n'
        work = ROOT / 'work/stack-lane-tests'
        work.mkdir(parents=True, exist_ok=True)
        path = work / 'lanes.c'
        path.write_text(source)
        exe = path.with_suffix('.exe')
        subprocess.run(['gcc', '-O2', '-std=gnu11', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off',
                        '-DAC1_DIRECT_MEMORY', '-I', str(lift.prepare_test_runtime()), str(path),
                        str(ROOT / 'tests/stack_lane_semantics.c'), '-o', str(exe)],
                       check=True, capture_output=True, timeout=60)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=30)
        self.assertIn('4096 forwarded stack-lane CPU and memory cases passed', run.stdout)


if __name__ == '__main__':
    unittest.main()
