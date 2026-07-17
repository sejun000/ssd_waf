#!/usr/bin/env python3
"""
RAISE 비율 plot for seq2tib trace REFLASH r864 run.

x axis: host write (TiB)
y axis: RAISE / (RAISE + LOWER) ratio per 96 GiB bin
shaded region: sequential injection window  (sim 11~13 TiB)
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

PAGE = 4096
TIB  = 1024**4
BIN_BYTES = 96 * 1024**3        # 96 GiB
BIN_PAGES = BIN_BYTES // PAGE
TIB_PER_BIN = BIN_BYTES / TIB    # = 0.09375

GSDEC = '/home/sejun000/ssd_waf/GS_FINAL_seq2tib_pr864.gsdec.log'

SEQ_LO_TIB = 11.0
SEQ_HI_TIB = 13.0


def main():
    bins_raise = {}
    bins_lower = {}
    with open(GSDEC) as f:
        next(f)
        for ln in f:
            t = ln.split()
            if len(t) < 11:
                continue
            try:
                ts  = int(t[0])
                dec = t[10]
            except ValueError:
                continue
            b = ts // BIN_PAGES
            if dec == 'RAISE':
                bins_raise[b] = bins_raise.get(b, 0) + 1
            elif dec == 'LOWER':
                bins_lower[b] = bins_lower.get(b, 0) + 1
    all_b = sorted(set(bins_raise) | set(bins_lower))
    xs = []
    ys = []
    for b in all_b:
        r = bins_raise.get(b, 0)
        l = bins_lower.get(b, 0)
        if r + l == 0:
            continue
        # bin center in TiB
        center_tib = (b + 0.5) * BIN_BYTES / TIB
        xs.append(center_tib)
        ys.append(r / (r + l))

    fig, ax = plt.subplots(figsize=(10, 4.2))
    ax.axvspan(SEQ_LO_TIB, SEQ_HI_TIB, color='#D81B60', alpha=0.35)
    # large label inside shaded region
    mid_x = (SEQ_LO_TIB + SEQ_HI_TIB) / 2
    ax.text(mid_x, 0.55, 'Sequential Inject',
            ha='center', va='center', fontsize=18, fontweight='bold',
            color='#7A0033')
    ax.plot(xs, ys, '-', color='steelblue', linewidth=1.4)
    ax.fill_between(xs, ys, color='steelblue', alpha=0.30)
    ax.set_xlabel('Host writes [TiB]', fontsize=16)
    ax.set_ylabel('GC Ratio', fontsize=16)
    ax.tick_params(axis='both', labelsize=13)
    ax.set_ylim(-0.02, 1.05)
    ax.set_xlim(8.0, max(xs) + 0.3)
    ax.grid(alpha=0.3)
    ax.set_axisbelow(True)
    plt.tight_layout()
    out_pdf = '/home/sejun000/ssd_waf/AA_raise_ratio_seq2tib.pdf'
    out_png = '/home/sejun000/ssd_waf/AA_raise_ratio_seq2tib.png'
    plt.savefig(out_pdf)
    plt.savefig(out_png, dpi=150)
    print(f'saved: {out_pdf}')
    print(f'saved: {out_png}')

    # rough numeric overview
    arr = np.array(ys)
    print(f'bins={len(ys)}  mean RAISE ratio = {arr.mean():.3f}')
    seq_mask = (np.array(xs) >= SEQ_LO_TIB) & (np.array(xs) <= SEQ_HI_TIB)
    pre_mask = (np.array(xs) >= 6.0) & (np.array(xs) < SEQ_LO_TIB)
    if seq_mask.any():
        print(f'  seq window  ({SEQ_LO_TIB}~{SEQ_HI_TIB} TiB): mean = {arr[seq_mask].mean():.3f} ({seq_mask.sum()} bins)')
    if pre_mask.any():
        print(f'  pre  window ( 6~{SEQ_LO_TIB} TiB):   mean = {arr[pre_mask].mean():.3f} ({pre_mask.sum()} bins)')


if __name__ == '__main__':
    main()
