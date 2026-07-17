#!/usr/bin/env python3
"""Compare measured G/F (gsdec.log) against dp.0.XX oracle.

Oracle = WIN=32 moving avg of (delta_compacted_blocks / delta_host_write_blocks)
         and (delta_evicted_blocks  / delta_host_write_blocks) per dp.0.XX file.

For each gsdec record at (ts_blocks, cur_util, tgt_after):
  - measured G_u  ↔ oracle G at U_B=cur_util,  host=ts*4096
  - measured G_ud ↔ oracle G at U_B=tgt_after, host=ts*4096
  - same for F.
Error per record = |measured - oracle|.
Then smooth the error trace with WIN=32 and report mean.
"""
import os, re, glob, sys
import numpy as np

WIN = 32
DP_DIR = "cb_11_dwpd2"
PAGE_BYTES = 4096
SEGS = [1, 2, 4, 8, 16]
GSDEC_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}_pr864.gsdec.log"

# ---------- 1. Parse gsdec.log files ----------
def parse_gsdec(path):
    arr = []
    with open(path) as f:
        next(f)  # skip header
        for line in f:
            t = line.split()
            if len(t) < 18: continue
            ts        = int(t[0])
            G_u       = float(t[4])
            F_u       = float(t[5])
            G_ud      = float(t[6])
            F_ud      = float(t[7])
            cur_util  = float(t[11])
            tgt_after = float(t[13])
            arr.append((ts, cur_util, tgt_after, G_u, F_u, G_ud, F_ud))
    return np.array(arr, dtype=np.float64)

# ---------- 2. Parse dp.0.XX → oracle trace ----------
RE_LOG = re.compile(r"compacted_blocks:\s+(\d+).*write_size_to_cache:\s+(\d+)\s+evicted_blocks:\s+(\d+)")

