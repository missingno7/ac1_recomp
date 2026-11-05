"""AC1_VECTOR_ACCESS: 16-byte guest accesses as one vector access, equal to the default in every observable."""
import shutil
import subprocess
import unittest
import test_codegen_flags as common

lift = common.lift

# Lifted-style body: unaligned and aligned 16-byte moves around a lane read, a
# lane write into the reloaded value's slot, and an overlapping 16-byte copy.
BODY = r'''
void fixture(CPU *c) {
    AC1_DECLARE_LOCAL_ARENA;
    XMM x = rdxm(c->ecx);
    wrxm(c->esp + 0x10u, x);
    c->eax = rd32(c->esp + 0x14u);
    wrss(c->esp + 0x18u, 2.5f);
    XMM y = rdxm(c->esp + 0x10u);
    wrxm(c->edx + 1u, y);
    XMM z = rdxm(c->edx + 5u);
    wrxm(c->edx + 9u, z);
    c->xmm[0] = x; c->xmm[1] = y; c->xmm[2] = z;
}
'''

DRIVER = r'''
#include <stdio.h>
static unsigned char memory[0x10000];
uint8_t *ac1_direct_arena = memory;
static uint64_t hash = 1469598103934665603ull;
static void mix(const void *p, size_t n) { const unsigned char *b = p; while (n--) hash = (hash ^ *b++) * 1099511628211ull; }
static uint32_t seed = 0x9E3779B9u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
int main(void) {
    for (unsigned k = 0; k < 4000; ++k) {
        for (unsigned i = 0x1000; i < 0x4000; ++i) memory[i] = (unsigned char)rnd();
        CPU c; memset(&c, 0, sizeof c);
        c.ecx = 0x1000u + (rnd() & 0x3FFu); c.esp = 0x2000u + (rnd() & 0x3F0u); c.edx = 0x3000u + (rnd() & 0x3FFu);
        fixture(&c);
        mix(&c.eax, 4); mix(c.xmm, 3 * sizeof(XMM)); mix(memory, sizeof memory);
    }
    printf("%016llx\n", (unsigned long long)hash);
    return 0;
}
'''


@unittest.skipUnless(shutil.which('gcc'), 'native C compiler required')
class VectorAccessTests(unittest.TestCase):
    def run_build(self, name, defines):
        runtime = lift.prepare_test_runtime()
        work = common.ROOT/'work/vector-access-tests'
        work.mkdir(parents=True, exist_ok=True)
        path = work/'fixture.c'
        path.write_text('#include "cpu.h"\n#include "ac1_local_memory.h"\n' + BODY + DRIVER)
        exe = work/f'{name}.exe'
        build = subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-DAC1_DIRECT_MEMORY', *defines,
                                '-I', str(runtime), str(path), '-o', str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run.stdout.strip()

    def test_matches_the_default_on_aligned_unaligned_and_overlapping_accesses(self):
        self.assertEqual(self.run_build('vector', ['-DAC1_VECTOR_ACCESS']), self.run_build('default', []))

    def test_both_accessor_families_take_the_vector_path(self):
        # The arena-based clones are what generated code calls; a template that
        # misses them would leave the switch silently without effect.
        header = (lift.prepare_test_runtime()/'cpu.h').read_text(encoding='utf-8')
        self.assertIn('*(ac1_unaligned_v16 *)o = *(const ac1_unaligned_v16 *)(ac1_direct_arena + a)', header)
        self.assertIn('*(ac1_unaligned_v16 *)o = *(const ac1_unaligned_v16 *)(_ac1_base + a)', header)
        self.assertIn('*(ac1_unaligned_v16 *)(_ac1_base + a) = *(const ac1_unaligned_v16 *)i', header)

    def test_the_fixture_detects_a_wrong_vector_access(self):
        # A write that moves one byte must change the hash, or equality above proves nothing.
        self.assertNotEqual(self.run_build('default', []), self.run_mutant())

    def run_mutant(self):
        runtime = lift.prepare_test_runtime()
        work = common.ROOT/'work/vector-access-tests'
        text = (runtime/'cpu.h').read_text(encoding='utf-8').replace(
            '*(ac1_unaligned_v16 *)(_ac1_base + a) = *(const ac1_unaligned_v16 *)i',
            '*(ac1_unaligned_v16 *)(_ac1_base + a + 1u) = *(const ac1_unaligned_v16 *)i')
        (work/'mutant').mkdir(exist_ok=True)
        for f in runtime.glob('*.h'):
            (work/'mutant'/f.name).write_bytes(f.read_bytes())
        (work/'mutant'/'cpu.h').write_text(text, encoding='utf-8')
        path = work/'fixture.c'
        path.write_text('#include "cpu.h"\n#include "ac1_local_memory.h"\n' + BODY + DRIVER)
        exe = work/'mutant.exe'
        subprocess.run(['gcc', '-O2', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-DAC1_DIRECT_MEMORY', '-DAC1_VECTOR_ACCESS',
                        '-I', str(work/'mutant'), str(path), '-o', str(exe)], capture_output=True, text=True, timeout=120, check=True)
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=60).stdout.strip()


if __name__ == '__main__':
    unittest.main()
