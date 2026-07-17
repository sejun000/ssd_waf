#!/usr/bin/env python3
"""
Regenerate two Alibaba figures up to 50 TiB from the current sqrt-r864 REFLASH runs:

  Fig1  AA_tec_save_Ali_pct.pdf   cumulative TEC reduction (%) vs host writes
                                  Ali1 = hi (blue), Ali2 = mid (red), REFLASH vs LOG_FIFO
  Fig2  AA_rule_diag_Ali.pdf      rule diagnostic, 2 subplots (a) Alibaba1=hi (b) Alibaba2=mid
                                  left  : Ratio  -> LHS(Gud, red), RHS(retain, blue), waf(Wgc, green dash)
                                  right : RAISE (%) gray dotted

Data prepared in _plotdat/ by awk (see session).  comp/cold_nand cumulative -> step interp.
"""
import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

os.chdir('/home/sejun000/ssd_waf')
DAT = '_plotdat'
R = 8.64
PAGE = 4096
XMAX = 60.0

C_HI = '#1F4E79'   # Ali1 / Alibaba1  (blue)
C_MID = '#C0392B'  # Ali2 / Alibaba2  (red)


def load2(path):
    a = np.loadtxt(path)
    return a[:, 0], a[:, 1]


def step(hx, hy, grid):
    """cumulative step interp: value at last sample <= grid point."""
    idx = np.searchsorted(hx, grid, side='right') - 1
    idx = np.clip(idx, 0, len(hy) - 1)
    return hy[idx]


# ----------------------------------------------------------------- Fig 1
def fig1():
    fig, ax = plt.subplots(figsize=(9.2, 4.8))
    for wl, color, label in [('hi', C_HI, 'Ali1'), ('mid', C_MID, 'Ali2')]:
        ch, cc = load2(f'{DAT}/{wl}_compcum.dat')                       # host_tib, comp_cum(pages)
        rh, rn = load2(f'{DAT}/dwpd{wl}_384_swf_r864.coldnand.dat')     # host_tib, cold_nand(bytes)
        fh, fn = load2(f'{DAT}/dwpd{wl}_384_logfifo.coldnand.dat')
        hmax = min(XMAX, ch.max(), rh.max(), fh.max())
        grid = np.arange(1.5, hmax, 0.25)   # skip pre-onset warmup spike
        comp_r = step(ch, cc, grid)
        cold_r = step(rh, rn, grid)
        cold_f = step(fh, fn, grid)
        tec_r = comp_r + R * cold_r / PAGE
        tec_f = R * cold_f / PAGE
        red = (tec_f - tec_r) / tec_f * 100.0
        ax.plot(grid, red, '-', color=color, linewidth=1.8, label=label)
        print(f'{label}({wl}): end@{grid[-1]:.1f}TiB  TECred={red[-1]:.1f}%  '
              f'(min {red.min():.1f} / max {red.max():.1f})')
    ax.axhline(0, color='k', linestyle=':', linewidth=0.9)
    ax.set_xlabel('host writes (TiB)', fontsize=21)
    ax.set_ylabel('TEC reduction (%)', fontsize=21)
    ax.set_xlim(0, XMAX)
    ax.set_ylim(0, 50)
    ax.grid(alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc='upper center', ncol=2, fontsize=20, frameon=False,
              bbox_to_anchor=(0.5, 1.13))
    ax.tick_params(labelsize=18)
    plt.tight_layout()
    plt.savefig('AA_tec_save_Ali_pct.pdf')
    plt.savefig('AA_tec_save_Ali_pct.png', dpi=150)
    print('saved AA_tec_save_Ali_pct.{pdf,png}')
    plt.close(fig)


# ----------------------------------------------------------------- Fig 2
def _smooth(y, w=5):
    """centered moving average over a 5-TiB window (bins are 1 TiB wide)."""
    if len(y) < w:
        return y
    k = np.ones(w) / w
    pad = w // 2
    yp = np.pad(y, pad, mode='edge')
    return np.convolve(yp, k, mode='valid')[:len(y)]


