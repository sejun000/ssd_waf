#!/usr/bin/env python3
# Windowed stats over [LO, HI] TiB host writes (delta between two fronts).
#   default window = [10, 28] TiB  (= the "18 TiB writes" ending at the 28 TiB point).
#   per run @ window:  d_host, cold writes(d col3), QLC nand(d col4), cold WAF=dCN/dCH,
#     gc/comp writes (d compacted_blocks*4096; FIFO=0 no compactor),
#     cold-side GC (dCN-dCH),  TEC = d_host + comp + R*dCN  (R=8.64).
#   LRU excluded (front < HI).  waf.log cols: 0=host 2=cold_host 3=cold_nand.
import bisect, sys
BLK = 4096; TIB = 2**40; R = 8.64
LO = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
HI = float(sys.argv[2]) if len(sys.argv) > 2 else 28.0
lo_b, hi_b = LO * TIB, HI * TIB

RUNS = {
 'hi': [('REFLASH r2.88','dwpdhi_1t8t_r288','cb'),
        ('REFLASH r8.64','dwpdhi_1t8t_r864','cb'),
        ('LOG_FIFO',     'dwpdhi_1t8t_logfifo','fifo')],
 'mid':[('REFLASH r2.88','dwpdmid_1t8t_r288','cb'),
        ('REFLASH r8.64','dwpdmid_1t8t_r864','cb'),
        ('LOG_FIFO',     'dwpdmid_1t8t_logfifo','fifo')],
}

def load_waf(tag):
    H,CH,CN=[],[],[]
    with open(tag+".waf.log") as f:
        for ln in f:
            t=ln.split()
            if len(t)<4: continue
            H.append(float(t[0])); CH.append(float(t[2])); CN.append(float(t[3]))
    return H,CH,CN

def load_stat(tag):
    W,CB=[],[]
    try: f=open(tag+".stat")
    except FileNotFoundError: return None
    with f:
        for ln in f:
            tk=ln.split(); d={}
            for i,t in enumerate(tk):
                if t.endswith(':') and i+1<len(tk): d[t[:-1]]=tk[i+1]
            if 'write_size_to_cache' not in d: continue
            W.append(float(d['write_size_to_cache'])); CB.append(float(d.get('compacted_blocks',0)))
    return W,CB

def near(xs,t):
    i=bisect.bisect_left(xs,t)
    if i<=0: return 0
    if i>=len(xs): return len(xs)-1
    return i if abs(xs[i]-t)<abs(xs[i-1]-t) else i-1

print("window = [%.0f, %.0f] TiB host  (width %.0f TiB)   R=%.2f\n" % (LO,HI,HI-LO,R))
for wl in ('hi','mid'):
    print("="*104)
    print("  [%s]" % wl)
    print("  %-14s %7s %8s %8s %8s %9s %9s %10s %8s" %
          ("policy","dHost","coldW","cNand","coldWAF","gc/comp","coldGC","TEC","dTEC%"))
    rows=[]
    for name,tag,kind in RUNS[wl]:
        H,CH,CN=load_waf(tag)
        if H[-1] < hi_b:
            print("  %-14s  front only %.2f TiB < %.0f  -> SKIP" % (name,H[-1]/TIB,HI)); continue
        a,b=near(H,lo_b),near(H,hi_b)
        dH=H[b]-H[a]; dCH=CH[b]-CH[a]; dCN=CN[b]-CN[a]
        comp=0.0
        if kind=='cb':
            st=load_stat(tag)
            if st:
                W,CB=st; ja,jb=near(W,lo_b),near(W,hi_b); comp=(CB[jb]-CB[ja])*BLK
        waf=dCN/dCH if dCH>0 else 0.0
        tec=dH+comp+R*dCN
        rows.append((name,dH,dCH,dCN,waf,comp,dCN-dCH,tec))
    base=next((r[7] for r in rows if r[0]=='LOG_FIFO'),None)
    for name,dH,dCH,dCN,waf,comp,cgc,tec in rows:
        dt=(tec/base-1)*100 if base else float('nan')
        print("  %-14s %7.2f %8.2f %8.2f %8.3f %9.2f %9.2f %10.2f %+8.1f" %
              (name,dH/TIB,dCH/TIB,dCN/TIB,waf,comp/TIB,cgc/TIB,tec/TIB,dt))
    print("  (all TiB; gc/comp=cache compaction[REFLASH], coldGC=cNand-coldW[QLC GC]; dTEC%% vs FIFO)")
print()
