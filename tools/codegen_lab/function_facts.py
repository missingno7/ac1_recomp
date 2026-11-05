"""Decoded x86 for lab analyses: the catalog's functions and their guest stack traffic."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


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


def stack_accesses(program, address):
    """Guest stack traffic of one function from the decoded x86.

    Forward dataflow of esp and ebp as offsets from esp at entry (None when
    unknown; a call or `and esp` starts a new base, so offsets compare only
    within it). Returns (accesses, escapes): accesses maps a va to
    [(is_store, frame offset, size)] for push, pop and memory operands based on
    a known esp or ebp; escapes lists the vas where an esp- or ebp-derived
    address leaves those registers (lea or mov into another register, or a
    frame-relative value pushed), after which other code may hold a frame
    pointer.
    """
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    fn = program.functions[address]
    decoded = program.decode(address)
    blocks = {b['start']: b for b in fn['blocks']}
    state_in = {address: (0, None)}
    accesses, escapes = {}, []
    order = collections.deque([address])
    seen = collections.Counter()
    while order:
        start = order.popleft()
        seen[start] += 1
        if seen[start] > 16: continue
        esp, ebp = state_in[start]
        for va, _ in blocks[start]['instructions']:
            insn = decoded[va]
            m = insn.mnemonic
            found = []
            name = lambda op: insn.reg_name(op.reg) if op.type == X86_OP_REG else None
            for k, op in enumerate(insn.operands):
                if op.type != X86_OP_MEM or m == 'lea' or op.mem.segment: continue
                base = insn.reg_name(op.mem.base) if op.mem.base else None
                frame = esp if base == 'esp' else ebp if base == 'ebp' else None
                if frame is not None and not op.mem.index:
                    written = k == 0 and m not in ('cmp', 'test', 'push', 'fld', 'fild', 'fadd', 'fmul', 'fsub', 'fdiv', 'fcom', 'fcomp')
                    both = k == 0 and m in ('add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'not', 'shl', 'shr', 'sar')
                    if written: found.append((True, frame + op.mem.disp, op.size))
                    if not written or both: found.append((False, frame + op.mem.disp, op.size))
            if m == 'push':
                if esp is not None: found.append((True, esp - 4, 4))
                if insn.operands and name(insn.operands[0]) == 'esp': escapes.append(va)
            elif m == 'pop' and esp is not None:
                found.append((False, esp, 4))
            if found: accesses[va] = found
            ops = insn.operands
            dst = name(ops[0]) if ops else None
            # Effects on esp and ebp.
            if m == 'push': esp = None if esp is None else esp - 4
            elif m == 'pop':
                esp = None if esp is None else esp + 4
                if dst == 'ebp': ebp = None
            elif m == 'call':
                # The callee's ret n is unknown: a new esp epoch, so slots compare only between calls.
                esp = None if esp is None else (1 << 40) + (va << 12)
            elif m == 'and' and dst == 'esp' and esp is not None:
                esp = (1 << 60) + (va << 12)   # realigned: a new frame base
            elif m in ('add', 'sub') and dst == 'esp' and ops[1].type == X86_OP_IMM and esp is not None:
                esp += ops[1].imm if m == 'add' else -ops[1].imm
            elif m == 'mov' and dst == 'ebp' and name(ops[1]) == 'esp': ebp = esp
            elif m == 'mov' and dst == 'esp' and name(ops[1]) == 'ebp': esp = ebp
            elif m == 'lea' and dst == 'esp' and ops[1].mem.base and insn.reg_name(ops[1].mem.base) == 'ebp' and ebp is not None:
                esp = ebp + ops[1].mem.disp
            elif dst == 'esp': esp = None
            elif dst == 'ebp': ebp = None
            if m == 'lea' and ops[1].mem.base and insn.reg_name(ops[1].mem.base) in ('esp', 'ebp') and dst not in ('esp', 'ebp'):
                escapes.append(va)
            elif m == 'mov' and name(ops[1]) in ('esp', 'ebp') and dst not in ('esp', 'ebp'):
                escapes.append(va)
        for succ in blocks[start]['successors']:
            if succ not in blocks: continue
            new = (esp, ebp)
            if succ not in state_in:
                state_in[succ] = new; order.append(succ)
            elif state_in[succ] != new:
                old = state_in[succ]
                merged = (old[0] if old[0] == new[0] else None, old[1] if old[1] == new[1] else None)
                if merged != old: state_in[succ] = merged; order.append(succ)
    return accesses, sorted(set(escapes))
