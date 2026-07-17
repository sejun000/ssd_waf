#!/usr/bin/env python3
"""GS v1 (ghost_sum += dt*rate, independent counter) vs v4 (ghost_sum =
compacted_blocks + dt*rate, anchored on real comp) vs CSAL.

X = segs.  r=8 only.  Stacked bars: Flush Cost (r*evict/host) + GC Cost
(comp/host).  Two bars per segs (v1, v4); CSAL drawn as a horizontal stacked
reference column on the right.
"""
import os, re, csv, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS   = [1, 2, 4, 8, 16, 32, 64, 128]
R         = 8
CSAL_CSV  = "/home/sejun000/ssd_waf/ftl0_20260225_082514.csv"
WARMUP_MB = 6 * 1024 * 1024
WARMUP_BYTES = 6 * 1024**4
BLOCK_BYTES  = 4096
KEY_RE    = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TAG_V1    = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_segs{p}.stat_pr{r}"
TAG_V4    = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{r}"
OUT_BASE  = "gs_v1_vs_v4_r{r}"

def parse_csal():
    if not os.path.exists(CSAL_CSV):
        return None
    wl = ll = None
    with open(CSAL_CSV) as f:
        for row in csv.DictReader(f):
            if float(row['host_write_MB']) < WARMUP_MB:
                wl = row
            ll = row
    if wl is None or ll is None:
        return None
    h = float(ll['host_write_MB'])     - float(wl['host_write_MB'])
    c = float(ll['cache_write_MB'])    - float(wl['cache_write_MB'])
    e = float(ll['backend_write_MB'])  - float(wl['backend_write_MB'])
    return e / h, c / h

def parse_stat(path):
    last = warm_first = None
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                wsc = int(kv["write_size_to_cache"])
                ev  = int(kv["evicted_blocks"])
                cp  = int(kv["compacted_blocks"])
            except KeyError:
                continue
            if wsc >= WARMUP_BYTES and warm_first is None:
                warm_first = (wsc, ev, cp)
            last = (wsc, ev, cp)
    if not warm_first or not last:
        return None
    h = (last[0] - warm_first[0]) / BLOCK_BYTES
    if h <= 0:
        return None
    return (last[1] - warm_first[1]) / h, (last[2] - warm_first[2]) / h

def gather():
    out = {}
    for p in PERIODS:
        for tag_fmt, label in [(TAG_V1, "v1"), (TAG_V4, "v4")]:
            path = tag_fmt.format(p=p, r=R)
            if not os.path.exists(path):
                print(f"missing: {path}", file=sys.stderr); continue
            v = parse_stat(path)
            if v is not None:
                out[(label, p)] = v
    csal = parse_csal()
    if csal is not None:
        out[("CSAL", None)] = csal
    return out

def main():
    data = gather()
    xs   = np.arange(len(PERIODS))
    bw   = 0.36

    fig, ax = plt.subplots(figsize=(13, 6))
    flush_v1 = "#4C72B0"
    flush_v4 = "#55A868"
    comp_col = "#DD8452"

    for i, lab in enumerate(["v1", "v4"]):
        flush = []; comp = []
        for p in PERIODS:
            v = data.get((lab, p))
            if v is None:
                flush.append(0); comp.append(0)
            else:
                fl, cp = v
                flush.append(R * fl)
                comp.append(cp)
        off = (i - 0.5) * bw
        flush = np.array(flush); comp = np.array(comp)
        ax.bar(xs + off, flush, bw,
               color=flush_v1 if lab == "v1" else flush_v4,
               edgecolor="black", linewidth=0.4,
               label=f"{lab} Flush Cost")
        ax.bar(xs + off, comp, bw, bottom=flush,
               color=comp_col, edgecolor="black", linewidth=0.4,
               label="GC Cost" if i == 0 else None)
        for x, fl, cp in zip(xs + off, flush, comp):
            tot = fl + cp
            if tot > 0:
                ax.text(x, tot + 0.05, f"{tot:.2f}",
                        ha="center", va="bottom", fontsize=10, rotation=90)

    csal_v = data.get(("CSAL", None))
    if csal_v is not None:
        csal_y = R * csal_v[0] + csal_v[1]
        ax.axhline(csal_y, color="#888", lw=1.2, ls="--",
                   label=f"CSAL (TEC={csal_y:.2f})")

    ax.set_xticks(xs)
    ax.set_xticklabels([f"s{p}" for p in PERIODS], fontsize=15)
    ax.tick_params(axis="y", labelsize=13)
    ax.set_xlabel("gs_decision_period_segs", fontsize=16)
    ax.set_ylabel("Normalized TEC", fontsize=16)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=12, loc="upper left", framealpha=0.9, ncol=2)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"{OUT_BASE.format(r=R)}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

    print("\n--- summary ---")
    hdr = f"{'segs':<6}" + "  " + f"{'v1 TEC':<8}" + "  " + f"{'v4 TEC':<8}" + "  Δ"
    print(hdr)
    for p in PERIODS:
        v1 = data.get(("v1", p)); v4 = data.get(("v4", p))
        a = (R * v1[0] + v1[1]) if v1 else None
        b = (R * v4[0] + v4[1]) if v4 else None
        if a is not None and b is not None:
            print(f"{p:<6}  {a:<8.3f}  {b:<8.3f}  {b-a:+.3f}")
    if csal_v is not None:
        print(f"CSAL    {R*csal_v[0]+csal_v[1]:<8.3f}")

if __name__ == "__main__":
    main()
