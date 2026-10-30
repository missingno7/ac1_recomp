#!/usr/bin/env python3
"""Diff inclusive entry/tree costs; retain denominators and overlap explicitly."""
import argparse
import json
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('menu',type=Path)
    p.add_argument('gameplay',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    menu,game=(json.loads(f.read_text()) for f in (a.menu,a.gameplay))
    result={}
    lines=['Gameplay minus menu; CPU percentage points, overlapping inclusive entries.']
    for key in ('inclusive','self_with_host','self_body','trees'):
        identity=lambda r: tuple(r['path']) if key=='trees' else r['va']
        m={identity(r):r for r in menu[key]}; g={identity(r):r for r in game[key]}
        rows=[]
        for va in m.keys()|g.keys():
            before=m.get(va,{}).get('cpu_percent',0); after=g.get(va,{}).get('cpu_percent',0)
            menu_ms=m.get(va,{}).get('ms',0)/max(menu['frames']-1,1)
            game_ms=g.get(va,{}).get('ms',0)/max(game['frames']-1,1)
            rows.append({'path' if key=='trees' else 'va':va,'menu_percent':before,
                         'gameplay_percent':after,'delta_pp':after-before,
                         'menu_ms_per_frame':menu_ms,'gameplay_ms_per_frame':game_ms,
                         'delta_ms_per_frame':game_ms-menu_ms})
        rows.sort(key=lambda r:-r['delta_pp']); result[key]=rows
        lines.append(key)
        for r in rows[:50]:
            label=' > '.join(r['path']) if key=='trees' else r['va']
            lines.append(f"  {r['delta_pp']:+7.3f} pp ({r['menu_percent']:7.3f} -> {r['gameplay_percent']:7.3f}) {label}")
    a.output.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
    a.output.with_suffix('.txt').write_text('\n'.join(lines)+'\n')
    print('\n'.join(lines[:55]))

if __name__=='__main__': main()