def fig2():
    fig, (ax_a, ax_b, ax_c) = plt.subplots(3, 1, figsize=(10, 8.5), sharex=True,
                                            gridspec_kw={'height_ratios': [3, 4, 3]})

    # ---- (a) Capacity-tier device WA  (REFLASH QLC WA, 1-TiB-bin differential) ----
    cwh, cwv = load2(f'{DAT}/hi_coldwaf_diff2t.dat')   # d(cold_nand)/d(cold_host) per 2 TiB
    ma = cwh <= XMAX
    ax_a.plot(cwh[ma], cwv[ma], '-o', color=C_HI, lw=2.0, ms=4)
    ax_a.set_ylabel('WA', fontsize=22)
    ax_a.set_ylim(0.95, 2.5)
    ax_a.set_xlim(0, XMAX)
    ax_a.grid(alpha=0.3)
    ax_a.set_axisbelow(True)
    ax_a.set_title('(a) Capacity-tier device WA', fontsize=20, y=-0.30)
    ax_a.tick_params(labelsize=19, labelbottom=False)

    # ---- (b) Decision rule: GC_cost + GC_benefit (left) + RAISE% (right, red)  (2-TiB bins) ----
    d = np.loadtxt(f'{DAT}/hi_rulebin_2t.dat')   # ctr LHS RHS waf raise% cnt gccost (2 TiB)
    x, lhs, rhs, raise_pct = d[:, 0], d[:, 1], d[:, 2], d[:, 4]
    m = x <= XMAX
    x = x[m]; lhs = lhs[m]; rhs = rhs[m]; raise_pct = raise_pct[m]
    l2, = ax_b.plot(x, rhs, '-o', color='#1976D2', lw=1.6, ms=4, label=r'$Benefit_{gc}$')
    l3, = ax_b.plot(x, lhs, '--s', color='#2E8B57', lw=1.4, ms=4, label=r'$Cost_{gc}$')
    ax_b.set_ylabel('Ratio', fontsize=22)
    ax_b.set_ylim(0, 8)
    ax_b.set_xlim(0, XMAX)
    ax_b.grid(alpha=0.3)
    ax_b.set_axisbelow(True)
    ax_b.set_title('(b) Reclamation decision', fontsize=20, y=-0.20)
    ax_b.tick_params(labelsize=19, labelbottom=False)
    axr = ax_b.twinx()
    l4, = axr.plot(x, raise_pct, ':^', color='#D62728', lw=1.4, ms=4, label='GC reclamation (%)')
    axr.set_ylabel('GC reclamation (%)', fontsize=22, color='#D62728')
    axr.set_ylim(0, 100)
    axr.tick_params(axis='y', labelcolor='#D62728', labelsize=18)
    ax_b.legend([l2, l3, l4], [h.get_label() for h in (l2, l3, l4)],
                loc='upper left', ncol=3, fontsize=15, frameon=True,
                framealpha=0.92, edgecolor='0.4', borderpad=0.35,
                columnspacing=1.2, handlelength=2.0, handletextpad=0.5)

    # ---- (c) TEC saving (REFLASH vs LOG_FIFO) - cumulative ----
    ch, cc = load2(f'{DAT}/hi_compcum.dat')
    rh, rn = load2(f'{DAT}/dwpdhi_384_swf_r864.coldnand.dat')
    fh, fn = load2(f'{DAT}/dwpdhi_384_logfifo.coldnand.dat')
    hmax = min(XMAX, ch.max(), rh.max(), fh.max())
    grid = np.arange(1.5, hmax, 0.25)
    tec_r = step(ch, cc, grid) + R * step(rh, rn, grid) / PAGE
    tec_f = R * step(fh, fn, grid) / PAGE
    red = (tec_f - tec_r) / tec_f * 100.0
    ax_c.plot(grid, red, '-', color=C_HI, lw=2.0)
    ax_c.set_ylabel('TEC reduction (%)', fontsize=22)
    ax_c.set_ylim(0, 50)
    ax_c.set_xlim(0, XMAX)
    ax_c.grid(alpha=0.3)
    ax_c.set_axisbelow(True)
    ax_c.set_xlabel('host writes (TiB)', fontsize=22)
    ax_c.set_title('(c) TEC saving', fontsize=20, y=-0.80)
    ax_c.tick_params(labelsize=19)

    fig.subplots_adjust(left=0.14, right=0.88, top=0.96, bottom=0.12, hspace=0.32)
    plt.savefig('AA_rule_diag_Ali.pdf')
    plt.savefig('AA_rule_diag_Ali.png', dpi=150)
    print('saved AA_rule_diag_Ali.{pdf,png}')
    plt.close(fig)


if __name__ == '__main__':
    fig1()
    fig2()
