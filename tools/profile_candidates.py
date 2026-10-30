#!/usr/bin/env python3
"""Private disassembly/callgraph/RTTI evidence for measured inclusive roots."""
import argparse
import csv
import json
from pathlib import Path
import capstone
import pefile

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('entries',nargs='+',help='hex entry VAs')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--rtti',type=Path,default=Path('D:/Prog/ac1_wt_enginer/work/engine_render/virtual_methods.csv'))
    p.add_argument('--catalog',type=Path,default=Path('work/lift_initial/functions.json'))
    a=p.parse_args()
    config=json.loads(Path('config/ac1_dx9.json').read_text())
    pe=pefile.PE(config['binary'])
    image=pe.get_memory_mapped_image()
    base=pe.OPTIONAL_HEADER.ImageBase
    functions=json.loads(a.catalog.read_text())['functions']
    byva={f['address']:f for f in functions}
    methods={}
    if a.rtti.exists():
        for r in csv.DictReader(a.rtti.open()): methods.setdefault(int(r['method'],16),[]).append(r)
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
    a.output.mkdir(parents=True,exist_ok=True)
    evidence=[]
    for va in (int(s,16) for s in a.entries):
        if va not in byva: raise ValueError(f'{va:08X}: no catalog entry')
        f=byva[va]
        rows=[]
        for addr,size in sorted({tuple(i) for b in f['blocks'] for i in b['instructions']}):
            code=image[addr-base:addr-base+size]
            ins=list(md.disasm(code,addr))
            if len(ins)!=1 or ins[0].size!=size: raise ValueError(f'decode mismatch at {addr:08X}')
            i=ins[0]
            rows.append(f'{addr:08X}  {code.hex():20s} {i.mnemonic:8s} {i.op_str}')
        a.output.joinpath(f'{va:08X}.asm').write_text('\n'.join(rows)+'\n')
        evidence.append({'va':f'{va:08X}','size':f['size'],'instruction_count':f['instruction_count'],
                         'rtti':methods.get(va,[]),'calls':[f'{c:08X}' for c in f['calls_to']],
                         'callees_rtti':{f'{c:08X}':methods[c] for c in f['calls_to'] if c in methods},
                         'callers':[f"{g['address']:08X}" for g in functions if va in g['calls_to']]})
    a.output.joinpath('evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps([{k:v for k,v in e.items() if k!='callees_rtti'} for e in evidence],indent=2))

if __name__=='__main__': main()
