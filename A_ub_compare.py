#!/usr/bin/env python3
"""
Overlay U_B (target_valid_blk_rate) for REFLASH vs LOG_GREEDY_80
on the inv9_seq11to13 trace.

x : host writes [TiB]
y : target_valid_blk_rate
shaded : inv (9–10 TiB), seq (11–13 TiB)
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

PAGE = 4096
TIB = 1024**4
X_START_TIB = 8.0
INV_LO, INV_HI = 9.0, 10.0
SEQ_LO, SEQ_HI = 11.0, 13.0

CASES = [
    ('REFLASH r864',     'GS_FINAL_inv9seq.stat_pr864',
        '#1F4E79'),
    ('LOG_GREEDY_80',    'LOG_GREEDY_COST_BENEFIT_80.stat.log.20260603_111843',
        '#C55A11'),
]


def parse(path):
    xs, ys = [], []
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
            h = d.get('write_size_to_cache', 0) / TIB
            tgt = d.get('target_valid_rate', 0)
            xs.append(h)
            ys.append(tgt)
    return np.array(xs), np.array(ys)


def main():
    os.chdir('/home/sejun000/ssd_waf')
    fig, ax = plt.subplots(figsize=(11, 4.6))
    ax.axvspan(INV_LO, INV_HI, color='#1976D2', alpha=0.25)
    ax.text((INV_LO+INV_HI)/2, 0.05, 'Invalidate',
            ha='center', va='bottom', fontsize=15, fontweight='bold',
            color='#0D3F73')
    ax.axvspan(SEQ_LO, SEQ_HI, color='#D81B60', alpha=0.30)
    ax.text((SEQ_LO+SEQ_HI)/2, 0.05, 'Sequential Inject',
            ha='center', va='bottom', fontsize=17, fontweight='bold',
            color='#7A0033')
    for label, path, color in CASES:
        if not os.path.exists(path):
            print(f'# MISSING {path}')
            continue
        xs, ys = parse(path)
        m = xs >= X_START_TIB
        ax.plot(xs[m], ys[m], '-', color=color, linewidth=1.6, label=label)
        print(f'{label}: rows={len(xs)}  mean U_B={ys[m].mean():.3f}')
        for lo, hi, name in [(INV_LO, INV_HI, 'inv'),
                             (SEQ_LO, SEQ_HI, 'seq'),
                             (X_START_TIB, INV_LO, 'pre')]:
            mm = (xs >= lo) & (xs < hi)
            if mm.any():
                print(f'  [{lo:.1f}~{hi:.1f}] {name}: mean U_B = {ys[mm].mean():.3f}  ({mm.sum()} samples)')
    ax.set_xlabel('Host writes [TiB]', fontsize=16)
    ax.set_ylabel('$U_B$  (target valid rate)', fontsize=16)
    ax.tick_params(axis='both', labelsize=13)
    ax.set_ylim(0, 1.0)
    ax.set_xlim(X_START_TIB, 14.05)
    ax.grid(alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc='upper left', fontsize=13)
    plt.tight_layout()
    plt.savefig('AA_UB_compare_inv9seq.pdf')
    plt.savefig('AA_UB_compare_inv9seq.png', dpi=150)
    print('saved: AA_UB_compare_inv9seq.{pdf,png}')


if __name__ == '__main__':
    main()
