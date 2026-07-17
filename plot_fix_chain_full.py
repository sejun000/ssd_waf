#!/usr/bin/env python3
"""Plot f = r*flush/host + comp/host for the post-fix 3-policy x 2-MA sweep."""
import os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

POLICIES = [
    ("LOG_GREEDY_COST_BENEFIT_10",        "GD"),
    ("LOG_GREEDY_COST_BENEFIT_10_TDELTA", "TDELTA"),
    ("LOG_GREEDY_COST_BENEFIT_10_GC",     "GC"),
]
MAS = ["ewma", "sma"]
RS = [2, 4, 6, 8, 10]
WARMUP_BYTES = 6 * 1024**4  # 6 TiB
BLOCK_BYTES = 4096

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")

def parse_last_stat_after_warmup(path):
    """Return (host_blocks, evicted_blocks, compacted_blocks) at end of run, after warmup."""
    last = None
    warm_first = None
    with open(path) as f:
        for line in f:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                wsc = int(kv["write_size_to_cache"])
                ev = int(kv["evicted_blocks"])
                cp = int(kv["compacted_blocks"])
            except KeyError:
                continue
            if wsc >= WARMUP_BYTES and warm_first is None:
                warm_first = (wsc, ev, cp)
            last = (wsc, ev, cp)
    if warm_first is None or last is None:
        return None
    h_first, e_first, c_first = warm_first
    h_last, e_last, c_last = last
    host_blocks = (h_last - h_first) / BLOCK_BYTES
    evict_blocks = e_last - e_first
    comp_blocks = c_last - c_first
    return host_blocks, evict_blocks, comp_blocks

def gather():
    out = {}
    for pol, _ in POLICIES:
        for ma in MAS:
            for r in RS:
                tag = f"{pol}_us02_{ma}_fix"
                path = f"{tag}.stat_pr{r}"
                if not os.path.exists(path):
                    print(f"missing: {path}", file=sys.stderr)
                    continue
                res = parse_last_stat_after_warmup(path)
                if res is None:
                    print(f"empty post-warmup: {path}", file=sys.stderr)
                    continue
                h, e, c = res
                if h <= 0:
                    continue
                flush = e / h
                comp = c / h
                f_obj = r * flush + comp
                out[(pol, ma, r)] = (f_obj, flush, comp)
    return out

def main():
    data = gather()
    fig, axes = plt.subplots(1, 3, figsize=(16, 4.6))
    titles = ["f = r·flush + comp", "flush / host", "comp / host"]
    ylabels = ["f (lower = better)", "evicted_blocks / host_blocks", "compacted_blocks / host_blocks"]

    colors = {"GD": "tab:blue", "TDELTA": "tab:orange", "GC": "tab:green"}
    styles = {"ewma": "-o", "sma": "--s"}

    for pol, label in POLICIES:
        for ma in MAS:
            xs, fs, fls, cps = [], [], [], []
            for r in RS:
                key = (pol, ma, r)
                if key not in data:
                    continue
                f_obj, flush, comp = data[key]
                xs.append(r); fs.append(f_obj); fls.append(flush); cps.append(comp)
            if not xs:
                continue
            line_label = f"{label} {ma.upper()}"
            for ax, ys in zip(axes, (fs, fls, cps)):
                ax.plot(xs, ys, styles[ma], color=colors[label], label=line_label,
                        markersize=6, linewidth=1.6)

    for ax, title, ylab in zip(axes, titles, ylabels):
        ax.set_xlabel("periodic_ratio r")
        ax.set_ylabel(ylab)
        ax.set_title(title)
        ax.set_xticks(RS)
        ax.grid(True, alpha=0.3)
    axes[0].legend(fontsize=8, loc="best", ncol=2)

    fig.suptitle("Post-fix sweep: 3 policies × {EWMA, SMA} × r∈{2,4,6,8,10}, us=0.02", y=1.02)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"fix_chain_full.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

    print("\nf-objective table:")
    print(f"{'r':<4}", end="")
    for pol, lab in POLICIES:
        for ma in MAS:
            print(f"  {lab}_{ma:<5}", end="")
    print()
    for r in RS:
        print(f"{r:<4}", end="")
        for pol, lab in POLICIES:
            for ma in MAS:
                v = data.get((pol, ma, r))
                if v is None:
                    print(f"  {'-':<10}", end="")
                else:
                    print(f"  {v[0]:<10.3f}", end="")
        print()

if __name__ == "__main__":
    main()
