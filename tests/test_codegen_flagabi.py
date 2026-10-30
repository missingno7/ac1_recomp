"""Return-flag contracts never weaken an externally callable guest entry."""
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


class FlagAbiTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_combined_locals_and_abi_keep_region_oracles_closed_and_continuations_scoped(self):
        import pefile
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        image=bytearray(0x2000)
        code={0x401000:bytes.fromhex('e8fb000000eb0083c400c3'),
              0x401100:bytes.fromhex('83c001c3'),
              0x401200:bytes.fromhex('e8fbfdffff83c400c3')}
        specs=[];md=Cs(CS_ARCH_X86,CS_MODE_32)
        for address,raw in code.items():
            image[address-0x400000:address-0x400000+len(raw)]=raw
            instructions=list(md.disasm(raw,address))
            specs.append(dict(address=address,size=len(raw),instruction_count=len(instructions),
                blocks=[dict(start=i.address,end=i.address+i.size,
                    instructions=[[i.address,i.size]]) for i in instructions]))
        fake=SimpleNamespace(OPTIONAL_HEADER=SimpleNamespace(ImageBase=0x400000,SizeOfImage=0x3000,
            DATA_DIRECTORY=[SimpleNamespace(VirtualAddress=0)]*16),
            get_data=lambda offset,size:bytes(image[offset:offset+size]),close=lambda:None)
        region={0x401000:dict(boundary='0x00401007',context_bytes=64,
            members=[dict(address=hex(va)) for va in (0x401000,0x401100)],
            implementation='test_region',plan='test_prepare',pending='test_pending')}
        work=common.ROOT/'work/flagabi-region-tests';work.mkdir(parents=True,exist_ok=True)
        with patch.object(pefile,'PE',return_value=fake), \
             patch.object(lift,'validate_region_bindings',return_value=region):
            result=lift.emit_lift(work/'synthetic.exe',lift.DUKE_DEFAULT,specs,work,1,
                flag_liveness='abi',local_regs='all')
        region_source=(work/'initial_region_00401000.c').read_text()
        self.assertNotIn('ac1_abi_',region_source)
        self.assertNotIn('_ac1_regs',region_source)
        self.assertIn('ac1_region_00401000_m_00401100(c)',region_source)
        sources='\n'.join((work/name).read_text() for name in result['translation_units'])
        self.assertIn('ac1_abi_00401100',sources)
        self.assertNotIn('ac1_abi_00401000',sources)  # preserve the native entry wrapper
        runtime=lift.prepare_test_runtime()
        for direct in (False,True):
            for name in result['translation_units']:
                subprocess.run(['gcc','-O2','-std=c11','-Werror=implicit-function-declaration',
                    *(['-DAC1_DIRECT_MEMORY'] if direct else []),
                    '-I',str(runtime),'-I',str(common.ROOT/'src/runtime'),'-I',str(work),
                    '-c',str(work/name),'-o',str(work/(name+'.obj'))],
                    check=True,capture_output=True,timeout=60)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_real_emitter_keeps_private_symbols_out_of_dispatch_table(self):
        import pefile
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        image=bytearray(0x2000)
        code={0x401000:bytes.fromhex('e8fb00000083c400c3'), 0x401100:bytes.fromhex('83c001c3')}
        specs=[]
        md=Cs(CS_ARCH_X86,CS_MODE_32)
        for address,raw in code.items():
            image[address-0x400000:address-0x400000+len(raw)]=raw
            specs.append(dict(address=address,size=len(raw),instruction_count=len(list(md.disasm(raw,address))),
                blocks=[dict(start=address,end=address+len(raw),instructions=[[i.address,i.size] for i in md.disasm(raw,address)])]))
        fake=SimpleNamespace(OPTIONAL_HEADER=SimpleNamespace(ImageBase=0x400000,SizeOfImage=0x3000,
            DATA_DIRECTORY=[SimpleNamespace(VirtualAddress=0)]*16),
            get_data=lambda offset,size:bytes(image[offset:offset+size]),close=lambda:None)
        work=common.ROOT/'work/flagabi-emitter-tests';work.mkdir(parents=True,exist_ok=True)
        with patch.object(pefile,'PE',return_value=fake):
            result=lift.emit_lift(work/'synthetic.exe',lift.DUKE_DEFAULT,specs,work,1,flag_liveness='abi')
        self.assertEqual(result['flag_abi']['private_functions_narrowed'],1)
        dispatch=(work/'initial_dispatch.c').read_text()
        self.assertNotIn('ac1_abi_',dispatch)
        self.assertIn('{ 0x00401100u, L_00401100 }',dispatch)
        for name in result['translation_units']:
            subprocess.run(['gcc','-O2','-std=c11','-Werror=implicit-function-declaration',
                '-I',str(lift.prepare_test_runtime()),'-I',str(work),'-c',str(work/name),
                '-o',str(work/(name+'.obj'))],check=True,capture_output=True,timeout=60)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_compiled_public_private_and_dispatch_fallback(self):
        caller = function(0x401000,
            'push32(c, 0x00401001u); AC1_CALL(c, L_00401100, 0x00401100u);',
            'if(c->cf) c->ebx ^= 0x12345678u;',
            'push32(c, 0x00401003u); AC1_CALL(c, L_00401100, 0x00401100u);',
            'c->eax = flags_add(c, c->eax, c->ebx, 4);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->eax = flags_sub(c, c->eax, c->edx, 4);',
                          'c->esp += 4; return;')
        bodies = {0x401000:caller, 0x401100:callee}
        public, private, report = lift.flag_abi_program(bodies, private_variants=True)
        self.assertEqual(report['private_functions_narrowed'], 1)
        source = '#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        for variant, generated in [('cfg', [lift.flag_liveness_function(b) for b in bodies.values()]),
                                   ('abi', [*public.values(), *private.values()]),
                                   ('abi_local', [lift.local_register_function(b) for b in [*public.values(), *private.values()]])]:
            text = '\n'.join(generated)
            text = re.sub(r'\b(L_[0-9A-F]{8}|ac1_abi_[0-9A-F]{8})\b',
                          lambda m: variant+'_'+m[0], text)
            names = set(re.findall(r'void (\w+)\(CPU \*c\)', text))
            source += ''.join('void '+name+'(CPU *);\n' for name in names)+text+'\n'
        work = common.ROOT/'work/flagabi-tests'
        work.mkdir(parents=True, exist_ok=True)
        generated = work/'generated.c'; generated.write_text(source)
        for force_dispatch in (False, True):
            executable = work/f'contract-{force_dispatch}.exe'
            subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing',
                *(['-DAC1_NO_DIRECT_CALLS'] if force_dispatch else []),
                '-I', str(lift.prepare_test_runtime()), str(generated),
                str(common.ROOT/'tests/flag_abi_semantics.c'), '-o', str(executable)],
                check=True, capture_output=True, timeout=60)
            run = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('4096 public/private/fallback full CPU cases', run.stdout)

    def test_all_public_table_entries_pin_return_flags(self):
        bodies = {0x401000: function(0x401000, 'flags_add(c, 1, 2, 4);', 'c->esp += 4; return;')}
        public, private, report = lift.flag_abi_program(bodies)
        self.assertFalse(private)
        self.assertIn('0x3Fu', public[0x401000])
        self.assertEqual(report['public_returns_narrowed'], 0)
        self.assertEqual(report['public_entries_all_live'], 1)

    def test_only_the_dead_callsite_uses_a_private_body(self):
        bodies = {
            0x401000: function(0x401000,
                'AC1_CALL(c, L_00401100, 0x00401100u);',
                'if(c->cf) c->eax = 4;',
                'AC1_CALL(c, L_00401100, 0x00401100u);',
                'flags_add(c, 1, 2, 4);', 'c->esp += 4; return;'),
            0x401100: function(0x401100, 'flags_sub(c, 1, 2, 4);', 'c->esp += 4; return;'),
        }
        public, private, report = lift.flag_abi_program(bodies, private_variants=True)
        self.assertEqual(set(private), {0x401100})
        self.assertEqual(public[0x401000].count('AC1_CALL(c, L_00401100,'), 1)
        self.assertEqual(public[0x401000].count('AC1_CALL(c, ac1_abi_00401100,'), 1)
        self.assertIn('flags_sub_m(c, 1, 2, 4, 0x3Fu)', public[0x401100])
        self.assertIn('flags_sub_m(c, 1, 2, 4, 0x00u)', private[0x401100])
        self.assertEqual(report['direct_sites_narrowed'], 1)
        self.assertEqual(report['private_returns_narrowed'], 1)

    def test_partial_definitions_and_recursive_fixed_point(self):
        a = function(0x401000, 'AC1_CALL(c, L_00401100, 0x00401100u);',
                     'flags_incs(c, 4, 4);', 'c->esp += 4; return;')
        b = function(0x401100, 'flags_logicz(c, 4, 4);', 'c->esp += 4; return;')
        _, _, report = lift.flag_abi_program({0x401000:a, 0x401100:b})
        self.assertEqual(report['proven_defining_callees'], 2)
        self.assertEqual(lift.flag_abi_must_define(lift.flag_flow(b), {}), lift.FLAG_ALL & ~lift.FLAG_BITS['af'])
        inc = function(0x401200, 'flags_incs(c, 4, 4);', 'c->esp += 4; return;')
        self.assertEqual(lift.flag_abi_must_define(lift.flag_flow(inc), {}), lift.FLAG_ALL & ~lift.FLAG_BITS['cf'])
        # No recursive bootstrap assumption that a callee defines all flags.
        recursive = function(0x401000, 'AC1_CALL(c, L_00401000, 0x00401000u);', 'c->esp += 4; return;')
        _, private, report = lift.flag_abi_program({0x401000:recursive}, private_variants=True)
        self.assertFalse(private)
        self.assertEqual(report['proven_defining_callees'], 0)

    def test_pinned_native_and_seh_entries_are_not_cloned(self):
        caller = function(0x401000, 'AC1_CALL(c, L_00401100, 0x00401100u);',
                          'flags_sub(c, 1, 2, 4);', 'c->esp += 4; return;')
        callee = function(0x401100, 'flags_add(c, 1, 2, 4);', 'c->esp += 4; return;')
        for pinned, body in (({0x401100}, callee), (set(), callee.replace('flags_add', '/* fs:[0] */ flags_add'))):
            _, private, report = lift.flag_abi_program({0x401000:caller, 0x401100:body}, pinned, True)
            self.assertFalse(private)
            self.assertEqual(report['private_returns_narrowed'], 0)

    def test_unknown_dispatch_observers_and_live_return_propagate(self):
        for observer in ('dispatch(c, c->eax);', 'dispatch_jmp(c, c->eax); return;',
                         'push32(c, eflags_pack(c));', 'unknown(c);', 'c->esp += 4; return;'):
            caller = function(0x401000, 'AC1_CALL(c, L_00401100, 0x00401100u);', observer)
            callee = function(0x401100, 'flags_add(c, 1, 2, 4);', 'c->esp += 4; return;')
            _, private, _ = lift.flag_abi_program({0x401000:caller, 0x401100:callee}, private_variants=True)
            self.assertFalse(private, observer)


if __name__ == '__main__': unittest.main()
