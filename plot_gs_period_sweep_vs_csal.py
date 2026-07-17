#!/usr/bin/env python3
"""Stacked bar: GS decision-period sweep vs CSAL.

GS configs (auto-derived util_step = (segs * seg_pages) / total_cache_pages):
  segs in {1, 2, 4, 8, 16, 32}, EWMA hl=1572864, scale=2, dwpd1to2_4x trace.
Bars stack r*flush (bottom) + comp (top). CSAL is a constant reference
(same value repeated at every r) parsed from ftl0_20260225_082514.csv.
"""
import os, re, csv, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSAL_CSV   = "/home/sejun000/ssd_waf/ftl0_20260225_082514.csv"
WARMUP_MB  = 6 * 1024 * 1024            # 6 TiB
WARMUP_BYTES = 6 * 1024**4
BLOCK_BYTES  = 4096
RS         = [2, 4, 6, 8, 10]
PERIODS    = [1, 2, 4, 8]
KEY_RE     = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TAG_FMT    = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_segs{p}.stat_pr{r}"

def parse_csal():
    if not os.path.exists(CSAL_CSV):
        return None
    with open(CSAL_CSV) as f:
        wl = ll = None
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
    e = last[1] - warm_first[1]
    c = last[2] - warm_first[2]
    return e / h, c / h

def gather():
    out = {}
    for p in PERIODS:
        for r in RS:
            path = TAG_FMT.format(p=p, r=r)
            if not os.path.exists(path):
                print(f"missing: {path}", file=sys.stderr); continue
            v = parse_stat(path)
            if v is not None:
                out[(f"GS segs={p}", r)] = v
    csal = parse_csal()
    if csal is not None:
        for r in RS:
            out[("CSAL", r)] = csal
    return out

def main():
    data   = gather()
    labels = [f"GS segs={p}" for p in PERIODS] + ["CSAL"]
    n      = len(labels)
    xs     = np.arange(len(RS))
    bw     = 0.12

    flush_color = "#4C72B0"
    comp_color  = "#DD8452"

    fig, ax = plt.subplots(figsize=(15, 6.2))
    for i, lab in enumerate(labels):
        flush = []; comp = []
        for r in RS:
            v = data.get((lab, r))
            if v is None:
                flush.append(0); comp.append(0)
            else:
                fl, cp = v
                flush.append(r * fl)
                comp.append(cp)
        flush = np.array(flush); comp = np.array(comp)
        off = (i - (n - 1) / 2.0) * bw
        ax.bar(xs + off, flush, bw,
               color=flush_color, edgecolor="black", linewidth=0.4,
               label="Flush Cost" if i == 0 else None)
        ax.bar(xs + off, comp, bw, bottom=flush,
               color=comp_color, edgecolor="black", linewidth=0.4,
               label="GC Cost" if i == 0 else None)
        for x, fl, cp in zip(xs + off, flush, comp):
            tot = fl + cp
            if tot > 0:
                ax.text(x, tot + 0.04, f"{tot:.2f}",
                        ha="center", va="bottom", fontsize=11, rotation=90)
            ax.text(x, -0.02, lab.replace("GS ", "").replace("segs=", "s"),
                    ha="center", va="top", fontsize=11, rotation=45,
                    transform=ax.get_xaxis_transform())

    ax.set_xticks(xs)
    ax.set_xticklabels([f"r={r}" for r in RS], y=-0.13, fontsize=16)
    ax.tick_params(axis="y", labelsize=14)
    ax.set_xlabel("periodic_ratio  r", labelpad=28, fontsize=18)
    ax.set_ylabel("Normalized TEC", fontsize=18)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=14, loc="upper left", framealpha=0.9, ncol=2)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_period_sweep_vs_csal.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

    print("\n--- summary (rows = config, cols = r) ---")
    for lab in labels:
        row = []
        for r in RS:
            v = data.get((lab, r))
            row.append(f"{(r*v[0]+v[1]):.3f}" if v else "  -  ")
        print(f"{lab:14s} | " + "  ".join(row))

if __name__ == "__main__":
    main()
