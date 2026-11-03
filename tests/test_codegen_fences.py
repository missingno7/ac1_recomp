"""Loop fences: every branch back to an equal or lower address keeps guest loads in the loop."""
import unittest
import test_codegen_flags as common

lift = common.lift


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


if __name__ == '__main__':
    unittest.main()
