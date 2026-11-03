"""Per-function facts from the decoded x86, shared by analyses that need the CFG.

The lifter's passes so far read structure back out of generated C. This module
answers the same questions from the recovered blocks (functions.json) and the
decoded instructions instead: the CFG, what each call transfers to, and what
each x87 instruction does to the register stack. Analyses built on it can run
inside the lifter (which already holds `spec` and the decoded instructions) or
standalone over the catalog:

    python tools/function_facts.py x87-census [--profile NAME]
"""
import argparse
import collections
import dataclasses
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------- x87 effects
# Depth counts values on the register stack: a push adds one, a pop removes
# one, and TOP moves the opposite way. Each entry: (pops, pushes, kind).
# kind: 'stack' (ordinary), 'status' (reads the status word, which embeds TOP),
# 'neutral' (control word, exceptions, waits: no stack or TOP access),
# 'rotate' (moves TOP without a value: fincstp/fdecstp), 'tag' (ffree: tag word
# only, which the native lowering does not model), 'reset' (TOP := 0) and
# 'env' (saves or loads the whole x87 state). Anything else is unknown.
_PUSH = ('fld', 'fild', 'fbld', 'fldz', 'fld1', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2')
_NONE = ('fst', 'fist', 'fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr', 'fiadd', 'fisub', 'fisubr', 'fimul',
         'fidiv', 'fidivr', 'fcom', 'fucom', 'ficom', 'fcomi', 'fucomi', 'ftst', 'fxam', 'fchs', 'fabs', 'fsqrt',
         'fsin', 'fcos', 'frndint', 'fscale', 'f2xm1', 'fprem', 'fprem1', 'fxch',
         'fcmovb', 'fcmove', 'fcmovbe', 'fcmovu', 'fcmovnb', 'fcmovne', 'fcmovnbe', 'fcmovnu')
_POP1 = ('fstp', 'fistp', 'fisttp', 'fbstp', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fcomp',
         'fucomp', 'ficomp', 'fcomip', 'fucomip', 'fcompi', 'fucompi', 'fpatan', 'fyl2x', 'fyl2xp1')
X87_EFFECTS = {
    **{m: (0, 1, 'stack') for m in _PUSH}, **{m: (0, 0, 'stack') for m in _NONE},
    **{m: (1, 0, 'stack') for m in _POP1},
    'fcompp': (2, 0, 'stack'), 'fucompp': (2, 0, 'stack'),
    'fptan': (0, 1, 'stack'), 'fsincos': (0, 1, 'stack'), 'fxtract': (0, 1, 'stack'),
    'fnstsw': (0, 0, 'status'), 'fstsw': (0, 0, 'status'),
    'fnstcw': (0, 0, 'neutral'), 'fstcw': (0, 0, 'neutral'), 'fldcw': (0, 0, 'neutral'),
    'fnclex': (0, 0, 'neutral'), 'fclex': (0, 0, 'neutral'), 'fwait': (0, 0, 'neutral'),
    'wait': (0, 0, 'neutral'), 'fnop': (0, 0, 'neutral'),
    'fincstp': (1, 0, 'rotate'), 'fdecstp': (0, 1, 'rotate'),
    'ffree': (0, 0, 'tag'), 'ffreep': (1, 0, 'tag'),
    'fninit': (0, 0, 'reset'), 'finit': (0, 0, 'reset'),
    **{m: (0, 0, 'env') for m in ('fnsave', 'fsave', 'frstor', 'fldenv', 'fnstenv', 'fstenv', 'fxsave', 'fxrstor')},
}
MMX_MNEMONICS_EXTRA = ('emms', 'femms')


def x87_effect(insn):
    """(pops, pushes, kind) of an x87 instruction, 'mmx' for MMX, None if it is neither."""
    m = insn.mnemonic
    if m in MMX_MNEMONICS_EXTRA or any(insn.reg_name(op.reg).startswith('mm')
                                       for op in insn.operands if op.type == 1 and op.reg):
        return (0, 0, 'mmx')
    if m in X87_EFFECTS:
        return X87_EFFECTS[m]
    if m.startswith('f') and m not in ('fs', 'femms'):
        return (0, 0, 'unknown')
    return None


# ---------------------------------------------------------------- facts
@dataclasses.dataclass
class Transfer:
    kind: str              # 'direct', 'import', 'indirect' (call) or 'ret', 'tail', 'tail-indirect' (exit)
    target: int | None = None


class Program:
    """The catalog's functions with decoded instructions, from the original image."""

    def __init__(self, catalog=ROOT/'work/lift_initial/functions.json', config=ROOT/'config/ac1_dx9.json'):
        import capstone
        import pefile
        self.config = json.loads(Path(config).read_text())
        pe = pefile.PE(self.config['binary'], fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.image = pe.get_memory_mapped_image()
        self.iat = {imp.address for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []) for imp in entry.imports}
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        doc = json.loads(Path(catalog).read_text())
        self.functions = {f['address']: f for f in doc['functions']}

    def decode(self, address):
        """{va: insn} for one function's recovered blocks."""
        out = {}
        for block in self.functions[address]['blocks']:
            for va, size in block['instructions']:
                if va not in out:
                    raw = self.image[va - self.base: va - self.base + size]
                    insn = next(self.md.disasm(raw, va, count=1))
                    if insn.size != size:
                        raise RuntimeError(f'instruction changed at 0x{va:08X}')
                    out[va] = insn
        return out

    def facts(self, address):
        """Compact per-instruction facts: [(start, successors, [(va, mnemonic, x87 effect, transfer)])]."""
        fn = self.functions[address]
        decoded = self.decode(address)
        return [(b['start'], tuple(b['successors']),
                 [(va, decoded[va].mnemonic, x87_effect(decoded[va]), self.transfer(decoded[va], fn))
                  for va, _ in b['instructions']]) for b in fn['blocks']]

    def transfer(self, insn, function):
        """What a call or a block-ending jump/ret transfers to."""
        return classify_transfer(insn, {b['start'] for b in function['blocks']}, self.iat,
                                 bool(function.get('register_switches')))


def classify_transfer(insn, block_starts, iat=frozenset(), register_switches=False):
    """Transfer of a call, ret or jmp; None for jumps that stay inside the function."""
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM
    m = insn.mnemonic
    if m in ('ret', 'retn', 'retf'):
        return Transfer('ret')
    op = insn.operands[0] if insn.operands else None
    if m == 'call':
        if op is not None and op.type == X86_OP_IMM:
            return Transfer('direct', op.imm & 0xFFFFFFFF)
        if op is not None and op.type == X86_OP_MEM and not op.mem.base and not op.mem.index \
                and (op.mem.disp & 0xFFFFFFFF) in iat:
            return Transfer('import', op.mem.disp & 0xFFFFFFFF)
        return Transfer('indirect')
    if m == 'jmp':
        if op is not None and op.type == X86_OP_IMM:
            target = op.imm & 0xFFFFFFFF
            return None if target in block_starts else Transfer('tail', target)
        # A jump table (memory with an index, or a recovered register switch) stays inside.
        return None if register_switches or (op is not None and op.type == X86_OP_MEM and op.mem.index) \
            else Transfer('tail-indirect')
    return None


def linear_facts(insns):
    """Compact facts for straight-line decoded code (tests): leaders split blocks at branches."""
    from capstone.x86 import X86_OP_IMM
    by_va = {i.address: i for i in insns}
    leaders = {insns[0].address}
    for i, insn in enumerate(insns):
        m = insn.mnemonic
        if m.startswith('j') or m.startswith('loop') or m in ('ret', 'retn'):
            if i + 1 < len(insns): leaders.add(insns[i + 1].address)
            for op in insn.operands:
                if op.type == X86_OP_IMM and (op.imm & 0xFFFFFFFF) in by_va: leaders.add(op.imm & 0xFFFFFFFF)
    starts = sorted(leaders); blocks = []
    for k, start in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else None
        body = [i for i in insns if i.address >= start and (end is None or i.address < end)]
        last = body[-1]; m = last.mnemonic; succ = []
        targets = [op.imm & 0xFFFFFFFF for op in last.operands if op.type == X86_OP_IMM]
        if m.startswith('j') or m.startswith('loop'):
            succ += [t for t in targets if t in by_va]
        if not (m == 'jmp' or m in ('ret', 'retn')) and end is not None:
            succ.append(end)
        blocks.append((start, tuple(succ), [(i.address, i.mnemonic, x87_effect(i), classify_transfer(i, set(starts)))
                                           for i in body]))
    return blocks


# ---------------------------------------------------------------- x87 depth
@dataclasses.dataclass
class X87Region:
    """Whole-function x87 stack tracking for one function.

    TOP is tracked as (anchor, d): `anchor` names the point where TOP was last
    read from the struct (function entry, after a transfer of unknown stack
    effect, or a block whose predecessors disagree) and d is the number of
    values pushed since. Within one anchor's region st(i) is the fixed
    physical slot (i - d) mod 8 relative to the anchor's TOP, so a value keeps
    its identity across blocks, joins and loops (FXCH swaps two slots) and TOP
    is never reloaded.
    """
    address: int
    status: str                    # 'none' (no x87), 'ok', or 'unsupported: <reason>'
    blocks: int = 0
    anchors: list = dataclasses.field(default_factory=list)       # ('entry'|'call'|'join', va)
    block_state: dict = dataclasses.field(default_factory=dict)   # block start -> (anchor, d)
    delta: int | None = None        # net depth change at every return (None: unknown)
    x87_instructions: int = 0
    kinds: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    call_delta: dict = dataclasses.field(default_factory=dict)    # call or tail-jump va -> net depth or None

    def lowering(self):
        """What native_x87_function needs for region mode, or None when this function must keep block anchors."""
        if self.status != 'ok' or self.kinds['rotate'] or self.kinds['tag']:
            return None
        return dict(labels={start: (anchor[0] == 'join', d) for start, (anchor, d) in self.block_state.items()},
                    calls=dict(self.call_delta))


def x87_region(address, facts, delta_of):
    """Prove a static TOP offset at every instruction of one function, or say why not."""
    blocks = {start: (succ, insns) for start, succ, insns in facts}
    result = X87Region(address, 'none', blocks=len(blocks))
    for _, _, insns in facts:
        for va, m, effect, t in insns:
            if effect:
                result.x87_instructions += 1; result.kinds[effect[2]] += 1
    bad = [k for k in ('mmx', 'env', 'reset', 'tag', 'unknown') if result.kinds[k]]
    state_in = {address: (('entry', address), 0)}
    conflict, returns, anchors = set(), set(), {('entry', address)}
    order = collections.deque([address]); queued = {address}
    if address not in blocks:
        return dataclasses.replace(result, status='unsupported: entry block missing')
    while order:
        start = order.popleft(); queued.discard(start)
        succ_list, insns = blocks[start]
        anchor, d = (('join', start), 0) if start in conflict else state_in[start]
        for va, m, effect, t in insns:
            if effect and effect[2] in ('stack', 'rotate', 'tag'):
                d += effect[1] - effect[0]
            if t is None:
                continue
            if t.kind in ('direct', 'import', 'indirect'):
                delta = delta_of(t.target) if t.kind == 'direct' else None
                result.call_delta[va] = delta
                if delta is None:
                    anchor, d = ('call', va), 0; anchors.add(anchor)
                else:
                    d += delta
            elif t.kind == 'ret':
                returns.add((anchor, d))
            elif t.kind == 'tail':
                delta = delta_of(t.target)
                result.call_delta[va] = delta
                returns.add((anchor, d + delta) if delta is not None else (('unknown', va), 0))
            elif t.kind == 'tail-indirect':
                returns.add((('unknown', va), 0))
        for succ in succ_list:
            if succ not in blocks:
                return dataclasses.replace(result, status=f'unsupported: successor 0x{succ:08X} outside the CFG')
            new = (anchor, d)
            old = state_in.get(succ)
            if succ in conflict or old == new:
                continue
            if old is None:
                state_in[succ] = new
            else:
                conflict.add(succ); anchors.add(('join', succ))
            if succ not in queued:
                order.append(succ); queued.add(succ)
    result.block_state = {s_: (('join', s_), 0) if s_ in conflict else state_in[s_] for s_ in state_in}
    if len(returns) == 1:
        (anchor, d), = returns
        result.delta = d if anchor == ('entry', address) else None
    elif not returns:
        result.delta = 0          # never returns
    result.anchors = sorted(anchors)
    if not result.x87_instructions:
        return result
    if bad:
        result.status = 'unsupported: ' + ', '.join(bad)
    else:
        result.status = 'ok'
    return result


def x87_deltas(program, facts):
    """Net x87 depth change of each function's returns, to a fixed point over the call graph.

    Starts optimistic at 0 for every function (recursion stays consistent) and
    revisits callers when a callee's summary changes. A summary may change from
    0 to one other value and then only to unknown (None), so this terminates.
    """
    delta = dict.fromkeys(facts, 0)
    changes = collections.Counter()
    callers = collections.defaultdict(set)
    for a, f in facts.items():
        for _, _, insns in f:
            for _, _, _, t in insns:
                if t and t.kind in ('direct', 'tail') and t.target in facts:
                    callers[t.target].add(a)
    queue = collections.deque(facts); queued = set(facts)
    while queue:
        a = queue.popleft(); queued.discard(a)
        if delta[a] is None:
            continue
        r = x87_region(a, facts[a], lambda t: delta.get(t))
        new = None if r.status.startswith('unsupported: successor') or r.status.startswith('unsupported: entry') else r.delta
        if new != delta[a]:
            changes[a] += 1
            delta[a] = new if changes[a] == 1 else None
            for c in callers[a]:
                if c not in queued:
                    queue.append(c); queued.add(c)
    return delta


# ---------------------------------------------------------------- census
def x87_census(args):
    program = Program()
    print(f'{len(program.functions)} functions; decoding and summarizing x87 stack effects ...', flush=True)
    facts = {}
    for i, a in enumerate(program.functions):
        facts[a] = program.facts(a)
        if i % 20000 == 0: print(f'  decoded {i}', flush=True)
    delta = x87_deltas(program, facts)
    profile = {}
    if args.profile:
        p = json.loads((ROOT/'work/codegen_lab/profiles'/args.profile/'profile.json').read_text())
        profile = {int(k, 16): v / p['total'] for k, v in p['functions'].items()}
    native = set()
    if args.native_list:
        native = {int(x, 16) for x in Path(args.native_list).read_text().split()}
    rows = []
    for a in program.functions:
        r = x87_region(a, facts[a], lambda t: delta.get(t))
        if r.status == 'none':
            continue
        mnems = [m for _, _, insns in facts[a] for _, m, _, _ in insns]
        calls = sum(m == 'call' for m in mnems)
        branches = sum(m.startswith('j') or m.startswith('loop') for m in mnems)
        rows.append(dict(va=f'0x{a:08X}', status=r.status, blocks=r.blocks, anchors=len(r.anchors),
                         join_anchors=sum(1 for k, _ in r.anchors if k == 'join'),
                         call_anchors=sum(1 for k, _ in r.anchors if k == 'call'),
                         calls=calls, branches=branches, x87=r.x87_instructions, kinds=dict(r.kinds),
                         delta=r.delta, weight=profile.get(a, 0.0), native=a in native))
    out = ROOT/'work/codegen_lab/x87_census.json'
    out.write_text(json.dumps(dict(deltas_unknown=sum(v is None for v in delta.values()), functions=rows), indent=1))
    by = collections.Counter(); w = collections.Counter()
    for r in rows:
        key = r['status'] if not r['status'].startswith('unsupported') else r['status'][:60]
        by[key] += 1; w[key] += r['weight']
    print(f'{len(rows)} functions with x87 instructions; {sum(v is None for v in delta.values())} '
          f'functions with unknown net depth')
    for k, n in by.most_common():
        print(f'  {n:6d}  {100*w[k]:6.2f}% of profiled cycles  {k}')
    ok = [r for r in rows if r['status'] == 'ok']
    single = [r for r in ok if r['anchors'] == 1]
    print(f'ok: {len(single)} need one anchor (entry only); {len(ok) - len(single)} need more '
          f'(calls of unknown effect: {sum(r["call_anchors"] > 0 for r in ok)}, joins that disagree: '
          f'{sum(r["join_anchors"] > 0 for r in ok)})')
    print(f'TOP anchors today ~ blocks + branches + calls + returns: {sum(r["blocks"] + r["calls"] for r in ok)}; '
          f'whole-function: {sum(r["anchors"] for r in ok)}')
    if native:
        nat = [r for r in rows if r['native']]
        print(f'native-lowered now: {len(nat)}; of them ok: {sum(r["status"] == "ok" for r in nat)} '
              f'({100*sum(r["weight"] for r in nat if r["status"] == "ok"):.2f}% of cycles), '
              f'with rotate/tag/reset: {sum(bool(r["kinds"].get("rotate") or r["kinds"].get("tag")) for r in nat)}')
    print(f'details: {out.relative_to(ROOT)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('x87-census'); s.add_argument('--profile'); s.add_argument('--native-list')
    args = parser.parse_args()
    {'x87-census': x87_census}[args.cmd](args)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
