"""Whole-program flag contracts: one body per function, only demanded flags."""
import re
import shutil
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import test_codegen_flags as common

lift = common.lift


def function(address, *instructions):
    return common.function(*instructions).replace('void L_00401000(', f'void L_{address:08X}(')


def call(target, ret):
    return f'push32(c, 0x{ret:08X}u); AC1_CALL(c, L_{target:08X}, 0x{target:08X}u);'


def masks(body):
    return re.findall(r'flags_\w+_m\(.*?, (0x[0-9A-F]+)u\)', body)


def compile_and_run(source, name, *flags):
    work = common.ROOT/'work/flagabi-tests'
    work.mkdir(parents=True, exist_ok=True)
    (work/f'{name}.c').write_text(source)
    executable = work/f'{name}.exe'
    subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', *flags,
                    '-I', str(lift.prepare_test_runtime()), str(work/f'{name}.c'),
                    *[str(common.ROOT/'tests'/extra) for extra in ('flag_abi_semantics.c',) if name.startswith('program')],
                    '-o', str(executable)], check=True, capture_output=True, timeout=120)
    return subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)


def fake_pe(code):
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    image = bytearray(0x2000)
    specs, md = [], Cs(CS_ARCH_X86, CS_MODE_32)
    for address, raw in code.items():
        image[address-0x400000:address-0x400000+len(raw)] = raw
        instructions = list(md.disasm(raw, address))
        specs.append(dict(address=address, size=len(raw), instruction_count=len(instructions),
                          blocks=[dict(start=i.address, end=i.address+i.size,
                                       instructions=[[i.address, i.size]]) for i in instructions]))
    fake = SimpleNamespace(OPTIONAL_HEADER=SimpleNamespace(ImageBase=0x400000, SizeOfImage=0x3000,
                           DATA_DIRECTORY=[SimpleNamespace(VirtualAddress=0)]*16),
                           get_data=lambda offset, size: bytes(image[offset:offset+size]), close=lambda: None)
    return patch.object(pefile, 'PE', return_value=fake), specs


