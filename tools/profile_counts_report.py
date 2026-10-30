#!/usr/bin/env python3
"""Exact selected-entry counts between captured Present boundaries.

Use only on a dedicated --profile-count-entries build with matching runtime
AC1_PROFILE_COUNTS, AC1_PROFILE_FRAMES=1 and no sampling. Counts include all
guest threads; no per-call timer is inserted.
"""
import argparse
import json
from pathlib import Path
import statistics
import struct

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture',type=Path)
    p.add_argument('--from-present',type=int,default=1300)
    p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    captures=[(f,json.loads(f.read_text())) for f in a.capture.glob('raw_*.json')]
    latest=max(d['run_start'] for _,d in captures)
    captures=sorted(((f,d) for f,d in captures if d['run_start']==latest),key=lambda x:x[1]['start'])
    hz=captures[0][1]['frequency']; frames={}; targets=None; counts={}
    for f,d in captures:
        if d['errors'] or d['frame_dropped']: raise ValueError('capture has errors or dropped frame boundaries')
        if targets is None: targets=d['count_targets']
        assert targets==d['count_targets']
        start=d['present_count']-len(d['frames'])
        frames.update({start+i:t for i,t in enumerate(d['frames'])})
        fmt=struct.Struct('<'+'Q'*(1+len(targets)))
        for n,*row in fmt.iter_unpack(f.with_name(f.stem.replace('raw_','counts_')+'.bin').read_bytes()): counts[n]=row
    low=frames[a.from_present]
    if max(frames.values())<low+a.seconds*hz: raise ValueError('incomplete selected count window')
    last=next(n for n,t in sorted(frames.items()) if t>=low+a.seconds*hz)
    selected=list(range(a.from_present,last+1))
    if any(n not in frames or n not in counts for n in selected): raise ValueError('missing frame/count boundary')
    result={'first_present':selected[0],'last_present':selected[-1],
            'seconds':(frames[selected[-1]]-low)/hz,'frames':len(selected)-1,'entries':[]}
    for i,va in enumerate(targets):
        series=[counts[b][i]-counts[n][i] for n,b in zip(selected,selected[1:])]
        assert all(n>=0 for n in series)
        result['entries'].append({'va':f'{va:08X}','total':sum(series),'per_frame_mean':statistics.mean(series),
                                  'per_frame_median':statistics.median(series),'min':min(series),'max':max(series)})
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__': main()
