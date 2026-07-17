#!/usr/bin/env python3
"""
predSAVE vs actSAVE table (4 workloads, REFLASH 6/1 = GS_FINAL_clean).

For each RAISE tick with raw_compact > 0 in the *next* tick:
  (1) Σ RHS_t · Δhost     vs  Σ (comp_cum_{t+1} − comp_cum_t)
  (2) Σ LHS_t · Δhost     vs  Σ LHS_{t+1} · Δhost
  (3) Σ(RHS−LHS)·Δhost    vs  ΔTEC = TEC(LOGFIFO_AFTER_6TiB) − TEC(REFLASH)

All sums in TiB.
"""
import os

PAGE = 4096
TIB = 1024 ** 4
QLC = 8.64

REFLASH_STAT = {
    'Ali1':   'GS_FINAL_clean_dwpd2_5x.stat_pr864',
    'Ali2':   'GS_FINAL_clean_dwpd1to2.stat_pr864',
    'Ali3':   'GS_FINAL_clean_dwpd01to1.stat_pr864',
    'YCSB-A': 'GS_FINAL_clean_scaled4x.stat_pr864',
}
REFLASH_GSDEC = {
    'Ali1':   'GS_FINAL_clean_dwpd2_5x_pr864.gsdec.log',
    'Ali2':   'GS_FINAL_clean_dwpd1to2_pr864.gsdec.log',
    'Ali3':   'GS_FINAL_clean_dwpd01to1_pr864.gsdec.log',
    'YCSB-A': 'GS_FINAL_clean_scaled4x_pr864.gsdec.log',
}
LOGFIFO_STAT = {
    'Ali1':   'LOGFIFO_AFTER_6TiB_dwpd2_5x_pr864.stat',
    'Ali2':   'LOGFIFO_AFTER_6TiB_dwpd1to2_pr864.stat',
    'Ali3':   'LOGFIFO_AFTER_6TiB_dwpd01to1_pr864.stat',
    'YCSB-A': 'LOGFIFO_AFTER_6TiB_scaled4x_pr864.stat',
}


def last_stat(path):
    """Return last LOG_* line as dict {key: float}."""
    last = None
    with open(path) as f:
        for ln in f:
            if ln.startswith('LOG'):
                last = ln
    if last is None:
        return None
    toks = last.split()
    # Parse "key: val" pairs
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
    return d


def tec_from_stat(d):
    host = d['write_size_to_cache']                         # bytes
    comp = d['compacted_blocks'] * PAGE                     # bytes
    evict = d['evicted_blocks'] * PAGE                      # bytes
    return (host + comp + QLC * evict) / TIB                # TiB


def tec_from_stat_after(path, warmup_pg):
    """TEC computed only from increments after host write >= warmup_pg."""
    base = None
    last = None
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
            host_pg = d.get('write_size_to_cache', 0) / PAGE
            if base is None and host_pg >= warmup_pg:
                base = (d.get('write_size_to_cache', 0),
                        d.get('compacted_blocks', 0) * PAGE,
                        d.get('evicted_blocks', 0) * PAGE,
                        d.get('global_valid_blocks', 0))
            last = (d.get('write_size_to_cache', 0),
                    d.get('compacted_blocks', 0) * PAGE,
                    d.get('evicted_blocks', 0) * PAGE,
                    d.get('global_valid_blocks', 0))
    if base is None or last is None:
        return None, None, None, None
    dh = (last[0] - base[0]) / TIB
    dc = (last[1] - base[1]) / TIB
    de = (last[2] - base[2]) / TIB
    tec = dh + dc + QLC * de
    return tec, dh, dc, de


