#!/usr/bin/env python3
"""Stacked bar: f = r*flush + comp for GD / TDELTA / GC / GS_thetaI / GD002.

NOTE on settings drift:
  - GD / TDELTA / GC (_fix): EWMA window = 0 -> default 6291456 blocks half-life.
  - GS (_fix, theta*i corr):    EWMA window = 3145728 blocks (default x 0.5).
  - GD002:                       prev_init05_ghost01/ snapshot (older code path).
The MA half-life mismatch is annotated in the legend.
"""
import os, re, csv, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSAL_CSV = "/home/sejun000/ssd_waf/ftl0_20260225_082514.csv"
WARMUP_MB = 6 * 1024 * 1024  # 6 TiB in MB

def parse_csal():
    if not os.path.exists(CSAL_CSV):
        return None
    with open(CSAL_CSV) as f:
        rd = csv.DictReader(f)
        wl = None; ll = None
        for row in rd:
            if float(row['host_write_MB']) < WARMUP_MB: wl = row
            ll = row
    h = float(ll['host_write_MB']) - float(wl['host_write_MB'])
    c = float(ll['cache_write_MB']) - float(wl['cache_write_MB'])
    e = float(ll['backend_write_MB']) - float(wl['backend_write_MB'])
    return e/h, c/h  # flush_rate, comp_rate

RS = [2, 4, 6, 8, 10]
WARMUP_BYTES = 6 * 1024**4
BLOCK_BYTES = 4096
KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")

# Order: short label, file pattern (with {r}), absolute_dir or None
POLICIES = [
    ("GD",      "LOG_GREEDY_COST_BENEFIT_10_us02_ewma_fix.stat_pr{r}",        None),
    ("TDELTA",  "LOG_GREEDY_COST_BENEFIT_10_TDELTA_us02_ewma_fix.stat_pr{r}", None),
    ("GC",      "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl3145728.stat_pr{r}", None),
    ("GS (θ·i)","LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_fix.stat_pr{r}",     None),
    ("GD002",   "LOG_GREEDY_COST_BENEFIT_10_GD002.stat_pr{r}",
                "/home/sejun000/ssd_waf_main_tdelta/prev_init05_ghost01"),
    ("CSAL",    None, None),
]

def parse(path):
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
    e = last[1] - warm_first[1]
    c = last[2] - warm_first[2]
    if h <= 0:
        return None
    return e/h, c/h

def gather():
    out = {}
    for label, pat, base in POLICIES:
        if label == "CSAL":
            continue
        for r in RS:
            name = pat.format(r=r)
            path = os.path.join(base, name) if base else name
            if not os.path.exists(path):
                print(f"missing: {path}", file=sys.stderr)
                continue
            v = parse(path)
            if v is None:
                continue
            out[(label, r)] = v
    csal = parse_csal()
    if csal is not None:
        for r in RS:
            out[("CSAL", r)] = csal
    return out

def main():
    data = gather()
    labels = [p[0] for p in POLICIES]
    n_pol = len(labels)
    xs = np.arange(len(RS))
    bw = 0.16

    fig, ax = plt.subplots(figsize=(13, 5.8))
    color_flush = "#4C72B0"
    color_comp  = "#DD8452"

    for i, label in enumerate(labels):
        flush_part = []
        comp_part  = []
        for r in RS:
            v = data.get((label, r))
            if v is None:
                flush_part.append(0); comp_part.append(0)
            else:
                fl, cp = v
                flush_part.append(r * fl)
                comp_part.append(cp)
        flush_part = np.array(flush_part)
        comp_part  = np.array(comp_part)
        offset = (i - (n_pol - 1) / 2.0) * bw
        ax.bar(xs + offset, flush_part, bw,
               color=color_flush, edgecolor="black", linewidth=0.4,
               label="r·flush" if i == 0 else None)
        ax.bar(xs + offset, comp_part, bw, bottom=flush_part,
               color=color_comp, edgecolor="black", linewidth=0.4,
               label="comp" if i == 0 else None)
        for x, fl, cp in zip(xs + offset, flush_part, comp_part):
            total = fl + cp
            if total > 0:
                ax.text(x, total + 0.04, f"{total:.2f}",
                        ha="center", va="bottom", fontsize=6.5, rotation=90)
        for x in xs:
            ax.text(x + offset, -0.02, label, ha="center", va="top",
                    fontsize=6.5, rotation=45,
                    transform=ax.get_xaxis_transform())

    ax.set_xticks(xs)
    ax.set_xticklabels([f"r={r}" for r in RS], y=-0.10)
    ax.set_xlabel("periodic_ratio  r", labelpad=22)
    ax.set_ylabel("f = r·(evict/host) + (comp/host)")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=9, loc="upper left", framealpha=0.9)
    ax.set_title("GD / TDELTA / GC / GS(θ·i) / GD002 / CSAL stacked f-decomposition\n"
                 "us=0.02, EWMA; GC+GS hl=3.15M, GD+TDELTA hl=default(6.29M); "
                 "GD002=prev_init05_ghost01 snapshot; "
                 "CSAL=ftl0_20260225_082514.csv (Alibaba DWPD2 trace, sim trace=dwpd1to2_4x)",
                 fontsize=9)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_compare_stacked.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
