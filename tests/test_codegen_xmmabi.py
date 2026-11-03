"""Whole-program XMM contracts: XMM locals sync only what callees and callers read."""
import re
import unittest
import test_codegen_flags as common

lift = common.lift


def function(address, *instructions):
    """One guest instruction per line, each with the address comment the lifter emits."""
    lines = [f'void L_{address:08X}(CPU *c)', '{']
    for i, text in enumerate(instructions):
        lines.extend([f'    AC1_SET_GUEST_EIP(c, 0x{address+i:08X}u);', f'    {text} /* {address+i:08X}: x */'])
    return '\n'.join(lines + ['}'])


def call(target):
    return f'AC1_CALL(c, L_{target:08X}, 0x{target:08X}u);'


def emitted(bodies, address, pinned=()):
    contracts, report = lift.xmm_contract_program(bodies, pinned)
    return lift.local_xmm_function(bodies[address], contracts.get(address)), contracts, report


def around_call(body):
    match = re.search(r'\{ ([^{}]*?)AC1_CALL\([^;]*;([^{}]*)\}', body)
    return re.findall(r'c->xmm\[(\d)\] = _ac1_xmm', match[1]), re.findall(r'_ac1_xmm(\d) = c->xmm', match[2])


class XmmContractTests(unittest.TestCase):
    def test_call_to_xmm_free_callee_syncs_nothing(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', call(0x401100),
                          'wrxm(c->ecx, c->xmm[0]);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->eax = rd32(c->esp);', 'c->esp += 4; return;')
        body, contracts, _ = emitted({0x401000: caller, 0x401100: callee}, 0x401000)
        # The callee never touches xmm0, so the caller's value survives in its
        # local; the callee passes it through, so it is stored and reloaded.
        self.assertEqual(contracts[0x401100]['entry'], 1)
        self.assertEqual(around_call(body), (['0'], ['0']))

    def test_dead_after_call_is_neither_stored_nor_reloaded(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', 'wrxm(c->ecx, c->xmm[0]);', call(0x401100),
                          'c->esp += 4; return;')
        callee = function(0x401100, 'c->xmm[0] = rdxm(c->esp);', 'wrxm(c->edx, c->xmm[0]);', 'c->esp += 4; return;')
        body, contracts, _ = emitted({0x401000: caller, 0x401100: callee}, 0x401000)
        self.assertEqual((contracts[0x401100]['entry'], contracts[0x401100]['ret']), (0, 0))
        self.assertIsNone(re.search(r'\{ [^{}]*AC1_CALL', body) and re.search(r'c->xmm\[0\] = _ac1_xmm0; AC1_CALL', body))
        self.assertEqual(around_call(body), ([], []))
        # Nothing is live at entry: the local starts from zero, not the struct.
        self.assertIn('_ac1_xmm0 __attribute__((unused)) = {{0}}', body)

    def test_callee_result_read_by_caller_joins_return_demand(self):
        caller = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[1]);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->xmm[1] = rdxm(c->esp);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: caller, 0x401100: callee})
        self.assertEqual((contracts[0x401100]['entry'], contracts[0x401100]['ret']), (0, 2))
        callee_body = lift.local_xmm_function(callee, contracts[0x401100])
        self.assertIn('{ c->xmm[1] = _ac1_xmm1; return; }', callee_body)

    def test_lane_writes_read_the_register(self):
        callee = function(0x401100, 'c->xmm[2].vf32 = ac1_lane_vf32(c->xmm[2].vf32, 0, 1.0f);',
                          'wrxm(c->edx, c->xmm[2]);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401100: callee})
        self.assertEqual(contracts[0x401100]['entry'], 4)

    def test_guarded_writes_do_not_kill(self):
        callee = function(0x401100, 'if (c->eax) c->xmm[3] = rdxm(c->esp);', 'wrxm(c->edx, c->xmm[3]);',
                          'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401100: callee})
        self.assertEqual(contracts[0x401100]['entry'], 8)

    def test_indirect_calls_use_every_possible_target(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', '{ uint32_t _ct = c->edx; dispatch(c, _ct); }',
                          'wrxm(c->ecx, c->xmm[0]);', 'c->esp += 4; return;')
        target = function(0x401100, 'wrxm(c->edx, c->xmm[5]);', 'c->esp += 4; return;')
        contracts, report = lift.xmm_contract_program({0x401000: caller, 0x401100: target},
                                                      call_targets={0x401100}, jump_targets={0x401100})
        self.assertEqual(report['indirect_entry_demand'], 0x21)
        self.assertEqual(contracts[0x401100]['ret'] & 1, 1)
        site = contracts[0x401000]['sites'][0x401001]
        self.assertEqual(site[:2], (0x21, 1))

    def test_pinned_native_bindings_sync_everything(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', call(0x401100), 'wrxm(c->ecx, c->xmm[0]);',
                          'c->esp += 4; return;')
        native = function(0x401100, 'c->esp += 4; return;')
        body, contracts, _ = emitted({0x401000: caller, 0x401100: native}, 0x401000, pinned={0x401100})
        self.assertNotIn(0x401100, contracts)
        self.assertEqual(around_call(body), (['0'], ['0']))

    def test_pinned_translated_bodies_pass_demand_to_their_callees(self):
        # A region's translated fallback reads registers after its calls.
        region = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[6]);', 'c->esp += 4; return;')
        callee = function(0x401100, 'c->xmm[6] = rdxm(c->esp);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: region, 0x401100: callee}, pinned={0x401000})
        self.assertEqual(contracts[0x401100]['ret'], 0xFF)

    def test_tail_call_reloads_and_returns_the_return_demand(self):
        top = function(0x401000, call(0x401100), 'wrxm(c->ecx, c->xmm[4]);', 'c->esp += 4; return;')
        mid = function(0x401100, 'c->xmm[4] = rdxm(c->eax);', 'wrxm(c->ecx, c->xmm[4]);',
                       'AC1_CALL(c, L_00401200, 0x00401200u); return;')
        leaf = function(0x401200, 'c->xmm[4] = rdxm(c->esp);', 'c->esp += 4; return;')
        contracts, _ = lift.xmm_contract_program({0x401000: top, 0x401100: mid, 0x401200: leaf})
        self.assertEqual(contracts[0x401200]['ret'], 0x10)
        body = lift.local_xmm_function(mid, contracts[0x401100])
        self.assertIn('AC1_CALL(c, L_00401200, 0x00401200u); _ac1_xmm4 = c->xmm[4]; }', body)

    def test_no_contract_keeps_the_full_sync(self):
        caller = function(0x401000, 'c->xmm[0] = rdxm(c->eax);', call(0x401100), 'wrxm(c->ecx, c->xmm[0]);',
                          'c->esp += 4; return;')
        self.assertEqual(lift.local_xmm_function(caller), lift.local_xmm_function(caller, None))
        self.assertEqual(around_call(lift.local_xmm_function(caller)), (['0'], ['0']))


if __name__ == '__main__':
    unittest.main()
