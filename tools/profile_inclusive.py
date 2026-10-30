#!/usr/bin/env python3
"""Inclusive guest trees from AC1_PROFILE=1 AC1_PROFILE_STACKS=1 captures.

Keeps flat samples readable by profile_report.py. All addresses are symbolized
after execution. CPU estimates weight per-thread mixes by OS CPU times over
complete intervals; wall trees include parked threads and must not be added
across guest threads. Recursive inclusive entries are counted once per sample.
"""
from __future__ import annotations
import argparse
import bisect
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct
from profile_report import SAMPLE, GUEST, category, load_symbols, syscall_parked, percentile, thread_cpu

def analyze(capture, exe, low_seconds=0, seconds=60, present=None):
    captures = [(p, json.loads(p.read_text())) for p in capture.glob('raw_*.json')]
    latest = max(d['run_start'] for _, d in captures)
    captures = sorted(((p,d) for p,d in captures if d['run_start']==latest), key=lambda x:x[1]['start'])
    origin, hz = captures[0][1]['run_start'], captures[0][1]['frequency']
    frames = sorted(f for _,d in captures for f in d['frames'])
    low = origin + low_seconds*hz
    if present is not None:
        if len(frames)<=present: raise ValueError('requested Present absent')
        low = max(low, frames[present])
    high = low+seconds*hz
    if captures[-1][1]['end']<high: raise ValueError('capture is shorter than requested window')
    # Exact OS CPU deltas can only cover full metadata intervals.
    bounds = [d for _,d in captures if low<=d['start'] and d['end']<=high]
    if not bounds: raise ValueError('no complete CPU counter intervals')
    previous = next((d for _,d in captures if d['end']==bounds[0]['start']), {})
    before = thread_cpu(previous)
    cpus = {tid:max(0,value-before.get(tid,0))/10000 for tid,value in thread_cpu(bounds[-1]).items()}
    rvas,names = load_symbols(exe,'C:/msys64/mingw64/bin/nm.exe')
    symbol_cache = {}
    def symbolize(pc, modules):
        if pc in symbol_cache: return symbol_cache[pc]
        for m in modules:
            if m['base']<=pc<m['base']+m['size']:
                mod = Path(m['path'].replace('\\','/')).name
                if mod.lower()==exe.name.lower():
                    i=bisect.bisect_right(rvas,pc-m['base'])-1
                    name=names[i] if i>=0 else '[unknown exe]'
                    match=GUEST.search(name)
                    result=(name,category(name),f'{int(match[1],16):08X}' if match else None)
                else: result=(mod+'+0x'+format(pc-m['base'],'x'),'external: '+mod,None)
                symbol_cache[pc]=result
                return result
        return ('[unknown image]','unknown',None)
    running=Counter(); observed=Counter(); guest_ids={}
    categories=defaultdict(Counter); inc=defaultdict(Counter); own=defaultdict(Counter); body=defaultdict(Counter)
    tree=defaultdict(Counter); paths=defaultdict(Counter); details=defaultdict(Counter)
    parked_tree=defaultdict(Counter); alltree=defaultdict(Counter)
    tree_self=defaultdict(Counter); inc_details=defaultdict(Counter)
    statuses=Counter(); suspensions=[]; coverage=Counter(); pause_ms=Counter()
    for path,d in captures:
        if d['end']<low or d['start']>high: continue
        fmt=struct.Struct('<IIQ'+'Q'*d['stack_depth'])
        raw=path.with_suffix('.bin').read_bytes()
        stackraw=path.with_name(path.stem.replace('raw_','stacks_')+'.bin').read_bytes()
        if len(raw)!=d['samples']*SAMPLE.size or len(stackraw)!=d['samples']*fmt.size:
            raise ValueError('raw/stack sample count mismatch')
        for sample,stack in zip(SAMPLE.iter_unpack(raw),fmt.iter_unpack(stackraw)):
            tick,rip,cycles,tid,guest,eip,flags,module,_=sample
            if not low<=tick<high: continue
            depth,status,suspend,*pcs=stack
            statuses[status]+=1; suspensions.append(suspend*1000/hz)
            pause_ms[tid]+=suspend*1000/hz
            leaf=symbolize(rip,d['modules'])
            chain=[]; previous_guest_name=''
            for pc in pcs[:depth]:
                n,cat,va=symbolize(pc,d['modules'])
                if va:
                    # Collapse a native/oracle body plus its public wrapper,
                    # while retaining real recursive L_VA activations.
                    wrapper=(chain and chain[-1]==va and n.startswith('L_') and
                             previous_guest_name.startswith(('ac1_translated_','ac1_native_')))
                    if not wrapper: chain.append(va)
                    previous_guest_name=n
            rootpath=tuple(reversed(chain))
            observed[tid]+=1; guest_ids[tid]=guest
            for i in range(1,len(rootpath)+1): alltree[tid][rootpath[:i]]+=1
            mod=d['modules'][module-1] if module else None
            parked=bool(flags&8 or syscall_parked(mod,rip))
            if parked:
                for i in range(1,len(rootpath)+1): parked_tree[tid][rootpath[:i]]+=1
            if not flags&1 or parked: continue
            running[tid]+=1; categories[tid][leaf[1]]+=1
            coverage['active']+=1; coverage['with_guest_chain']+=bool(chain)
            if guest: coverage['registered_guest_active']+=1; coverage['registered_with_chain']+=bool(chain)
            if leaf[2]: body[tid][leaf[2]]+=1
            if chain:
                own[tid][chain[0]]+=1
                details[chain[0]][leaf[1]]+=1
                paths[tid][rootpath]+=1
                tree_self[tid][rootpath]+=1
            for va in set(chain): inc[tid][va]+=1
            for va in set(chain): inc_details[tid][(va,leaf[1])]+=1
            for i in range(1,len(rootpath)+1): tree[tid][rootpath[:i]]+=1
    def weighted(mixes):
        result=Counter()
        for tid,mix in mixes.items():
            if running[tid]:
                for k,v in mix.items(): result[k]+=cpus.get(tid,0)*v/running[tid]
        return result
    counter_seconds=(bounds[-1]['end']-bounds[0]['start'])/hz
    # Expand CPU estimates to the full selected sampling window for ms/frame.
    # Percentages remain unchanged. Raw counter duration is reported separately.
    scale=seconds/counter_seconds
    cpus={tid:ms*scale for tid,ms in cpus.items()}
    total_cpu=sum(cpus.values())
    inclusive,selftime,bodies,trees,cats = map(weighted,(inc,own,body,tree,categories))
    tree_self_ms=weighted(tree_self)
    host_details=defaultdict(dict)
    for (va,cat),ms in weighted(inc_details).items(): host_details[va][cat]=ms
    def rows(counter):
        return [{'va':k,'ms':v,'cpu_percent':100*v/max(total_cpu,1),
                 'ms_per_frame':v/max(len(selected_frames)-1,1)} for k,v in counter.most_common()]
    selected_frames=[f for f in frames if low<=f<high]
    intervals=sorted((b-a)*1000/hz for a,b in zip(selected_frames,selected_frames[1:]))
    tids=[]
    for tid in observed:
        cpu_per_sample=cpus.get(tid,0)/max(running[tid],1)
        tids.append({'tid':tid,'guest':guest_ids[tid],'cpu_ms':cpus.get(tid,0),
                     'target_pause_upper_bound_ms':pause_ms[tid],
                     'target_pause_upper_bound_percent':pause_ms[tid]/(seconds*10),
                     'active':running[tid],'observed':observed[tid],
                     'inclusive':[{'va':va,'estimated_cpu_ms':n*cpu_per_sample,
                                   'wall_equivalent_percent':n*cpu_per_sample/(seconds*10)}
                                  for va,n in inc[tid].most_common()],
                     'cpu_trees':[{'path':list(p),'estimated_cpu_ms':n*cpu_per_sample,
                                   'wall_equivalent_percent':n*cpu_per_sample/(seconds*10),
                                   'self_cpu_ms':tree_self[tid][p]*cpu_per_sample}
                                  for p,n in tree[tid].most_common()],
                     'wall_tree':[{'path':list(p),'samples':n,'percent':100*n/observed[tid],
                                   'parked_samples':parked_tree[tid][p]} for p,n in alltree[tid].most_common()]})
    return {'window_seconds':seconds,'from_seconds':(low-origin)/hz,
            'capture_errors':captures[-1][1]['errors'],
            'dropped_samples':captures[-1][1]['dropped'],
            'dropped_frames':captures[-1][1]['frame_dropped'],
            'counter_window_seconds':(bounds[-1]['end']-bounds[0]['start'])/hz,
            'counter_from_seconds':(bounds[0]['start']-origin)/hz,
            'first_present':bisect.bisect_left(frames,low),'last_present':bisect.bisect_left(frames,high)-1,
            'frames':len(selected_frames),'fps':((len(selected_frames)-1)*hz/(selected_frames[-1]-selected_frames[0]) if len(selected_frames)>1 else 0),
            'frame_p50_p95_p99_ms':[percentile(intervals,q) for q in (.5,.95,.99)],
            'cpu_ms':total_cpu,'coverage':dict(coverage),'statuses':dict(statuses),
            'sampler_cpu_ms':(bounds[-1].get('sampler_cpu_100ns',0)-previous.get('sampler_cpu_100ns',0))/10000*scale,
            'suspend_ms_p50_p95_p99':[percentile(sorted(suspensions),q) for q in (.5,.95,.99)],
            'inclusive':rows(inclusive),'self_with_host':rows(selftime),'self_body':rows(bodies),
            'categories':dict(cats),'host_details_samples':dict(details),
            'inclusive_host_ms':dict(host_details),
            'trees':[{'path':list(k),'ms':v,'cpu_percent':100*v/max(total_cpu,1),
                      'self_ms':tree_self_ms[k]} for k,v in trees.most_common()],
            'threads':tids}