class FlagContractTests(unittest.TestCase):
    def test_callers_demand_only_the_flags_they_read(self):
        reader = function(0x401000, call(0x401100, 0x401001), 'if (c->cf) c->ebx ^= 1u;', 'c->esp += 4; return;')
        ignorer = function(0x401200, call(0x401100, 0x401001), 'c->esp += 4; return;')
        callee = function(0x401100, 'c->eax = flags_sub(c, c->eax, c->edx, 4);', 'c->esp += 4; return;')
        out, report = lift.flag_contract_program({0x401000: reader, 0x401100: callee, 0x401200: ignorer})
        self.assertEqual(masks(out[0x401100]), ['0x01'])
        self.assertEqual(report['return_demand_functions'], 1)
        out, _ = lift.flag_contract_program({0x401100: callee, 0x401200: ignorer})
        self.assertEqual(masks(out[0x401100]), ['0x00'])

    def test_entry_demand_is_computed_by_callers(self):
        caller = function(0x401000, 'flags_sub(c, c->eax, 5, 4);', call(0x401100, 0x401002), 'c->esp += 4; return;')
        callee = function(0x401100, 'if (c->zf) c->eax = 0u;', 'c->esp += 4; return;')
        out, report = lift.flag_contract_program({0x401000: caller, 0x401100: callee})
        self.assertEqual(masks(out[0x401000]), ['0x08'])
        self.assertEqual(report['entry_demand_functions'], 1)

    def test_tail_calls_return_the_callee_flags(self):
        top = function(0x401000, call(0x401100, 0x401001), 'if (c->zf) c->ebx = 1u;', 'c->esp += 4; return;')
        mid = function(0x401100, 'c->eax = flags_add(c, c->eax, 1, 4);',
                       'AC1_CALL(c, L_00401200, 0x00401200u); return;')
        leaf = function(0x401200, 'c->eax = flags_sub(c, c->eax, 1, 4);', 'c->esp += 4; return;')
        out, _ = lift.flag_contract_program({0x401000: top, 0x401100: mid, 0x401200: leaf})
        self.assertEqual(masks(out[0x401100]), ['0x00'])
        self.assertEqual(masks(out[0x401200]), ['0x08'])
        # A guarded tail call neither kills the fallthrough demand nor reads ours.
        guarded = function(0x401100, 'c->eax = flags_add(c, c->eax, 1, 4);',
                           'if (c->cf) { AC1_CALL(c, L_00401200, 0x00401200u); return; }', 'c->esp += 4; return;')
        out, _ = lift.flag_contract_program({0x401000: top, 0x401100: guarded, 0x401200: leaf})
        self.assertEqual(masks(out[0x401100]), ['0x09'])
        self.assertEqual(masks(out[0x401200]), ['0x08'])

    def test_imports_and_native_entries_keep_the_incoming_flags(self):
        for transfer, pinned in (('push32(c, 0x00401002u); dispatch(c, 0x00500000u);', ()),
                                 (call(0x401100, 0x401002), {0x401100})):
            caller = function(0x401000, 'flags_sub(c, c->eax, 5, 4);', transfer,
                              'if (c->cf) c->eax = 0u;', 'c->esp += 4; return;')
            native = function(0x401100, 'c->eax = flags_add(c, c->eax, 1, 4);', 'c->esp += 4; return;')
            out, report = lift.flag_contract_program({0x401000: caller, 0x401100: native}, pinned)
            self.assertEqual(masks(out[0x401000]), ['0x01'], transfer)
        self.assertEqual(masks(out[0x401100]), ['0x3F'])   # conservative per-function masks
        self.assertEqual(report['native_contracts'], {'0x00401100': [0, 1]})

    def test_indirect_transfers_use_only_possible_targets(self):
        caller = function(0x401000, 'flags_sub(c, c->eax, 5, 4);',
                          '{ uint32_t _ct = c->eax; push32(c, 0x00401002u); dispatch(c, _ct); }',
                          'if (c->sf) c->ebx = 2u;', 'c->esp += 4; return;')
        taken = function(0x401100, 'if (c->zf) c->eax = 0u;', 'c->eax = flags_add(c, c->eax, 1, 4);',
                         'c->esp += 4; return;')
        private = function(0x401200, 'if (c->of) c->eax = 0u;', 'c->eax = flags_add(c, c->eax, 1, 4);',
                           'c->esp += 4; return;')
        bodies = {0x401000: caller, 0x401100: taken, 0x401200: private}
        out, report = lift.flag_contract_program(bodies, call_targets={0x401100}, jump_targets={0x401100})
        self.assertEqual([masks(out[a]) for a in sorted(bodies)], [['0x08'], ['0x10'], ['0x00']])
        self.assertEqual((report['indirect_entry_demand'], report['indirect_return_demand']), (0x08, 0x10))
        self.assertEqual(report['indirect_targets_with_entry_demand'], ['0x00401100'])
        out, _ = lift.flag_contract_program(bodies)   # by default any entry is a target
        self.assertEqual([masks(out[a]) for a in sorted(bodies)], [['0x28'], ['0x10'], ['0x10']])

    def test_indirect_jumps_pass_on_return_demand(self):
        top = function(0x401000, call(0x401100, 0x401001), 'if (c->cf) c->ebx = 1u;', 'c->esp += 4; return;')
        thunk = function(0x401100, 'flags_sub(c, c->eax, 1, 4);', 'dispatch_jmp(c, c->eax); return;')
        target = function(0x401200, 'if (c->pf) c->ecx = 1u;', 'c->eax = flags_add(c, c->eax, 1, 4);',
                          'c->esp += 4; return;')
        out, report = lift.flag_contract_program({0x401000: top, 0x401100: thunk, 0x401200: target},
                                                 call_targets=set(), jump_targets={0x401200})
        self.assertEqual((masks(out[0x401100]), masks(out[0x401200])), (['0x02'], ['0x01']))
        self.assertEqual(report['indirect_jump_return_demand'], 0x01)

    def test_logical_operations_and_constant_shifts_define_flags(self):
        observe = 'push32(c, eflags_pack(c)); c->edi = pop32(c);'
        body = function(0x401000, 'c->eax = flags_add(c, c->eax, 1, 4);', 'flags_logicz(c, c->eax & 4u, 4);',
                        observe, 'c->esp += 4; return;')
        out, _ = lift.flag_contract_program({0x401000: body})
        self.assertEqual(masks(out[0x401000]), ['0x00', '0x3F'])
        for shift, mask in (('op_shl(c, c->eax, 0x3u, 4)', '0x00'), ('op_shl(c, c->eax, 0x20u, 4)', '0x08'),
                            ('op_shl(c, c->eax, c->ecx, 4)', '0x08'), ('op_rol(c, c->eax, 0x3u, 4)', '0x08'),
                            ('op_shrd(c, c->eax, c->edx, 0x4u, 4)', '0x00'),
                            ('op_rcr(c, c->eax, 0x1u, 4)', '0x09')):
            body = function(0x401000, 'flags_sub(c, c->eax, 1, 4);', f'c->eax = {shift};',
                            'if (c->zf) c->ebx = 1u;', 'c->esp += 4; return;')
            out, _ = lift.flag_contract_program({0x401000: body})
            self.assertEqual(masks(out[0x401000]), [mask], shift)

    def test_unknown_helpers_and_unresolved_gotos_observe_every_flag(self):
        for observer in ('unknown(c);', 'goto L_00402000;'):
            body = function(0x401000, 'flags_sub(c, c->eax, 1, 4);', observer, 'c->esp += 4; return;')
            out, report = lift.flag_contract_program({0x401000: body})
            self.assertEqual(masks(out[0x401000]), ['0x3F'], observer)
            self.assertEqual(report['opaque_helpers'], {'unknown': 1} if observer == 'unknown(c);' else {})

    def test_x87_checks_do_not_guard_the_compare(self):
        # fcomi: the failing check stops the guest, so its return neither reads
        # our return demand nor makes the compare's flags conditional.
        top = function(0x401000, call(0x401100, 0x401001), 'if (c->zf) c->ebx = 1u;', 'c->esp += 4; return;')
        body = function(0x401100, 'flags_sub(c, c->eax, 1, 4);',
                        'if (!ac1_x87_check(c, 0x00401001u)) return; '
                        'fcompare_eflags(c, *fst(c, 0), *fst(c, 1)); fpop(c);',
                        'if (c->cf) c->ebx = 1u;', 'c->esp += 4; return;')
        out, _ = lift.flag_contract_program({0x401000: top, 0x401100: body})
        self.assertEqual(masks(out[0x401100]), ['0x00'])

    def test_diagnostic_stops_end_demand(self):
        # `call <noreturn>; int3`: the stop never returns, so nothing after it
        # (here a flag read) asks the callee for flags.
        caller = function(0x401000, call(0x401100, 0x401001),
                          'ac1_unsupported_instruction(0x00401001u, "guest INT3 breakpoint");',
                          'if (c->zf) c->eax = 0u;', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->eax = flags_sub(c, c->eax, 1, 4);', 'c->esp += 4; return;')
        out, _ = lift.flag_contract_program({0x401000: caller, 0x401100: callee})
        self.assertEqual(masks(out[0x401100]), ['0x00'])
        guarded = function(0x401000, 'flags_sub(c, c->eax, 1, 4);',
                           'if (c->eax) RECOMP_TODO(0x00401001, "x");', 'if (c->cf) c->ebx = 1u;', 'c->esp += 4; return;')
        out, _ = lift.flag_contract_program({0x401000: guarded})
        self.assertEqual(masks(out[0x401000]), ['0x01'])

    def test_recursion_reaches_a_fixed_point(self):
        body = function(0x401000, 'flags_sub(c, c->eax, 1, 4);', 'if (c->zf) goto L_00401004;',
                        call(0x401000, 0x401003), 'if (c->cf) c->ebx = 1u;', ('L_00401004', 'c->esp += 4; return;'))
        out, _ = lift.flag_contract_program({0x401000: body})
        self.assertEqual(masks(out[0x401000]), ['0x09'])

    def test_possible_targets_exclude_call_operands_return_addresses_and_interiors(self):
        bodies = {
            0x401000: function(0x401000, call(0x401100, 0x401200), 'c->eax = 0x00401300u;',
                               '{ uint32_t _ct = c->eax; push32(c, 0x00401400u); dispatch(c, _ct); }',
                               'dispatch(c, 0x00401500u); return;'),
            **{a: function(a, 'c->esp += 4; return;') for a in (0x401100, 0x401200, 0x401300, 0x401400, 0x401500)}}
        specs = [dict(address=0x401500, blocks=[dict(instructions=[[0x401500, 1], [0x401300, 1]])])]
        calls, jumps = lift.flag_indirect_targets(bodies, specs, stored={0x401100})
        self.assertEqual((calls, jumps), ({0x401100}, {0x401100, 0x401300}))

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_helper_tables_match_the_prepared_header(self):
        header = (lift.prepare_test_runtime()/'cpu.h').read_text(encoding='utf-8')
        definitions = {}   # every preprocessor variant of each helper
        for m in re.finditer(r'^(?:static|AC1_\w+_INLINE)\b[^\n;{(]*?\b(\w+)\s*\([^;{]*\)\s*\{', header, re.M):
            depth, end = 1, m.end()
            while depth:
                depth += (header[end] == '{') - (header[end] == '}'); end += 1
            definitions.setdefault(m[1], []).append(header[m.end():end-1])
        for m in re.finditer(r'^#define\s+(\w+)\([^)]*\)((?:.*\\\n)*.*)', header, re.M):
            definitions.setdefault(m[1], []).append(m[2])
        for name in lift.FLAG_TRANSPARENT_HELPERS - {'AC1_SET_GUEST_EIP'}:
            self.assertIn(name, definitions)
            seen, pending = set(), [name]
            while pending:
                current = pending.pop()
                if current in seen or current not in definitions: continue
                seen.add(current)
                for body in definitions[current]:
                    self.assertNotRegex(body, r'->\s*(?:cf|pf|af|zf|sf|of)\b|\bdispatch|\bAC1_CALL\b',
                                        f'{name} via {current}')
                    pending.extend(re.findall(r'\b(\w+)\s*\(', body))
        bits = lift.FLAG_BITS
        for name, declared in lift.FLAG_HELPER_READS.items():
            for body in definitions[name]:
                read = sum(bits[f] for f in set(re.findall(r'->\s*(cf|pf|af|zf|sf|of)\b(?!\s*=(?!=))', body)))
                self.assertEqual(read & ~declared, 0, name)
        # Every declared write is a function of the inputs, never of the prior flag.
        checks = []
        def check(declared, expression, reads=0):
            checks.append(f'CHECK(0x{declared:02X}u, 0x{reads:02X}u, {expression});')
        for helper, declared in lift.FLAG_HELPERS.items():
            for a, b in ((0, 0), (0x7F, 1), (0x80, 0x80), (0xFFFF, 1), (0x7FFFFFFF, 0xFFFFFFFF)):
                for size in (1, 2, 4):
                    arguments = f'0x{a:X}u, {size}' if helper in ('flags_incs', 'flags_decs', 'flags_logicz') else f'0x{a:X}u, 0x{b:X}u, {size}'
                    check(declared, f'{helper}(c, {arguments})', bits['cf'] if helper in ('flags_adc', 'flags_sbb') else 0)
        for a, b in (('1.0', '2.0'), ('2.0', '1.0'), ('1.0', '1.0'), ('NAN', '1.0')):
            check(lift.FLAG_HELPER_WRITES['sse_compare'], f'sse_compare(c, {a}, {b})')
            check(lift.FLAG_HELPER_WRITES['fcompare_eflags'], f'fcompare_eflags(c, {a}, {b})')
        for value in (0, 0x8D5, 0xFFF):
            check(lift.FLAG_HELPER_WRITES['eflags_unpack'], f'eflags_unpack(c, 0x{value:X}u)')
            check(lift.FLAG_HELPER_WRITES['do_sahf'], f'do_sahf(c, 0x{value & 0xFF:X}u)')
        for source in (0, 1, 0x80000000):
            for helper in ('op_bsf', 'op_bsr'):
                check(lift.FLAG_HELPER_WRITES[helper], f'{helper}(c, 5u, 0x{source:X}u, 4)')
        for index in (0, 5, 33, -7):
            for operation in range(4):
                check(lift.FLAG_HELPER_WRITES['bit_string_op'], f'bit_string_op(c, 0x40u, {index}, {operation})')
        for helper, position in lift.FLAG_COUNTED_HELPERS.items():
            for size in (1, 2, 4):
                for count in (0, 1, 3, 8, 9, 16, 17, 31, 32, 33):
                    for value in (0x1, 0x80, 0x8001, 0xC0000003):
                        arguments = ([f'0x{value:X}u', '0x5A5Au'] if position == 2 else [f'0x{value:X}u']) + [f'0x{count:X}u', str(size)]
                        expression = f'{helper}(c, {", ".join(arguments)})'
                        check(lift.flag_counted_writes(expression), expression, lift.FLAG_HELPER_READS.get(helper, 0))
        source = '\n'.join([
            '#include "cpu.h"', '#include <stdio.h>', 'static unsigned char memory[256];',
            'void guest_execution_read(uint32_t a, void *o, size_t n) { memcpy(o, memory + (a & 127u), n); }',
            'void guest_execution_write(uint32_t a, const void *v, size_t n) { memcpy(memory + (a & 127u), v, n); }',
            'static unsigned flags(const CPU *c) { return c->cf | c->pf << 1 | c->af << 2 | c->zf << 3 | c->sf << 4 | c->of << 5; }',
            'static void set(CPU *c, unsigned f) { c->cf = f & 1; c->pf = f >> 1 & 1; c->af = f >> 2 & 1; c->zf = f >> 3 & 1; c->sf = f >> 4 & 1; c->of = f >> 5 & 1; }',
            'static int failures, cases;',
            '#define CHECK(declared, reads, expression) do { for (unsigned in = 0; in < 2; ++in) { '
            'CPU first = {0}, second = {0}; unsigned kept = in ? (reads) : 0; set(&first, kept); set(&second, 0x3Fu & ~(reads) | kept); '
            'memset(memory, 0x5A, sizeof memory); { CPU *c = &first; (void)(expression); } '
            'memset(memory, 0x5A, sizeof memory); { CPU *c = &second; (void)(expression); } ++cases; '
            'if ((flags(&first) ^ flags(&second)) & (declared)) { printf("prior flag leaks: %s\\n", #expression); ++failures; } } } while (0)',
            'int main(void) {', *checks, 'printf("%d helper write cases\\n", cases); return failures != 0; }'])
        run = compile_and_run(source, 'helper_writes')
        self.assertEqual(run.returncode, 0, run.stdout)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_compiled_contracts_match_per_function_masks(self):
        bodies = {
            0x401000: function(0x401000,
                'c->eax = flags_add(c, c->eax, c->ebx, 4);',
                call(0x401500, 0x401002),
                call(0x401100, 0x401003),
                'if (c->cf) c->ebx ^= 0x12345678u;',
                'flags_sub(c, c->edx, c->esi, 4);',
                '{ uint32_t _ct = 0x00401200u; push32(c, 0x00401006u); dispatch(c, _ct); }',
                'if (c->pf) c->esi ^= 0x55u;',
                'flags_sub(c, c->eax, 7u, 4);',
                'push32(c, 0x00401009u); dispatch(c, 0x00500000u);',
                'if (c->zf) c->edx += 3u;',
                call(0x401400, 0x40100B),
                'if (c->sf) c->edi ^= 0x99u;',
                'c->eax = op_shl(c, c->eax, 0x3u, 4);',
                'if (c->cf) c->ebp ^= 1u;',
                'c->ecx = flags_logicz(c, c->ecx & c->eax, 4);',
                'push32(c, eflags_pack(c)); c->edi ^= pop32(c);',
                'c->esp += 4; return;'),
            0x401100: function(0x401100, 'c->eax = flags_sub(c, c->eax, c->edx, 4);', 'c->esp += 4; return;'),
            0x401200: function(0x401200, 'if (c->cf) c->edx ^= 9u;', 'c->edx = flags_add(c, c->edx, c->eax, 4);',
                               'c->esp += 4; return;'),
            0x401300: function(0x401300, 'c->esi = flags_sub(c, c->esi, c->ebx, 4);', 'c->esp += 4; return;'),
            0x401400: function(0x401400, 'c->eax = flags_add(c, c->eax, 1u, 4);',
                               'AC1_CALL(c, L_00401300, 0x00401300u); return;'),
            0x401500: function(0x401500, 'if (c->zf) c->esi ^= 0x77u;', 'c->ebx = flags_logicz(c, c->ebx | 1u, 4);',
                               'c->esp += 4; return;'),
        }
        call_targets, jump_targets = lift.flag_indirect_targets(bodies, [])
        self.assertEqual(call_targets, {0x401200})
        contracted, report = lift.flag_contract_program(bodies, (), call_targets, jump_targets)
        self.assertEqual(masks(contracted[0x401000]), ['0x08', '0x01', '0x09', '0x3F'])
        self.assertEqual([masks(contracted[a]) for a in (0x401100, 0x401200, 0x401300, 0x401400, 0x401500)],
                         [['0x01'], ['0x0A'], ['0x10'], ['0x00'], ['0x00']])
        variants = [('cfg', [lift.flag_liveness_function(b) for b in bodies.values()]),
                    ('abi', list(contracted.values())),
                    ('abi_local', [lift.local_register_function(b) for b in contracted.values()])]
        source = '#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        for variant, generated in variants:
            text = re.sub(r'\bL_[0-9A-F]{8}\b', lambda m: variant+'_'+m[0], '\n'.join(generated))
            source += ''.join(f'void {name}(CPU *);\n' for name in sorted(set(re.findall(r'void (\w+)\(CPU \*c\)', text))))
            source += text + '\n'
        source += 'void variant_dispatch(int v, CPU *c, uint32_t t) {\n    switch (t) {\n'
        for a in sorted(bodies):
            source += (f'    case 0x{a:08X}u: if (v == 2) abi_local_L_{a:08X}(c); '
                       f'else if (v) abi_L_{a:08X}(c); else cfg_L_{a:08X}(c); return;\n')
        source += '    case 0x00500000u: c->esp += 4; return;\n    }\n    abort();\n}\n'
        source += (f'const unsigned variant_entries = {len(bodies)};\n'
                   'void variant_entry(int v, CPU *c, unsigned e) {\n'
                   f'    static const uint32_t entries[] = {{{", ".join(f"0x{a:08X}u" for a in sorted(bodies))}}};\n'
                   '    c->esp -= 4; wr32(c->esp, 0x00400000u); variant_dispatch(v, c, entries[e]);\n}\n')
        for force_dispatch in (False, True):
            run = compile_and_run(source, f'program-{force_dispatch}', *(['-DAC1_NO_DIRECT_CALLS'] if force_dispatch else []))
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('4096 CFG/contract/local whole-program cases passed', run.stdout)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_real_emitter_keeps_one_public_body_per_function(self):
        pe, specs = fake_pe({0x401000: bytes.fromhex('e8fb00000083c400c3'), 0x401100: bytes.fromhex('83c001c3')})
        work = common.ROOT/'work/flagabi-emitter-tests'; work.mkdir(parents=True, exist_ok=True)
        with pe:
            result = lift.emit_lift(work/'synthetic.exe', lift.DUKE_DEFAULT, specs, work, 1, flag_liveness='abi')
        self.assertEqual(result['flag_abi']['functions'], 2)
        self.assertIn('{ 0x00401100u, L_00401100 }', (work/'initial_dispatch.c').read_text())
        sources = '\n'.join((work/name).read_text() for name in result['translation_units'])
        self.assertNotIn('ac1_abi_', sources)
        self.assertIn('flags_add_m(c, c->eax, 0x1u, 4, 0x00u)', sources)
        for name in result['translation_units']:
            subprocess.run(['gcc', '-O2', '-std=c11', '-Werror=implicit-function-declaration',
                            '-I', str(lift.prepare_test_runtime()), '-I', str(work), '-c', str(work/name),
                            '-o', str(work/(name+'.obj'))], check=True, capture_output=True, timeout=60)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_region_oracles_keep_per_function_masks_with_contracts_and_locals(self):
        pe, specs = fake_pe({0x401000: bytes.fromhex('e8fb000000eb0083c400c3'),
                             0x401100: bytes.fromhex('83c001c3'),
                             0x401200: bytes.fromhex('e8fbfdffff83c400c3')})
        region = {0x401000: dict(boundary='0x00401007', context_bytes=64,
                  members=[dict(address=hex(va)) for va in (0x401000, 0x401100)],
                  implementation='test_region', plan='test_prepare', pending='test_pending')}
        work = common.ROOT/'work/flagabi-region-tests'; work.mkdir(parents=True, exist_ok=True)
        with pe, patch.object(lift, 'validate_region_bindings', return_value=region):
            result = lift.emit_lift(work/'synthetic.exe', lift.DUKE_DEFAULT, specs, work, 1,
                                    flag_liveness='abi', local_regs='all')
        region_source = (work/'initial_region_00401000.c').read_text()
        self.assertNotIn('_ac1_regs', region_source)
        self.assertIn('ac1_region_00401000_m_00401100(c)', region_source)
        self.assertEqual(set(masks(region_source)), {'0x3F'})
        sources = '\n'.join((work/name).read_text() for name in result['translation_units'])
        self.assertIn('flags_add_m(c, _ac1_regs.eax, 0x1u, 4, 0x00u)', sources)
        self.assertEqual(result['flag_abi']['native_entries'], 1)
        runtime = lift.prepare_test_runtime()
        for direct in (False, True):
            for name in result['translation_units']:
                subprocess.run(['gcc', '-O2', '-std=c11', '-Werror=implicit-function-declaration',
                                *(['-DAC1_DIRECT_MEMORY'] if direct else []),
                                '-I', str(runtime), '-I', str(common.ROOT/'src/runtime'), '-I', str(work),
                                '-c', str(work/name), '-o', str(work/(name+'.obj'))],
                               check=True, capture_output=True, timeout=60)


if __name__ == '__main__': unittest.main()
