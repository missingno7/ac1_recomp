"""Vector-member SSE lowering and the guest-EIP markers the lifter keeps."""
import shutil
import subprocess
import unittest

import test_lift_direct_calls as direct

lift, ROOT = direct.lift, direct.ROOT

PAIRS = ((0, 1), (2, 2), (5, 3), (7, 6))
REG_FORMS = {
    'addps': '0f58', 'subps': '0f5c', 'mulps': '0f59', 'divps': '0f5e', 'sqrtps': '0f51',
    'minps': '0f5d', 'maxps': '0f5f', 'addpd': '660f58', 'subpd': '660f5c', 'mulpd': '660f59',
    'divpd': '660f5e', 'sqrtpd': '660f51', 'minpd': '660f5d', 'maxpd': '660f5f',
    'andps': '0f54', 'andnps': '0f55', 'orps': '0f56', 'xorps': '0f57', 'andpd': '660f54',
    'andnpd': '660f55', 'orpd': '660f56', 'xorpd': '660f57', 'pand': '660fdb', 'pandn': '660fdf',
    'por': '660feb', 'pxor': '660fef', 'unpcklps': '0f14', 'unpckhps': '0f15', 'unpcklpd': '660f14',
    'unpckhpd': '660f15', 'movhlps': '0f12', 'movlhps': '0f16', 'cvtps2pd': '0f5a',
    'cvtpd2ps': '660f5a', 'cvtdq2ps': '0f5b', 'paddb': '660ffc', 'paddw': '660ffd',
    'paddd': '660ffe', 'paddq': '660fd4', 'psubb': '660ff8', 'psubw': '660ff9', 'psubd': '660ffa',
    'psubq': '660ffb', 'pcmpeqb': '660f74', 'pcmpeqw': '660f75', 'pcmpeqd': '660f76',
    'pcmpgtb': '660f64', 'pcmpgtw': '660f65', 'pcmpgtd': '660f66', 'psrlw': '660fd1',
    'psrld': '660fd2', 'psrlq': '660fd3', 'psraw': '660fe1', 'psrad': '660fe2', 'psllw': '660ff1',
    'pslld': '660ff2', 'psllq': '660ff3', 'punpcklbw': '660f60', 'punpcklwd': '660f61',
    'punpckldq': '660f62', 'punpcklqdq': '660f6c', 'punpckhbw': '660f68', 'punpckhwd': '660f69',
    'punpckhdq': '660f6a', 'punpckhqdq': '660f6d',
    # Scalar and lane forms the donor still emits: only their lanes are renamed.
    'addss': 'f30f58', 'subss': 'f30f5c', 'mulss': 'f30f59', 'divss': 'f30f5e', 'sqrtss': 'f30f51',
    'minss': 'f30f5d', 'maxss': 'f30f5f', 'addsd': 'f20f58', 'mulsd': 'f20f59', 'divsd': 'f20f5e',
    'sqrtsd': 'f20f51', 'cvtss2sd': 'f30f5a', 'cvtsd2ss': 'f20f5a', 'movss': 'f30f10',
    'movsd': 'f20f10', 'comiss': '0f2f', 'ucomiss': '0f2e', 'comisd': '660f2f', 'ucomisd': '660f2e',
    'cvttps2dq': 'f30f5b', 'cvtps2dq': '660f5b',
}
IMM_FORMS = {'shufps': ('0fc6', (0x00, 0x1B, 0x4E, 0xC9, 0xD2, 0xFF, 0xE4, 0x93)),
             'shufpd': ('660fc6', (0, 1, 2, 3)), 'pshufd': ('660f70', (0x00, 0x1B, 0x4E, 0xE4, 0x55)),
             'cmpps': ('0fc2', range(8)), 'cmppd': ('660fc2', range(8)),
             'cmpss': ('f30fc2', range(8)), 'cmpsd': ('f20fc2', range(8))}
# (opcode, /reg) for the immediate shift group.
SHIFT_IMM = {'psrlw': ('660f71', 2), 'psraw': ('660f71', 4), 'psllw': ('660f71', 6),
             'psrld': ('660f72', 2), 'psrad': ('660f72', 4), 'pslld': ('660f72', 6),
             'psrlq': ('660f73', 2), 'psrldq': ('660f73', 3), 'psllq': ('660f73', 6), 'pslldq': ('660f73', 7)}
SHIFT_COUNTS = (0, 1, 4, 7, 8, 12, 15, 16, 31, 32, 63, 64, 200)
# GPR forms: (opcode, xmm in the reg field?)
# Lanes where both inputs are NaN may take either payload (C commutes + and *).
COMMUTATIVE_FLOAT = {'addps': 32, 'mulps': 32, 'addpd': 64, 'mulpd': 64,
                     'addss': 32, 'mulss': 32, 'addsd': 64, 'mulsd': 64}
GPR_FORMS = {'cvttss2si': ('f30f2c', False), 'cvtsi2ss': ('f30f2a', True), 'cvtsi2sd': ('f20f2a', True),
             'movmskps': ('0f50', False), 'movd_to_xmm': ('660f6e', True), 'movd_from_xmm': ('660f7e', True)}


def encodings():
    for name, opcode in REG_FORMS.items():
        for d, s in PAIRS:
            yield name, opcode + f'{0xC0 | d << 3 | s:02x}'
    for name, (opcode, immediates) in IMM_FORMS.items():
        for d, s in PAIRS[:3]:
            for imm in immediates:
                yield name, opcode + f'{0xC0 | d << 3 | s:02x}{imm:02x}'
    for name, (opcode, reg) in SHIFT_IMM.items():
        for d in (0, 5):
            for count in SHIFT_COUNTS:
                yield name, opcode + f'{0xC0 | reg << 3 | d:02x}{count:02x}'
    for name, (opcode, xmm_reg) in GPR_FORMS.items():
        for x, g in ((0, 0), (3, 3), (6, 6)):
            yield name, opcode + f'{0xC0 | (x << 3 | g if xmm_reg else g << 3 | x):02x}'


