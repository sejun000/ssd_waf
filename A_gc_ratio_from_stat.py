#!/usr/bin/env python3
"""
GC Ratio plot from stat file deltas (per 6 GiB tick, 96 GiB bin).

Each stat row prints cumulative compact / evict at 6 GiB host write interval.
  decision_t = "compact" if (Δcomp_t > Δflush_t) else "flush"
  per 96 GiB bin: ratio = #compact-ticks / 16

Plots for:
  (a) REFLASH (seq2tib)       — GS_FINAL_seq2tib.stat_pr864
  (b) LOG_GREEDY_80 (inv9_seq)— LOG_GREEDY80_inv9seq.stat

x: Host writes [TiB]
y: GC Ratio
shaded: inv (9–10 TiB, only for plot b) + sequential inject (11–13 TiB)
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

PAGE = 4096
TIB = 1024**4
BIN_BYTES = 96 * 1024**3
TICK_BYTES = 6 * 1024**3
TICKS_PER_BIN = BIN_BYTES // TICK_BYTES   # 16

X_START_TIB = 8.0
SEQ_LO_TIB, SEQ_HI_TIB = 11.0, 13.0
INV_LO_TIB, INV_HI_TIB = 9.0, 10.0


def parse_stat(path):
    """Yield (host_bytes, comp_blocks, evict_blocks) per LOG row."""
    rows = []
    with open(path) as f:
        for ln in f:
            if not ln.startswith('LOG'):
                continue
            toks = ln.split()
            d = {}
            i = 1
            while i < len(toks):
                k = toks[i]
                if k.endswith(':'):
                    k = k[:-1]
                    if i + 1 < len(toks):
                        try:
                            d[k] = float(toks[i+1])
                        except ValueError:
                            pass
                        i += 2
                    else:
                        i += 1
                else:
                    i += 1
            rows.append((d.get('write_size_to_cache', 0),
                         d.get('compacted_blocks', 0),
                         d.get('evicted_blocks', 0)))
    return rows


def bin_gc_ratio(rows):
    """Return list of (bin_center_TiB, gc_ratio)."""
    if len(rows) < 2:
        return []
    # collect per-tick deltas (skip first row which is just initial state)
    deltas = []  # (host_TiB_center, dcomp, dflush)
    for i in range(1, len(rows)):
        h_prev, c_prev, f_prev = rows[i-1]
        h_cur,  c_cur,  f_cur  = rows[i]
        dcomp  = c_cur - c_prev
        dflush = f_cur - f_prev
        center_TiB = (h_prev + h_cur) / 2 / TIB
        deltas.append((center_TiB, dcomp, dflush))
    out = []
    # group every 16 ticks
    for j in range(0, len(deltas), TICKS_PER_BIN):
        chunk = deltas[j:j+TICKS_PER_BIN]
        if not chunk:
            continue
        n_comp_wins = sum(1 for (_, dc, df) in chunk if dc > df)
        n_total    = sum(1 for (_, dc, df) in chunk if (dc + df) > 0)
        if n_total == 0:
            continue
        x = np.mean([c for (c, _, _) in chunk])
        out.append((x, n_comp_wins / n_total))
    return out


def plot_one(out_path, points, title_hint, inv=False):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    fig, ax = plt.subplots(figsize=(10, 4.2))
    if inv:
        ax.axvspan(INV_LO_TIB, INV_HI_TIB, color='#1976D2', alpha=0.30)
        ax.text((INV_LO_TIB + INV_HI_TIB)/2, 0.55, 'Invalidate',
                ha='center', va='center', fontsize=16, fontweight='bold',
                color='#0D3F73')
    ax.axvspan(SEQ_LO_TIB, SEQ_HI_TIB, color='#D81B60', alpha=0.35)
    ax.text((SEQ_LO_TIB + SEQ_HI_TIB)/2, 0.55, 'Sequential Inject',
            ha='center', va='center', fontsize=18, fontweight='bold',
            color='#7A0033')
    ax.plot(xs, ys, '-', color='steelblue', linewidth=1.4)
    ax.fill_between(xs, ys, color='steelblue', alpha=0.30)
    ax.set_xlabel('Host writes [TiB]', fontsize=16)
    ax.set_ylabel('GC Ratio', fontsize=16)
    ax.tick_params(axis='both', labelsize=13)
    ax.set_ylim(-0.02, 1.05)
    if xs:
        ax.set_xlim(X_START_TIB, max(xs) + 0.3)
    ax.grid(alpha=0.3)
    ax.set_axisbelow(True)
    plt.tight_layout()
    plt.savefig(out_path + '.pdf')
    plt.savefig(out_path + '.png', dpi=150)
    plt.close(fig)
    print(f'saved: {out_path}.{{pdf,png}}  bins={len(points)}  mean GC={np.mean(ys):.3f}')


def main():
    os.chdir('/home/sejun000/ssd_waf')
    cfgs = [
        ('GS_FINAL_seq2tib.stat_pr864',   'AA_gcratio_REFLASH_seq2tib',   False),
        ('LOG_GREEDY_COST_BENEFIT_80.stat.log.20260603_111843', 'AA_gcratio_GREEDY80_inv9seq', True),
        ('GS_FINAL_inv9seq.stat_pr864',   'AA_gcratio_REFLASH_inv9seq',   True),
    ]
    for stat, out, has_inv in cfgs:
        if not os.path.exists(stat):
            print(f'# MISSING {stat}')
            continue
        rows = parse_stat(stat)
        pts  = bin_gc_ratio(rows)
        plot_one(out, pts, stat, inv=has_inv)
        # rough numeric summary
        arr_x = np.array([p[0] for p in pts])
        arr_y = np.array([p[1] for p in pts])
        for lo, hi, name in [(SEQ_LO_TIB, SEQ_HI_TIB, 'seq'),
                             (INV_LO_TIB, INV_HI_TIB, 'inv'),
                             (6.0, SEQ_LO_TIB, 'pre-seq')]:
            mask = (arr_x >= lo) & (arr_x < hi)
            if mask.any():
                print(f'  {stat} [{lo:.1f}~{hi:.1f}] {name}: mean GC = {arr_y[mask].mean():.3f}  ({mask.sum()} bins)')


if __name__ == '__main__':
    main()