def analyze(gsdec_path, warmup_pg):
    """Return analysis dict.

    qualifying condition: ts_t >= warmup_pg
                          AND decision_t == RAISE
                          AND (comp_cum_{t+1} - comp_cum_t) > 0   ← NEXT tick ΔC > 0
    LHS  (=Gud)        : taken at CURRENT tick t
    ΔC (compact)       : taken at NEXT tick (t→t+1)
    """
    rows = []
    with open(gsdec_path) as f:
        next(f)  # header
        for ln in f:
            t = ln.split()
            if len(t) < 49:
                continue
            try:
                ts        = int(t[0])
                lhs       = float(t[8])
                rhs       = float(t[9])
                decision  = t[10]
                comp_cum  = float(t[16])
                raw_gud   = float(t[47])    # col 48
            except (ValueError, IndexError):
                continue
            rows.append((ts, lhs, rhs, decision, comp_cum, raw_gud))
    n_total_raise = 0
    n_qualify = 0
    sum_lhs_t       = 0.0
    sum_act_next    = 0.0
    sum_rhs_t       = 0.0
    sum_rhs_next    = 0.0
    sum_diff        = 0.0
    sum_rawgud_t    = 0.0   # Σ raw_gud_t · Δh
    sum_rawgud_next = 0.0   # Σ raw_gud_{t+1} · Δh
    for i in range(len(rows) - 1):
        ts,   lhs,  rhs,  dec,  cc,    rg    = rows[i]
        ts_n, _,    rhs_n,_,    cc_n,  rg_n  = rows[i+1]
        if ts < warmup_pg:
            continue
        if dec != 'RAISE':
            continue
        n_total_raise += 1
        d_host_next = ts_n - ts
        dc_next     = cc_n - cc
        if dc_next <= 0:
            continue
        n_qualify += 1
        sum_lhs_t       += lhs   * d_host_next
        sum_act_next    += dc_next
        sum_rhs_t       += rhs   * d_host_next
        sum_rhs_next    += rhs_n * d_host_next
        sum_diff        += (rhs - lhs) * d_host_next
        sum_rawgud_t    += rg    * d_host_next
        sum_rawgud_next += rg_n  * d_host_next
    return dict(
        n_total_raise=n_total_raise,
        n_qualify=n_qualify,
        sum_lhs_t_pg=sum_lhs_t,
        sum_act_comp_pg=sum_act_next,
        sum_rhs_t_pg=sum_rhs_t,
        sum_rhs_next_pg=sum_rhs_next,
        sum_diff_pg=sum_diff,
        sum_rawgud_t_pg=sum_rawgud_t,
        sum_rawgud_next_pg=sum_rawgud_next,
    )