class SSEVectorLoweringTests(unittest.TestCase):
    def translations(self):
        from lift32_cpu import Lifter
        lifter = Lifter('synthetic SSE fixture', 0x1000, lambda va, n: b'')
        cases = []
        for name, hexbytes in encodings():
            insn = next(lifter.md.disasm(bytes.fromhex(hexbytes), 0x401000))
            vector = lift.ac1_vector_sse_translation(lifter, insn)
            if insn.mnemonic in ('movhlps', 'movlhps'):   # AC1's own lane form, not the donor's
                d, s = (lifter._xi(op) for op in insn.operands)
                hi = insn.mnemonic == 'movhlps'
                reference = [f'{{ uint64_t _lane = c->xmm[{s}].u64[{int(hi)}]; c->xmm[{d}].u64[{int(not hi)}] = _lane; }}']
            else:
                reference = lifter.translate(insn, set())
            registers = [lifter._xi(op) for op in insn.operands[:2] if lifter._is_xmm(op)]
            cases.append((f'{insn.mnemonic} {insn.op_str}', reference, vector,
                          lift.xmm_vector_lanes('\n'.join(vector or reference)),
                          (COMMUTATIVE_FLOAT.get(insn.mnemonic, 0), *(registers + registers)[:2])))
        return cases

    def test_packed_forms_are_vector_expressions(self):
        for name, _reference, vector, lowered, _lanes in self.translations():
            with self.subTest(name=name):
                self.assertNotRegex(lowered, lift.XMM_ARRAY_LANE)
                mnemonic = name.split()[0]
                if mnemonic in REG_FORMS and not mnemonic.endswith(('ss', 'sd')) and mnemonic not in (
                        'comiss', 'comisd', 'ucomiss', 'ucomisd', 'cvttps2dq', 'cvtps2dq'):
                    self.assertIsNotNone(vector, name)
                    self.assertNotIn('for (', lowered)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_vector_lowering_matches_lane_lowering(self):
        source = ['#include "cpu.h"']
        table = []
        for index, (name, reference, _vector, lowered, (bits, d, s)) in enumerate(self.translations()):
            self.assertNotIn('RECOMP_TODO', '\n'.join(reference), name)
            source.append(f'static void ref_{index}(CPU *c) {{\n' + '\n'.join(reference) + '\n}')
            source.append(f'static void vec_{index}(CPU *c) {{\n' + lowered + '\n}')
            table.append(f'    {{ "{name}", ref_{index}, vec_{index}, {bits}u, {d}u, {s}u }},')
        source.append('typedef struct { const char *name; void (*ref)(CPU *); void (*vec)(CPU *); '
                      'unsigned nan_lane_bits, d, s; } sse_case;')
        source.append('const sse_case sse_cases[] = {\n' + '\n'.join(table) + '\n};')
        source.append(f'const unsigned sse_case_count = {len(table)}u;')
        work = ROOT / 'work/sse-vector-tests'
        work.mkdir(parents=True, exist_ok=True)
        path = work / 'cases.c'
        path.write_text('\n'.join(source) + '\n')
        exe = work / 'sse_vector.exe'
        subprocess.run(['gcc', '-O2', '-std=gnu11', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off',
                        '-I', str(lift.prepare_test_runtime()), str(path), str(ROOT / 'tests/sse_vector_semantics.c'),
                        '-lm', '-o', str(exe)], check=True, capture_output=True, timeout=120)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(f'{len(table)} SSE vector-lowering cases x 4096 states match the lane lowering', run.stdout)

    def test_eip_is_recorded_at_entries_transfers_and_after_calls(self):
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        from lift32_cpu import Lifter
        # push ebp; mov ebp,esp; mov eax,[ebp+8]; mov eax,[eax]; test eax,eax; je next; div ecx; call next;
        # mov [esp+4],eax; mov ecx,fs:[0]; pop ebp; ret
        raw = bytes.fromhex('558bec8b45088b0085c07400f7f1e80000000089442404648b0d000000005dc3')
        read = lambda va, n: raw[va - 0x401000:va - 0x401000 + n]
        lifter = Lifter('synthetic EIP fixture', 0x1000, read)
        instructions = list(lifter.md.disasm(raw, 0x401000))
        spec = {'address': 0x401000, 'size': len(raw), 'blocks': [{'start': 0x401000, 'end': 0x401000 + len(raw),
                'instructions': [[i.address, i.size] for i in instructions]}]}
        body = lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM)
        recorded = {0x401000, 0x40100A, 0x40100C, 0x40100E, 0x401013, 0x40101F}   # entry, je, div, call, after call, ret
        self.assertEqual([i.address for i in instructions if i.mnemonic in ('je', 'div', 'call', 'ret')],
                         [0x40100A, 0x40100C, 0x40100E, 0x40101F])
        for insn in instructions:
            marker = f'    AC1_SET_GUEST_EIP(c, 0x{insn.address:08X}u);'
            with self.subTest(insn=f'{insn.mnemonic} {insn.op_str}'):
                tag = '' if insn.address in recorded else lift.UNRECORDED_EIP_TAG
                self.assertIn(marker + tag + '\n', body + '\n')
        dropped = lift.drop_unrecorded_eip_markers(body)
        self.assertEqual(dropped.count('AC1_SET_GUEST_EIP'), len(recorded))
        self.assertNotIn(lift.UNRECORDED_EIP_TAG, dropped)


if __name__ == '__main__':
    unittest.main()