def text_report(d):
    lines=[f"Window {d['from_seconds']:.3f} + {d['window_seconds']:.3f}s; presents {d['first_present']}..{d['last_present']}; {d['fps']:.2f} FPS",
           f"CPU counters: {d['counter_from_seconds']:.3f} + {d['counter_window_seconds']:.3f}s; CPU={d['cpu_ms']:.1f}ms",
           f"Stack coverage {d['coverage']}; statuses {d['statuses']}; suspension p50/95/99 ms={d['suspend_ms_p50_p95_p99']}"]
    for title,key in [('Inclusive entries (overlap, recursion deduplicated)','inclusive'),('Self including host children','self_with_host'),('Flat self bodies','self_body')]:
        lines.append(title)
        for r in d[key][:45]: lines.append(f"  {r['cpu_percent']:7.3f}% {r['ms_per_frame']:8.3f} ms/frame  {r['va']}")
    lines.append('Inclusive subtree roots (overlapping paths)')
    for r in d['trees'][:100]: lines.append(f"  {r['cpu_percent']:7.3f}% {' > '.join(r['path'])}")
    return '\n'.join(lines)+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture',type=Path)
    p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--from-seconds',type=float,default=0)
    p.add_argument('--from-present',type=int)
    p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    d=analyze(a.capture,a.exe,a.from_seconds,a.seconds,a.from_present)
    a.output.with_suffix('.json').write_text(json.dumps(d,indent=2)+'\n')
    report=text_report(d)
    a.output.with_suffix('.txt').write_text(report)
    print(report)

if __name__=='__main__': main()
