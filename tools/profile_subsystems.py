#!/usr/bin/env python3
"""Group observed self paths under explicitly evidenced subsystem roots.

Root config: {group: {roots: [hex VA,...], evidence: text}}. Deepest matched
root gives disjoint attribution. Inclusive unions deduplicate multiple roots
of the same subsystem but can overlap other subsystems. No guessed RTTI names.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

def group(profile,config):
    root_groups={}
    for name,spec in config.items():
        for root in spec['roots']:
            root=f'{int(root,16):08X}'
            if root in root_groups: raise ValueError(f'ambiguous subsystem root {root}')
            root_groups[root]=name
    disjoint=Counter(); inclusive=Counter(); roots=Counter()
    total_self=0
    for row in profile['trees']:
        ms=row['self_ms']
        if not ms: continue
        total_self+=ms
        matches=[(va,root_groups[va]) for va in row['path'] if va in root_groups]
        if matches:
            va,name=matches[-1]; disjoint[name]+=ms; roots[va]+=ms
            for name in {name for _,name in matches}: inclusive[name]+=ms
        else: disjoint['other guest chains']+=ms
    disjoint['CPU without observed guest chain']=profile['cpu_ms']-total_self
    def rows(counter):
        return [{'name':name,'estimated_ms':ms,'cpu_percent':100*ms/max(profile['cpu_ms'],1),
                 'ms_per_frame':ms/max(profile['frames']-1,1)} for name,ms in counter.most_common()]
    return {'config':config,'inclusive_unions':rows(inclusive),'disjoint':rows(disjoint),'disjoint_roots':rows(roots)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile',type=Path); p.add_argument('config',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    result=group(json.loads(a.profile.read_text()),json.loads(a.config.read_text()))
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__': main()
