"""Captured-arena compiler and full-state differential contracts."""
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path
import test_codegen_flags as flags

lift,ROOT=flags.lift,flags.ROOT

class LocalMemoryTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
    def test_generated_region_continuation_has_an_independent_arena_capture(self):
        runtime=lift.prepare_test_runtime()
        work=ROOT/'work/local-memory-region-tests';work.mkdir(parents=True,exist_ok=True)
        body=lift.declare_local_memory_base(flags.function('c->eax=rd32(0x8000u);',
            ('L_00401001','wr32(0x8004u,c->eax);'),'c->esp+=4; return;'))
        header='#include "cpu.h"\n#include "ac1_local_memory.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        lift.emit_region_sources({0x401000:{'members':[{'address':'0x00401000'}],
            'boundary':'0x00401001'}},[0x401000],[body],work,header)
        subprocess.run(['gcc','-std=c11','-O2','-DAC1_DIRECT_MEMORY','-I',str(runtime),
            '-I',str(ROOT/'src/runtime'),'-c',str(work/'initial_region_00401000.c'),
            '-o',str(work/'region.o')],check=True,capture_output=True,timeout=60)
        lift.emit_journal_region_source(0x401000,{'members':[{'address':'0x00401000'}],
            'implementation':'test_journal_implementation'},
            {0x401000:body},work,header)
        subprocess.run(['gcc','-std=c11','-O2','-DAC1_DIRECT_MEMORY','-I',str(runtime),
            '-I',str(ROOT/'src/runtime'),'-c',str(work/'initial_region_00401000.c'),
            '-o',str(work/'journal.o')],check=True,capture_output=True,timeout=60)

    @unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
    def test_failed_reservation_keeps_platform_error_and_guard_size(self):
        runtime=lift.prepare_test_runtime()
        work=ROOT/'work/local-memory-tests';work.mkdir(parents=True,exist_ok=True)
        exe=work/'reserve-failure.exe'
        subprocess.run(['gcc',*(['-D_GNU_SOURCE=1'] if os.name!='nt' else []),'-std=c11','-O2','-I',str(runtime),'-I',str(ROOT/'src/runtime'),
            str(ROOT/'tests/local_memory_reserve_failure.c'),'-o',str(exe)],check=True,capture_output=True,timeout=60)
        run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
        self.assertIn('AC1 direct arena reservation failed: bytes=',run.stderr)
        self.assertRegex(run.stderr,r'guard=[1-9][0-9]*')
        self.assertIn('VirtualAlloc error=8' if os.name=='nt' else 'mmap errno=',run.stderr)

    def test_all_translated_entries_capture_before_sparse_guard(self):
        body=flags.function('c->eax=rd32(c->eax);','c->esp+=4; return;')
        body=body.replace('{\n','{\n    goto L_00401001;\n',1)
        transformed=lift.declare_local_memory_base(body)
        self.assertLess(transformed.index('AC1_DECLARE_LOCAL_ARENA'),transformed.index('goto'))
        for prefix in ('void L_', 'void ac1_abi_', 'static void ac1_translated_'):
            with self.subTest(prefix=prefix):
                candidate=body.replace('void L_',prefix)
                self.assertEqual(lift.declare_local_memory_base(candidate).count('AC1_DECLARE_LOCAL_ARENA'),1)

    def test_transitive_memory_helpers_do_not_read_global_base(self):
        runtime=lift.prepare_test_runtime()
        cpu=(runtime/'cpu.h').read_text()
        section=cpu.split('/* AC1_LOCAL_MEMORY_HELPERS */',1)[1]
        self.assertNotIn('ac1_direct_arena',section)
        self.assertIn('(guest_atomic_xadd32)',section)  # exact checked fallback
        for name in ('push32','pop32','rdf80','wrf80','ac1_x87_save','ac1_x87_restore','bit_string_op'):
            self.assertIn('ac1_base_'+name+'(',section)

    @unittest.skipUnless(os.name=='nt' and shutil.which('gcc'), 'Windows native fault oracle required')
    def test_precise_read_and_write_fault_eip_and_full_cpu_observers(self):
        runtime=lift.prepare_test_runtime()
        work=ROOT/'work/local-memory-eip-tests';work.mkdir(parents=True,exist_ok=True)
        for operation in ('c->ebx=rd32(0x8000u);','wr32(0x8000u,c->ebx); c->ebx=0;'):
            body=flags.function('c->eax=flags_add_m(c,c->eax,1u,4,0x3Fu);',
                                'c->edx ^= c->ecx;',operation,'observe(c);','c->esp += 4; return;')
            source='#include "cpu.h"\n#include "ac1_local_memory.h"\n'+'#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'+'\nvoid observe(CPU*);\n'
            for name,text in [('full',body),('local',lift.local_register_function(body))]:
                source+=lift.declare_local_memory_base(text).replace('void L_00401000(',f'void {name}_L_00401000(',1)+'\n'
            path=work/'generated.c';path.write_text(source);exe=work/'oracle.exe'
            subprocess.run(['gcc','-std=c11','-O2','-fwrapv','-fno-strict-aliasing','-ffp-contract=off',
                '-DAC1_DIRECT_MEMORY','-DAC1_TRACE_EIP','-I',str(runtime),
                str(path),str(ROOT/'tests/local_memory_eip.c'),'-lm','-o',str(exe)],check=True,capture_output=True,timeout=60)
            run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
            self.assertIn('4096 full CPU/observer cases and precise direct fault EIPs passed',run.stdout)

    @unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
    def test_two_real_arenas_full_cpu_memory_and_compiled_base_stability(self):
        runtime=lift.prepare_test_runtime()
        work=ROOT/'work/local-memory-tests';work.mkdir(parents=True,exist_ok=True)
        common=['gcc',*(['-D_GNU_SOURCE=1'] if os.name!='nt' else []),'-std=c11','-O2','-fwrapv','-fno-strict-aliasing','-ffp-contract=off',
                '-DAC1_DIRECT_MEMORY','-I',str(runtime),'-I',str(ROOT/'src/runtime')]
        objects=[]
        for name,defines in [('legacy',[]),('local',['-DLOCAL_VARIANT=1'])]:
            obj=work/(name+'.o');objects.append(str(obj))
            subprocess.run(common+defines+['-c',str(ROOT/'tests/local_memory_semantics.c'),'-o',str(obj)],check=True,capture_output=True,timeout=60)
        exe=work/'oracle.exe'
        subprocess.run(common+objects+[str(ROOT/'tests/local_memory_oracle.c'),str(runtime/'guest_memory.c'),'-lm','-o',str(exe)],check=True,capture_output=True,timeout=60)
        run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=60)
        self.assertIn('4096 full CPU/memory cases passed',run.stdout)
        for address,reason in [('0x8ff9','unaligned atomic'),('0x8fff','outside mapped range')]:
            for operation in range(3):
                failures=[subprocess.run([str(exe),str(local),str(operation),address],capture_output=True,text=True,timeout=30) for local in (0,1)]
                self.assertTrue(all(result.returncode!=0 for result in failures))
                self.assertEqual(failures[0].stderr,failures[1].stderr)
                self.assertIn('guest memory '+reason,failures[0].stderr)
                self.assertNotIn('unexpected atomic success',failures[0].stderr)
        # An opaque CPU observer and arbitrary byte stores defeat a global-base
        # reload optimization. The captured pointer must survive both.
        probe=work/'base_shape.c'
        probe.write_text('#include "cpu.h"\n#include "ac1_local_memory.h"\nvoid opaque(CPU*);\n'
                         'void probe(CPU*c) { AC1_DECLARE_LOCAL_ARENA; wr8(c->eax,1); '
                         'c->ecx=rd32(c->ebx); opaque(c); wr32(c->edi,c->ecx); c->edx=rd32(c->esi); }\n')
        asm=work/'base_shape.s'
        subprocess.run(common+['-S',str(probe),'-o',str(asm)],check=True,capture_output=True,timeout=60)
        text=asm.read_text();body=text[text.index('probe:'):]
        body=body.split('.seh_endproc',1)[0] if os.name=='nt' else body.split('.size',1)[0]
        self.assertEqual(len(re.findall(r'ac1_direct_arena',body)),1,text)
        self.assertNotIn('guest_execution_read',body)
        self.assertNotIn('guest_execution_write',body)
        self.assertNotIn('memcpy',body)

if __name__=='__main__':unittest.main()