def parse_dp(path):
    """Return arr(N,3): host_blocks, compacted, evicted."""
    out = []
    with open(path) as f:
        for line in f:
            if "LOG_GREEDY_COST_BENEFIT_11" not in line: break  # tail has non-LOG rows
            m = RE_LOG.search(line)
            if not m: continue
            cb   = int(m.group(1))
            wsbb = int(m.group(2))           # bytes
            eb   = int(m.group(3))
            out.append((wsbb // PAGE_BYTES, cb, eb))
    return np.array(out, dtype=np.float64)

def oracle_rates(dp):
    """WIN=32 backward moving average of (dCB/dHost) and (dEv/dHost).
    Returns host_blocks_array, G_rate_array, F_rate_array, all length N.
    For i<WIN, rate is undefined → np.nan.
    """
    host = dp[:,0]; cb = dp[:,1]; ev = dp[:,2]
    N = len(dp)
    G = np.full(N, np.nan); F = np.full(N, np.nan)
    for i in range(WIN, N):
        dh = host[i] - host[i-WIN]
        if dh <= 0: continue
        G[i] = (cb[i] - cb[i-WIN]) / dh
        F[i] = (ev[i] - ev[i-WIN]) / dh
    return host, G, F

# ---------- 3. Build oracle table keyed by U_B (rounded to 0.01) ----------
def load_oracles():
    """Map ub_key (int, util*100 rounded) → (host_array, G_array, F_array)."""
    o = {}
    for p in sorted(glob.glob(f"{DP_DIR}/dp.*")):
        name = os.path.basename(p)              # dp.0.42
        try: ub = float(name.split("dp.")[1])
        except: continue
        ub_key = int(round(ub * 100))
        dp = parse_dp(p)
        if len(dp) < WIN + 2: continue
        host, G, F = oracle_rates(dp)
        o[ub_key] = (host, G, F)
    return o

def lookup_oracle(oracles, ub, host_target):
    """Return (G_oracle, F_oracle) for given U_B (float) at host_target (blocks).
    U_B is rounded to nearest 0.01 within available range; out-of-range → np.nan.
    """
    keys = sorted(oracles.keys())
    ub_key = int(round(ub * 100))
    if ub_key < keys[0] or ub_key > keys[-1]:
        return np.nan, np.nan
    # closest key
    ub_key = min(keys, key=lambda k: abs(k - ub_key))
    host, G, F = oracles[ub_key]
    if host_target < host[0] or host_target > host[-1]:
        return np.nan, np.nan
    # nearest index
    idx = int(np.searchsorted(host, host_target))
    if idx == 0: idx = 1
    if idx >= len(host): idx = len(host) - 1
    # pick i with smaller |host[i] - target|
    if abs(host[idx-1] - host_target) < abs(host[idx] - host_target):
        idx -= 1
    return G[idx], F[idx]

def smooth(a, w):
    """Centered moving avg of length-w, ignoring NaNs; trims partial windows."""
    n = len(a)
    out = np.full(n, np.nan)
    for i in range(n):
        lo = max(0, i - w//2 + 1)
        hi = min(n, i + w//2 + 1)
        seg = a[lo:hi]
        seg = seg[~np.isnan(seg)]
        if len(seg) > 0:
            out[i] = seg.mean()
    return out

# ---------- main ----------
def main():
    print(f"# WIN={WIN}, mapping U_B → dp.0.XX (round to 0.01)")
    print(f"# host(blocks) ↔ write_size_to_cache/4096")
    oracles = load_oracles()
    ubs = sorted(oracles.keys())
    print(f"# loaded {len(oracles)} dp oracles, U_B keys: {ubs[0]/100:.2f}..{ubs[-1]/100:.2f}")

    print()
    hdr = "seg | n_rec | n_valid | G_u_err  G_ud_err  F_u_err  F_ud_err"
    print(hdr); print("-"*len(hdr))
    summary = []
    for s in SEGS:
        path = GSDEC_FMT.format(s=s)
        if not os.path.exists(path):
            print(f"seg={s}: file missing"); continue
        rec = parse_gsdec(path)
        N = len(rec)
        eGu  = np.full(N, np.nan); eFu  = np.full(N, np.nan)
        eGud = np.full(N, np.nan); eFud = np.full(N, np.nan)
        ora_Gu  = np.full(N, np.nan); ora_Fu  = np.full(N, np.nan)
        ora_Gud = np.full(N, np.nan); ora_Fud = np.full(N, np.nan)
        for i, r in enumerate(rec):
            ts, u, tgt, Gu, Fu, Gud, Fud = r
            host = ts  # ts is already in 4KB blocks; dp host_blocks = wsbb/4096
            Go_u,  Fo_u  = lookup_oracle(oracles, u,   host)
            Go_ud, Fo_ud = lookup_oracle(oracles, tgt, host)
            ora_Gu[i] = Go_u; ora_Fu[i] = Fo_u
            ora_Gud[i] = Go_ud; ora_Fud[i] = Fo_ud
            if not np.isnan(Go_u):  eGu[i]  = abs(Gu  - Go_u)
            if not np.isnan(Fo_u):  eFu[i]  = abs(Fu  - Fo_u)
            if not np.isnan(Go_ud): eGud[i] = abs(Gud - Go_ud)
            if not np.isnan(Fo_ud): eFud[i] = abs(Fud - Fo_ud)
        n_valid = int(np.sum(~np.isnan(eGud)))
        # WIN=32 smoothing on error trace, then mean
        sGu  = smooth(eGu,  WIN); sFu  = smooth(eFu,  WIN)
        sGud = smooth(eGud, WIN); sFud = smooth(eFud, WIN)
        mGu  = np.nanmean(sGu);  mFu  = np.nanmean(sFu)
        mGud = np.nanmean(sGud); mFud = np.nanmean(sFud)
        print(f" {s:2d} | {N:5d} | {n_valid:6d} | {mGu:.6f}  {mGud:.6f}  {mFu:.6f}  {mFud:.6f}")
        summary.append((s, N, n_valid, mGu, mGud, mFu, mFud))
        # dump per-record CSV
        out = f"A_oracle_err_seg{s}.csv"
        with open(out, "w") as f:
            f.write("ts,cur_util,tgt_after,G_u,F_u,G_ud,F_ud,"
                    "G_oracle_u,F_oracle_u,G_oracle_tgt,F_oracle_tgt,"
                    "err_Gu,err_Fu,err_Gud,err_Fud,"
                    "errsm_Gu,errsm_Fu,errsm_Gud,errsm_Fud\n")
            for i, r in enumerate(rec):
                ts, u, tgt, Gu, Fu, Gud, Fud = r
                f.write(f"{int(ts)},{u:.6f},{tgt:.6f},{Gu:.6f},{Fu:.6f},{Gud:.6f},{Fud:.6f},"
                        f"{ora_Gu[i]:.6f},{ora_Fu[i]:.6f},{ora_Gud[i]:.6f},{ora_Fud[i]:.6f},"
                        f"{eGu[i]:.6f},{eFu[i]:.6f},{eGud[i]:.6f},{eFud[i]:.6f},"
                        f"{sGu[i]:.6f},{sFu[i]:.6f},{sGud[i]:.6f},{sFud[i]:.6f}\n")
    # summary CSV
    with open("A_oracle_err_summary.csv", "w") as f:
        f.write("seg,n_rec,n_valid,mean_err_Gu,mean_err_Gud,mean_err_Fu,mean_err_Fud\n")
        for s, N, nv, gu, gud, fu, fud in summary:
            f.write(f"{s},{N},{nv},{gu:.6f},{gud:.6f},{fu:.6f},{fud:.6f}\n")
    print("\nwrote A_oracle_err_seg{1,2,4,8,16}.csv + A_oracle_err_summary.csv")

if __name__ == "__main__":
    main()
