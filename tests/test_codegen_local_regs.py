"""GPR ownership, exact import-boundary regression, and existing ABI oracles."""
import re
import shutil
import subprocess
import unittest
from unittest.mock import patch

import test_lift_direct_calls as direct
import test_lift_register_switches as switches

lift, ROOT = direct.lift, direct.ROOT


class LocalRegisterTests(unittest.TestCase):
    def test_flags_helpers_do_not_access_gprs(self):
        header = (lift.prepare_test_runtime() / 'cpu.h').read_text()
        count = 0
        for match in re.finditer(r'\b(flags_\w+)\([^{};]*\)\s*\{', header):
            depth, end = 1, match.end()
            while depth:
                depth += (header[end] == '{') - (header[end] == '}')
                end += 1
            self.assertNotRegex(header[match.end():end], r'c->(?:eax|ecx|edx|ebx|esp|ebp|esi|edi)\b')
            count += 1
        self.assertGreaterEqual(count, 20)

    def test_per_function_selection_fails_closed(self):
        entries = {0x401000, 0x401100}
        self.assertEqual(lift.local_register_selection('off', entries), set())
        self.assertEqual(lift.local_register_selection('all', entries), entries)
        self.assertEqual(lift.local_register_selection('00401000,0x00401100', entries), entries)
        for value in ('401000', 'all,00401000', '00401001', '00401000,', '0040100g'):
            with self.assertRaises(ValueError):
                lift.local_register_selection(value, entries)

    def test_helpers_stops_and_taken_edges_are_synchronized(self):
        source = '\n'.join(['void L_00401000(CPU *c)', '{',
            '    AC1_SET_GUEST_EIP(c, 0x00401000u);',
            '    c->eax = flags_add_m(c, c->eax, 1u, 4, 0x3Fu);',
            '    AC1_SET_GUEST_EIP(c, 0x00401001u);',
            '    c->ebx = pop32(c); if (c->zf) goto L_00401000;',
            '    AC1_SET_GUEST_EIP(c, 0x00401002u);',
            '    RECOMP_TODO(0x401002, "unknown");', '    c->esp += 4; return;', '}'])
        body = lift.local_register_function(source)
        self.assertIn('_ac1_regs.eax = flags_add_m(c, _ac1_regs.eax', body)
        self.assertIn('c->ebx = pop32(c)', body)
        self.assertIn('memcpy(&_ac1_regs, &c->eax, sizeof _ac1_regs); __asm__', body)
        self.assertIn('memcpy(&c->eax, &_ac1_regs, sizeof _ac1_regs);\n    AC1_SET_GUEST_EIP(c, 0x00401002', body)
        for text in ('/* fs:[0] */', 'setjmp(c->opaque);', 'longjmp(c->opaque);'):
            unsafe = source.replace('    c->esp += 4; return;', '    ' + text + ' return;')
            self.assertEqual(lift.local_register_function(unsafe), unsafe)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_leading_comment_dispatch_consumes_current_cpu(self):
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        from lift32_cpu import Lifter
        # MOV ECX,10200; ADD ESP,8; MOV [ESP+4],ECX; JMP [10000].
        raw = bytes.fromhex('b90002010083c408894c2404ff2500000100')
        read = lambda va, size: raw[va-0x401000:va-0x401000+size]
        lifter = Lifter('synthetic import-tail fixture', 0x1000, read)
        instructions = list(lifter.md.disasm(raw, 0x401000))
        spec = {'address': 0x401000, 'size': len(raw), 'blocks': [
            {'start': 0x401000, 'end': 0x401000 + len(raw),
             'instructions': [[i.address, i.size] for i in instructions]}]}
        body = lift.lift_exact_function(lifter, spec, read, X86_OP_IMM, X86_OP_MEM)
        self.assertIn('/* indirect jmp */ dispatch_jmp(c,', body)
        work = ROOT / 'work/local-regs-tests'
        work.mkdir(parents=True, exist_ok=True)
        for flags in ('off', 'cfg'):
            source = work / f'tail-{flags}.c'
            source.write_text('#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n' +
                              lift.local_register_function(lift.flag_liveness_function(body, flags)))
            exe = source.with_suffix('.exe')
            subprocess.run(['gcc', '-std=c11', '-O2', '-fwrapv', '-fno-strict-aliasing',
                            '-I', str(lift.prepare_test_runtime()), str(source),
                            str(ROOT / 'tests/codegen_local_regs_semantics.c'), '-o', str(exe)],
                           check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=15)
            self.assertIn('1000 leading-comment import tails', result.stdout)

    def test_generated_direct_transfers_with_local_registers(self):
        original = lift.specialize_direct_calls
        def transform(*args):
            body, callees, count = original(*args)
            return lift.local_register_function(lift.flag_liveness_function(body)), callees, count
        with patch.object(lift, 'specialize_direct_calls', side_effect=transform):
            direct.DirectCallsTests('test_generated_calls_and_tail_transfers').test_generated_calls_and_tail_transfers()

    def test_register_switch_execution_with_local_registers(self):
        original = lift.lift_exact_function
        with patch.object(lift, 'lift_exact_function', side_effect=lambda *a, **k:
                          lift.local_register_function(lift.flag_liveness_function(original(*a, **k)))):
            switches.RegisterSwitchTests().test_compile_execute_local_cases_default_ret8_and_unknown_fallback()


if __name__ == '__main__':
    unittest.main()
