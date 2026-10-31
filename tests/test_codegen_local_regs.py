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
    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_dirty_joins_loops_and_unreferenced_helper_writes(self):
        import test_codegen_flags as flags
        source=flags.function(
            'c->ecx = (c->ecx & 7u) + 1u;',
            ('L_00401001','if(c->eax & 1u) goto L_00401004;'),
            'c->ebx += c->edi;', 'goto L_00401005;',
            ('L_00401004','SET8L(c->edx, R8L(c->eax));'),
            ('L_00401005','opaque(c);'), '--c->ecx;',
            'if(c->ecx) goto L_00401001;', 'c->esp += 4; return;')
        optimized=lift.local_register_function(source)
        self.assertNotIn('_ac1_regs.ebp =',optimized)
        generated='#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\nvoid opaque(CPU *);\n'
        generated+=source.replace('void L_00401000(','void plain_L_00401000(',1)+'\n'
        generated+=optimized.replace('void L_00401000(','void local_L_00401000(',1)+'\n'
        work=ROOT/'work/local-regs-tests';work.mkdir(parents=True,exist_ok=True)
        path=work/'dirty.c';path.write_text(generated);exe=path.with_suffix('.exe')
        subprocess.run(['gcc','-O2','-std=c11','-fwrapv','-fno-strict-aliasing',
            '-I',str(lift.prepare_test_runtime()),str(path),str(ROOT/'tests/local_dirty_semantics.c'),
            '-o',str(exe)],check=True,capture_output=True,timeout=60)
        run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
        self.assertIn('4096 dirty CFG full CPU and observer cases',run.stdout)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_push_and_pop_move_the_local_esp(self):
        import test_codegen_flags as flags
        source=flags.function(
            'push32(c, c->eax);', 'push32(c, c->esp);', 'push32(c, 0x12345678u);',
            'c->ebx = (uint32_t)(pop32(c));', '(void)pop32(c);', 'c->ecx = pop32(c);',
            'push32(c, rd32((c->esp + 0x00000004u)));', 'opaque(c);', 'push32(c, c->esp);',
            'c->esp = (uint32_t)(pop32(c));', 'push32(c, c->edx);', 'c->esp += 4; return;')
        optimized=lift.local_register_function(source)
        self.assertNotIn('push32(',optimized)
        self.assertNotIn('pop32(',optimized)
        self.assertNotIn('c->esp = _ac1_regs.esp;\n    AC1_SET_GUEST_EIP(c, 0x00401001u);',optimized)
        generated='#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\nvoid opaque(CPU *);\n'
        generated+=source.replace('void L_00401000(','void plain_L_00401000(',1)+'\n'
        generated+=optimized.replace('void L_00401000(','void local_L_00401000(',1)+'\n'
        work=ROOT/'work/local-regs-tests';work.mkdir(parents=True,exist_ok=True)
        path=work/'stack.c';path.write_text(generated);exe=path.with_suffix('.exe')
        subprocess.run(['gcc','-O2','-std=c11','-fwrapv','-fno-strict-aliasing','-DAC1_DIRECT_MEMORY',
            '-I',str(lift.prepare_test_runtime()),str(path),str(ROOT/'tests/local_stack_semantics.c'),
            '-o',str(exe)],check=True,capture_output=True,timeout=60)
        run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
        self.assertIn('4096 local push/pop CPU and stack memory cases passed',run.stdout)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_xmm_locals_match_full_cpu_at_calls_and_returns(self):
        import test_codegen_flags as flags
        source=flags.function(
            'c->xmm[1] = c->xmm[0];',
            '{ XMM _s = c->xmm[2]; XMM _d = c->xmm[1]; c->xmm[1].f32[0] = _d.f32[0] + _s.f32[0]; '
            'c->xmm[1].f32[3] = _d.f32[3] * _s.f32[1]; }',
            'AC1_CALL(c, L_00401100, 0x00401100u);',
            'c->xmm[0].u32[1] ^= c->xmm[3].u32[2];',
            'opaque(c);',
            'if (c->eax & 1u) { c->esp += 4; return; }',
            '{ uint32_t _ct = c->ecx; dispatch(c, _ct); }',
            '{ uint64_t _lane = c->xmm[2].u64[1]; c->xmm[0].u64[0] = _lane; }',
            'c->esp += 4; return;')
        optimized=lift.local_xmm_function(source)
        self.assertIn('XMM _ac1_xmm0 __attribute__((unused)) = c->xmm[0], _ac1_xmm1 __attribute__((unused)) = c->xmm[1]',optimized)
        # Lane arithmetic uses the locals; the CPU copy is touched only at sync points.
        self.assertNotRegex(optimized,r'c->xmm\[\d\]\.')
        self.assertEqual(optimized.count('AC1_CALL('),1)
        self.assertEqual(lift.local_xmm_function(source.replace('c->xmm[1] = c->xmm[0];',
            'c->xmm[1] = c->xmm[c->ecx & 7u];')),source.replace('c->xmm[1] = c->xmm[0];',
            'c->xmm[1] = c->xmm[c->ecx & 7u];'))   # dynamic index: unchanged
        generated='#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        generated+='void opaque(CPU *);\nvoid L_00401100(CPU *);\n'
        generated+=source.replace('void L_00401000(','void plain_L_00401000(',1)+'\n'
        generated+=optimized.replace('void L_00401000(','void local_L_00401000(',1)+'\n'
        work=ROOT/'work/local-regs-tests';work.mkdir(parents=True,exist_ok=True)
        path=work/'xmm.c';path.write_text(generated);exe=path.with_suffix('.exe')
        subprocess.run(['gcc','-O2','-std=c11','-fwrapv','-fno-strict-aliasing',
            '-I',str(lift.prepare_test_runtime()),str(path),str(ROOT/'tests/local_xmm_semantics.c'),
            '-o',str(exe)],check=True,capture_output=True,timeout=60)
        run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
        self.assertIn('4096 XMM-local full CPU, call and return cases passed',run.stdout)

    def test_xmm_free_helpers_cover_the_prepared_header(self):
        header=(lift.prepare_test_runtime()/'cpu.h').read_text()
        free=lift.xmm_free_helpers(header)
        for name in ('push32','pop32','fst','fpush','sse_compare','flags_add_m','ac1_x87_integer','rdxm','wrxm'):
            self.assertIn(name,free)
        for name in ('AC1_CALL','dispatch','dispatch_jmp','RECOMP_TODO'):
            self.assertNotIn(name,free)
        with self.assertRaises(ValueError):
            lift.xmm_free_helpers(header+'\nstatic inline void leak(CPU *c) { c->xmm[0].u32[0]=0; }\n')

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
        # POP is arithmetic on the local ESP, so the taken edge reloads nothing.
        self.assertIn('_ac1_regs.ebx = _ac1_pop; } if (c->zf) {  __asm__', body)
        # A POP the rewrite leaves to the helper still moves ESP through c->esp.
        helper = lift.local_register_function(source.replace('c->ebx = pop32(c);', 'wr32(c->ebx, pop32(c));'))
        self.assertIn('wr32(_ac1_regs.ebx, pop32(c))', helper)
        self.assertIn('_ac1_regs.esp = c->esp; __asm__', helper)
        self.assertIn('c->esp = _ac1_regs.esp;\n    AC1_SET_GUEST_EIP(c, 0x00401002', body)
        for text in ('setjmp(c->opaque);', 'longjmp(c->opaque);'):
            unsafe = source.replace('    c->esp += 4; return;', '    ' + text + ' return;')
            self.assertEqual(lift.local_register_function(unsafe), unsafe)
        # Exception frames (fs:[0]) keep locals: the runtime never resumes guest
        # code from an exception context, and fs: accesses touch no GPR.
        framed = '\n'.join(['void L_00401000(CPU *c)', '{',
            '    AC1_SET_GUEST_EIP(c, 0x00401000u);',
            '    c->eax = (uint32_t)(FS_RD32(c, (0x00000000u)));   /* 00401000: mov eax, dword ptr fs:[0] */',
            '    AC1_SET_GUEST_EIP(c, 0x00401006u);',
            '    push32(c, c->eax);',
            '    AC1_SET_GUEST_EIP(c, 0x00401007u);',
            '    FS_WR32(c, (0x00000000u), c->esp);                /* 00401007: mov dword ptr fs:[0], esp */',
            '    AC1_SET_GUEST_EIP(c, 0x0040100Du);',
            '    c->esp += 4; return;', '}'])
        local = lift.local_register_function(framed)
        self.assertIn('_ac1_regs.eax = (uint32_t)(FS_RD32(c, (0x00000000u)));', local)
        self.assertIn('FS_WR32(c, (0x00000000u), _ac1_regs.esp);', local)
        escaped=source.replace('RECOMP_TODO(0x401002, "unknown");','opaque(&c->eax);')
        self.assertEqual(lift.local_register_function(escaped),escaped)

    def test_audited_pure_and_stack_helpers_use_only_declared_cpu_fields(self):
        header=(lift.prepare_test_runtime()/'cpu.h').read_text()
        self.assertRegex(header,re.compile(r'^#define STACK_BASE\(c\) 0u$',re.M))
        for name in (lift.LOCAL_GPR_FREE_HELPERS-{'AC1_SET_GUEST_EIP'}) | lift.LOCAL_ESP_HELPERS:
            bodies=[]
            for match in re.finditer(r'\b'+name+r'\([^{};]*\)\s*\{',header):
                depth,end=1,match.end()
                while depth:
                    depth+=(header[end]=='{')-(header[end]=='}');end+=1
                bodies.append(header[match.end():end-1])
            # Macro helpers (fs: accesses): every preprocessor variant.
            bodies+=re.findall(r'^#define\s+'+name+r'\([^)]*\)(.*)$',header,re.M)
            self.assertTrue(bodies,name)
            for body in bodies:
                code=re.sub(r'/\*.*?\*/','',body,flags=re.S).replace('(c)->','c->')
                fields=set(re.findall(r'\bc->(\w+)',code))
                permitted={'esp'} if name in lift.LOCAL_ESP_HELPERS else {'fs_base'} if name.startswith('FS_') else \
                    set(lift.FLAG_BITS)|{'df','eflags_misc','fpu_sw','fpu_top','fpu_cw','fpu_tw','mxcsr','fp_initialized'}
                self.assertLessEqual(fields,permitted,name)
                pointer=re.sub(r'\bc->\w+','member',code)
                if name in lift.LOCAL_ESP_HELPERS:pointer=pointer.replace('STACK_BASE(c)','0u')
                pointer=re.sub(r'\b(?:flags_\w+|'+'|'.join(lift.LOCAL_GPR_FREE_HELPERS)+r')\(c(?=[,)])','safe(',pointer)
                self.assertNotRegex(pointer,r'\bc\b|unsupported_instruction|RECOMP_TODO',name)

    @unittest.skipUnless(shutil.which('gcc'), 'native compiler required')
    def test_push_pop_esp_flags_and_shifts_match_full_cpu_and_stack(self):
        from capstone.x86 import X86_OP_IMM,X86_OP_MEM
        from lift32_cpu import Lifter
        # Save ESP; PUSH ESP/POP ECX; move stack via PUSH EAX/POP ESP;
        # PUSHFD/POP EDX; SAR with CL=0..31; conditional return path.
        raw=bytes.fromhex('89e354598d442440505c9c5a89f0d3f87405bf11111111c3')
        read=lambda va,n:raw[va-0x401000:va-0x401000+n]
        lifter=Lifter('synthetic scalar-local stack fixture',0x1000,read)
        instructions=list(lifter.md.disasm(raw,0x401000))
        spec={'address':0x401000,'size':len(raw),'blocks':[{'start':0x401000,'end':0x401000+len(raw),
            'instructions':[[i.address,i.size] for i in instructions]}]}
        plain=lift.flag_liveness_function(lift.lift_exact_function(lifter,spec,read,X86_OP_IMM,X86_OP_MEM))
        local=lift.local_register_function(plain)
        self.assertNotIn('memcpy(',local)
        self.assertIn('_ac1_regs.esp = _ac1_pop;',local)   # POP ESP takes the loaded value
        self.assertNotIn('pop32(',local)
        # The shift helper no longer forces a complete CPU synchronization.
        self.assertIn('op_sar(c, _ac1_regs.eax',local)
        source='#include "cpu.h"\n#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n'
        source+=plain.replace('void L_00401000(','void plain_L_00401000(',1)+'\n'
        source+=local.replace('void L_00401000(','void local_L_00401000(',1)+'\n'
        work=ROOT/'work/local-regs-tests';work.mkdir(parents=True,exist_ok=True)
        generated=work/'scalar-stack.c';generated.write_text(source)
        exe=generated.with_suffix('.exe')
        subprocess.run(['gcc','-O2','-std=c11','-fwrapv','-fno-strict-aliasing',
            '-I',str(lift.prepare_test_runtime()),str(generated),str(ROOT/'tests/local_scalar_semantics.c'),
            '-o',str(exe)],check=True,capture_output=True,timeout=60)
        run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
        self.assertIn('4096 scalar-local full CPU and stack cases',run.stdout)

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