def main():
    WARMUP_TIB = 6
    warmup_pg = WARMUP_TIB * TIB // PAGE   # 1,610,612,736 pages

    rows = []
    for w in ['Ali1', 'Ali2', 'Ali3', 'YCSB-A']:
        refl = REFLASH_STAT[w]
        fifo = LOGFIFO_STAT[w]
        gsdec = REFLASH_GSDEC[w]
        if not (os.path.exists(refl) and os.path.exists(fifo) and os.path.exists(gsdec)):
            print(f"# MISSING {w}")
            continue
        # TEC measured from 6 TiB onward
        tec_r, dh_r, dc_r, de_r = tec_from_stat_after(refl, warmup_pg)
        tec_f, dh_f, dc_f, de_f = tec_from_stat_after(fifo, warmup_pg)
        refl_d = last_stat(refl)
        fifo_d = last_stat(fifo)
        a = analyze(gsdec, warmup_pg)
        rows.append((w, tec_r, tec_f, refl_d, fifo_d, a,
                     dh_r, dc_r, de_r, dh_f, dc_f, de_f))

    print(f"# warmup = {WARMUP_TIB} TiB host write, all sums after that")

    # --- table 1: LHS (EWMA) vs raw_gud_t vs raw_gud_{t+1} vs ΔC_{t+1} ---
    print("\n== (1) Gud 예측  vs  ΔC_{t+1} (다음 tick 실측 compact)   [TiB] ==")
    print(f"{'Workload':8} {'#qual':>6} {'ΣLHS(EWMA)':>11} {'Σraw_gud_t':>11} {'Σraw_gud_{t+1}':>15} {'Σact_ΔC':>10} | {'EWMA/act':>9} {'raw_t/act':>10} {'raw_n/act':>10}")
    for w, tr, tf, rd, fd, a, *_ in rows:
        l_ewma = a['sum_lhs_t_pg']       * PAGE / TIB
        l_raw  = a['sum_rawgud_t_pg']    * PAGE / TIB
        l_rawn = a['sum_rawgud_next_pg'] * PAGE / TIB
        ac     = a['sum_act_comp_pg']    * PAGE / TIB
        r_e    = l_ewma / ac if ac > 0 else 0
        r_r    = l_raw  / ac if ac > 0 else 0
        r_n    = l_rawn / ac if ac > 0 else 0
        print(f"{w:8} {a['n_qualify']:>6d} {l_ewma:>11.3f} {l_raw:>11.3f} {l_rawn:>15.3f} {ac:>10.3f} | {r_e:>9.2f} {r_r:>10.2f} {r_n:>10.2f}")

    # --- table 2: RHS_t (current) vs RHS_{t+1} (next) ---
    print("\n== (2) RHS_t  vs  RHS_{t+1}  (Δhost-weighted)  [TiB] ==")
    print(f"{'Workload':8} {'ΣRHS_t·Δh':>12} {'ΣRHS_{t+1}':>13} {'diff(next−t)':>14}")
    for w, tr, tf, rd, fd, a, *_ in rows:
        r_t  = a['sum_rhs_t_pg']    * PAGE / TIB
        r_n  = a['sum_rhs_next_pg'] * PAGE / TIB
        print(f"{w:8} {r_t:>12.3f} {r_n:>13.3f} {(r_n - r_t):>14.3f}")

    # --- table 3: ΔTEC vs Σ(RHS-LHS)·Δh (after 6 TiB) ---
    print("\n== (3) ΔTEC = TEC(LOGFIFO_AFTER_6TiB) − TEC(REFLASH)  vs  Σ(RHS−LHS)·Δh   [TiB] ==")
    print(f"{'Workload':8} {'TEC_LF':>9} {'TEC_REFL':>10} {'actSAVE':>10} {'Σ(R−L)·Δh':>11} {'pred/act':>10}")
    for w, tr, tf, rd, fd, a, *_ in rows:
        save_act  = tf - tr
        save_pred = a['sum_diff_pg'] * PAGE / TIB
        ratio     = save_pred / save_act if abs(save_act) > 1e-9 else float('nan')
        print(f"{w:8} {tf:>9.3f} {tr:>10.3f} {save_act:>10.3f} {save_pred:>11.3f} {ratio:>10.2f}")

    # --- extra: gv correction ---
    print("\n== (extra) Δgv·4K·r  added to predSAVE   [TiB] ==")
    print(f"{'Workload':8} {'gv_REFL(M)':>11} {'gv_LF(M)':>10} {'Δgv·4K·r':>11} {'predSAVE':>10} {'pred+corr':>11} {'actSAVE':>10} {'(p+c)/act':>10}")
    for w, tr, tf, rd, fd, a, *_ in rows:
        gv_r = rd['global_valid_blocks']
        gv_f = fd['global_valid_blocks']
        corr = (gv_r - gv_f) * PAGE * QLC / TIB
        save_pred = a['sum_diff_pg'] * PAGE / TIB
        save_act  = tf - tr
        ratio = (save_pred + corr) / save_act if abs(save_act) > 1e-9 else float('nan')
        print(f"{w:8} {gv_r/1e6:>11.2f} {gv_f/1e6:>10.2f} {corr:>11.3f} {save_pred:>10.3f} {(save_pred+corr):>11.3f} {save_act:>10.3f} {ratio:>10.2f}")


if __name__ == '__main__':
    os.chdir('/home/sejun000/ssd_waf')
    main()
