#!/usr/bin/env python3
# Gud(predict) vs compact_avg[t+1](realized), conditioned on decision==RAISE,
# then STRATIFIED by what actually happened in period t->t+1 (from cumulative
# counters logged each tick):
#   comp_cum (col17)      cumulative compacted_blocks  -> dComp
#   evict_cum (col18)     cumulative evicted_blocks    -> dEvict (flush)
#   ex_high_valid (col22) cumulative 0.95-override     -> dHi   (RAISE->flush override)
# Hypothesis #2: the 0.95 override turns a RAISE into a flush, so compact_avg
# stays stale -> Gud looks over-predicting. Test: does pure-GC (dComp>0 &
# dEvict==0) bring mean Gud / mean compact_avg closer to 1 and tighten r?
import sys, math

def pearson(a, b):
    n = len(a)
    if n < 3: return float('nan')
    ma=sum(a)/n; mb=sum(b)/n
    sa=sum((x-ma)**2 for x in a); sb=sum((y-mb)**2 for y in b)
    if sa==0 or sb==0: return float('nan')
    return sum((a[i]-ma)*(b[i]-mb) for i in range(n))/math.sqrt(sa*sb)

def load(path):
    with open(path) as f:
        h=f.readline().split(); idx={n:i for i,n in enumerate(h)}
        rows=[ln.split() for ln in f if len(ln.split())>=len(h)]
    return idx, rows

def rpt(tag, g, c):
    n=len(g)
    if n<5:
        print(f"    {tag:22s} n={n:5d}  (too few)"); return
    mg=sum(g)/n; mc=sum(c)/n
    above=sum(1 for i in range(n) if g[i]>c[i])
    print(f"    {tag:22s} n={n:5d}  r={pearson(g,c):+.3f}  "
          f"meanGud={mg:.4f} meanCompact={mc:.4f} ratio={mg/mc:.3f} Gud>next={100*above/n:.0f}%")

for path in sys.argv[1:]:
    idx, rows = load(path)
    iG,iC,iD = idx['G_ud'],idx['compact_avg'],idx['decision']
    iCM,iEV,iHI = idx['comp_cum'],idx['evict_cum'],idx['ex_high_valid']
    cats = {k:([],[]) for k in
            ('RAISE all','  pureGC dC>0 dE=0','  GC+flush dC>0 dE>0','  noGC dC=0','  override dHi>0')}
    for t in range(len(rows)-1):
        if rows[t][iD] != 'RAISE': continue
        nxt=float(rows[t+1][iC])
        if nxt<=0.0: continue
        gt=float(rows[t][iG])
        dC=float(rows[t+1][iCM])-float(rows[t][iCM])
        dE=float(rows[t+1][iEV])-float(rows[t][iEV])
        dH=float(rows[t+1][iHI])-float(rows[t][iHI])
        def push(k): cats[k][0].append(gt); cats[k][1].append(nxt)
        push('RAISE all')
        if dC>0 and dE==0: push('  pureGC dC>0 dE=0')
        elif dC>0 and dE>0: push('  GC+flush dC>0 dE>0')
        elif dC==0: push('  noGC dC=0')
        if dH>0: push('  override dHi>0')
    print(f"\n=== {path.split('/')[-1]} ===")
    for k in ('RAISE all','  pureGC dC>0 dE=0','  GC+flush dC>0 dE>0','  noGC dC=0','  override dHi>0'):
        rpt(k, *cats[k])
