#!/usr/bin/env python3
# Compare ALL policies near 100 TB host writes (hi & mid).
#   metrics @ matched host front:  cold_host(col3), cold_nand(col4), coldWAF=col4/col3,
#     comp = compacted_blocks*4096 (REFLASH/LRU; FIFO=0, no compactor),
#     U_B  = global_valid_blocks*4096/total_cache_size,
#     TEC  = host + comp + R*cold_nand   (R=8.64 = true QLC/cache cost ratio, fixed for ALL runs)
#   waf.log cols: 0=host(write_size_to_cache) 1=col2 2=cold_host 3=cold_nand
import bisect
BLK = 4096
TB  = 10**12          # decimal TB (user said "100TB 바이트")
TIB = 2**40
R   = 8.64

RUNS = {
 'hi': [
   ('REFLASH r2.88', 'dwpdhi_512_inj10_r288_v2', 'cb'),
   ('REFLASH r8.64', 'dwpdhi_512_inj10_r864_v2', 'cb'),
   ('LOG_FIFO',      'dwpdhi_512_inj10_logfifo', 'fifo'),
   ('LRU',           'dwpdhi_512_inj10_lru',     'cb'),
 ],
 'mid': [
   ('REFLASH r2.88', 'dwpdmid_512_inj10_r288_v2', 'cb'),
   ('REFLASH r8.64', 'dwpdmid_512_inj10_r864_v2', 'cb'),
   ('LOG_FIFO',      'dwpdmid_512_inj10_logfifo', 'fifo'),
   ('LRU',           'dwpdmid_512_inj10_lru',     'cb'),
 ],
}

def load_waf(tag):
    H, CH, CN = [], [], []
    with open(tag + ".waf.log") as f:
        for ln in f:
            t = ln.split()
            if len(t) < 4:
                continue
            H.append(float(t[0])); CH.append(float(t[2])); CN.append(float(t[3]))
    return H, CH, CN

def load_stat(tag):
    """return (W list, dict-rows) -> arrays of write_size, compacted_blocks, valid, cache_size"""
    W, CB, GV, TC = [], [], [], []
    try:
        f = open(tag + ".stat")
    except FileNotFoundError:
        return None
    with f:
        for ln in f:
            tk = ln.split()
            d = {}
            for i, t in enumerate(tk):
                if t.endswith(':') and i + 1 < len(tk):
                    d[t[:-1]] = tk[i + 1]
            if 'write_size_to_cache' not in d:
                continue
            W.append(float(d['write_size_to_cache']))
            CB.append(float(d.get('compacted_blocks', 0)))
            GV.append(float(d.get('global_valid_blocks', 0)))
            TC.append(float(d.get('total_cache_size', 0)))
    return W, CB, GV, TC

def nearest(xs, target):
    i = bisect.bisect_left(xs, target)
    if i <= 0: return 0
    if i >= len(xs): return len(xs) - 1
    return i if abs(xs[i] - target) < abs(xs[i-1] - target) else i - 1

CACHE = {}
def metrics(tag, kind, target):
    if tag not in CACHE:
        CACHE[tag] = (load_waf(tag), load_stat(tag))
    (H, CH, CN), st = CACHE[tag]
    i = nearest(H, target)
    host, ch, cn = H[i], CH[i], CN[i]
    coldwaf = cn / ch if ch > 0 else 0.0
    comp = 0.0; ub = float('nan')
    if kind == 'cb' and st:
        W, CB, GV, TC = st
        j = nearest(W, target)
        comp = CB[j] * BLK
        if TC[j] > 0:
            ub = GV[j] * BLK / TC[j]
    tec = host + comp + R * cn
    return dict(front=host, cold_host=ch, cold_nand=cn, coldwaf=coldwaf,
               comp=comp, ub=ub, tec=tec)

def maxfront(tag):
    if tag not in CACHE:
        CACHE[tag] = (load_waf(tag), load_stat(tag))
    return CACHE[tag][0][0][-1]

def show(wl, target, label, include_lru=True):
    runs = [r for r in RUNS[wl] if include_lru or r[2] != 'fifo' or True]
    if not include_lru:
        runs = [r for r in RUNS[wl] if r[0] != 'LRU']
    print("\n" + "=" * 96)
    print("  [%s]  workload=%s   target front = %.2f TB (%.2f TiB)" %
          (label, wl, target / TB, target / TIB))
    print("  %-14s %8s %8s %8s %7s %8s %7s %9s %8s" %
          ("policy", "front_TB", "cHost", "cNand", "coldWAF", "comp_TB", "U_B", "TEC_TB", "dTEC%"))
    base = None
    rows = []
    for name, tag, kind in runs:
        m = metrics(tag, kind, target)
        rows.append((name, m))
    # baseline for dTEC% = LOG_FIFO
    for name, m in rows:
        if name == 'LOG_FIFO':
            base = m['tec']
    for name, m in rows:
        dtec = (m['tec'] / base - 1) * 100 if base else float('nan')
        ubs = ("%.3f" % m['ub']) if m['ub'] == m['ub'] else "  -  "
        print("  %-14s %8.2f %8.2f %8.2f %7.3f %8.2f %7s %9.2f %+8.1f" %
              (name, m['front']/TB, m['cold_host']/TB, m['cold_nand']/TB,
               m['coldwaf'], m['comp']/TB, ubs, m['tec']/TB, dtec))
    print("  (dTEC%% vs LOG_FIFO baseline;  TEC = host + comp + %.2f*cold_nand)" % R)

print("max host front reached (TB) :")
for wl in ('hi', 'mid'):
    for name, tag, kind in RUNS[wl]:
        try:
            print("   %-5s %-14s %.2f TB (%.2f TiB)" % (wl, name, maxfront(tag)/TB, maxfront(tag)/TIB))
        except Exception as e:
            print("   %-5s %-14s ERR %s" % (wl, name, e))

for wl in ('hi', 'mid'):
    # (A) common front = min over all 4 of their max front (= LRU terminal)
    common = min(maxfront(tag) for _, tag, _ in RUNS[wl])
    show(wl, common, "A: 4-way @ common front (LRU cap, closest-to-100TB w/ all 4)", include_lru=True)
    # (B) exactly 100 TB, REFLASH x2 + FIFO (LRU can't reach)
    show(wl, 100 * TB, "B: 100 TB exact (LRU excluded — short)", include_lru=False)
