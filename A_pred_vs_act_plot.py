#!/usr/bin/env python3
"""
Bar plot — predicted vs actual W_gc / Inv / TEC saving per workload.

All in TiB, measured from 6 TiB host write onward.

Definitions
  W_gc       — GC (compaction) cost
    predicted:  Σ LHS_t · Δh   (LHS = Gud EWMA)
    actual:     Σ ΔC_{t+1}      (next-tick comp_cum increment)

  Inv        — avoided cold-flush credit  (TiB, weighted by QLC=r=8.64)
    predicted:  Σ RHS_t · Δh   (RHS = invrate_sum · r · waf)
    actual:     QLC · (LF_evict − REFL_evict)

  TEC saving — net saving over LOGFIFO_AFTER_6TiB baseline
    predicted:  Σ (RHS − LHS) · Δh = pred Inv − pred W_gc
    actual:     TEC(LOGFIFO_AFTER_6TiB) − TEC(REFLASH) = act Inv − act W_gc
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# reuse extractors from sibling script
import importlib.util
spec = importlib.util.spec_from_file_location("base",
        "/home/sejun000/ssd_waf/A_pred_vs_act_table.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

PAGE, TIB, QLC = m.PAGE, m.TIB, m.QLC
WARMUP_PG = 6 * TIB // PAGE

WORKLOADS = ['Ali1', 'Ali2', 'Ali3', 'YCSB-A']

def collect(w):
    refl = m.REFLASH_STAT[w]
    fifo = m.LOGFIFO_STAT[w]
    gsdec = m.REFLASH_GSDEC[w]
    tec_r, dh_r, dc_r, de_r = m.tec_from_stat_after(refl, WARMUP_PG)
    tec_f, dh_f, dc_f, de_f = m.tec_from_stat_after(fifo, WARMUP_PG)
    a = m.analyze(gsdec, WARMUP_PG)
    pred_W   = a['sum_lhs_t_pg']    * PAGE / TIB
    pred_Inv = a['sum_rhs_t_pg']    * PAGE / TIB
    act_W    = dc_r                                 # REFL comp [TiB]
    act_Inv  = QLC * (de_f - de_r)                  # avoided cold flush, weighted
    pred_TEC = pred_Inv - pred_W
    act_TEC  = tec_f - tec_r
    return pred_W, act_W, pred_Inv, act_Inv, pred_TEC, act_TEC


def main():
    os.chdir('/home/sejun000/ssd_waf')

    vals = np.array([collect(w) for w in WORKLOADS])    # (4, 6)
    pred_W, act_W, pred_Inv, act_Inv, pred_TEC, act_TEC = vals.T

    x = np.arange(len(WORKLOADS))
    w = 0.13

    fig, ax = plt.subplots(figsize=(11, 5.5))
    colors = {
        'pred_W':   '#5B9BD5',
        'act_W':    '#1F4E79',
        'pred_Inv': '#F4B183',
        'act_Inv':  '#C55A11',
        'pred_TEC': '#A9D08E',
        'act_TEC':  '#385723',
    }
    ax.bar(x - 2.5*w, pred_W,   w, label='pred W_gc',     color=colors['pred_W'])
    ax.bar(x - 1.5*w, act_W,    w, label='actual W_gc',   color=colors['act_W'])
    ax.bar(x - 0.5*w, pred_Inv, w, label='pred Inv',      color=colors['pred_Inv'])
    ax.bar(x + 0.5*w, act_Inv,  w, label='actual Inv',    color=colors['act_Inv'])
    ax.bar(x + 1.5*w, pred_TEC, w, label='pred TEC save', color=colors['pred_TEC'])
    ax.bar(x + 2.5*w, act_TEC,  w, label='actual TEC save', color=colors['act_TEC'])

    # value labels above each bar
    for i, vv in enumerate(vals):
        for j, v in enumerate(vv):
            xpos = x[i] + (j - 2.5) * w
            ax.text(xpos, v + 0.4, f'{v:.1f}',
                    ha='center', va='bottom', fontsize=7)

    ax.set_xticks(x)
    ax.set_xticklabels(WORKLOADS, fontsize=11)
    ax.set_ylabel('TiB  (after 6 TiB host write)', fontsize=11)
    ax.set_title('REFLASH rule accuracy — predicted vs actual (TiB)', fontsize=12)
    ax.legend(ncol=3, loc='upper left', fontsize=9)
    ax.grid(axis='y', alpha=0.3)
    ax.set_axisbelow(True)
    ymax = float(np.max(vals)) * 1.18
    ax.set_ylim(0, ymax)

    plt.tight_layout()
    out_pdf = '/home/sejun000/ssd_waf/AA_pred_vs_act.pdf'
    out_png = '/home/sejun000/ssd_waf/AA_pred_vs_act.png'
    plt.savefig(out_pdf)
    plt.savefig(out_png, dpi=150)
    print(f'saved: {out_pdf}')
    print(f'saved: {out_png}')

    # numeric dump
    print()
    print(f"{'Workload':8} {'pW':>6} {'aW':>6} {'pInv':>6} {'aInv':>6} {'pTEC':>6} {'aTEC':>6}")
    for i, name in enumerate(WORKLOADS):
        print(f'{name:8} ' + ' '.join(f'{v:>6.2f}' for v in vals[i]))


if __name__ == '__main__':
    main()
