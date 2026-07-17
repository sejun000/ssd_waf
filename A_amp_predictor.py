#!/usr/bin/env python3
"""
3 comparisons under filter: RAISE & K>0 & dC>0  (lag-1 sync).

  (1)  Σ(G_ud·seg)              vs  Σ dC (actual compacted_blocks)
  (2)  Σ pred_inv_rate + avg_inv_rate_per_blk · Δgv   vs   Δinv = inv_GS − inv_LF
  (3)  Σ(G_ud·seg − pred_inv_rate) + Δgv              vs   ΔTEC = TEC_LF − TEC_GS

  Δ  = GS − LF.
  pred_inv_rate row column = invpred_pred_surv (cohort-fair survivor predictor).
  avg_inv_rate_per_blk    = inv_GS / host_GS_blocks (P(inv per host write)).

Usage:
  python3 A_amp_predictor.py <gsdec.log> <GS.stat> <LF.stat> [--r 8.64] [--seg 1572864] [--blk 4096]
"""
import argparse

SEG_DEFAULT = 1572864   # blocks per segment
BLK_DEFAULT = 4096      # bytes per block
R_DEFAULT   = 8.64      # periodic_ratio


def gsdec_cols(header):
    return {name: i for i, name in enumerate(header.strip().split())}


def walk_gsdec(path):
    """Lag-1 sync: row T-1 (decision/K/G_ud/invpred_*) ↔ row T (dC/dG/dF).

    Filter: prev_dec=="RAISE" & prev_K>0 & dC>0.
    """
    with open(path) as f:
        c = gsdec_cols(f.readline())
        need = ['decision', 'G_ud', 'gcsim_m', 'comp_cum',
                'invpred_pred_surv']
        for k in need:
            if k not in c: raise SystemExit(f"missing column: {k}")

        prev = None
        out = dict(n=0, sum_gud_ratio=0.0, sum_dC=0.0,
                   sum_pred_inv=0.0)

        for line in f:
            parts = line.split()
            if len(parts) < len(c): continue
            cur = dict(
                comp = float(parts[c['comp_cum']]),
                dec  = parts[c['decision']],
                K    = float(parts[c['gcsim_m']]),
                gud  = float(parts[c['G_ud']]),
                pinv = float(parts[c['invpred_pred_surv']]),
            )
            if prev is None:
                prev = cur; continue
            dC = cur['comp'] - prev['comp']
            if prev['dec'] == 'RAISE' and prev['K'] > 0 and dC > 0:
                out['n']             += 1
                out['sum_gud_ratio'] += prev['gud']
                out['sum_dC']        += dC
                out['sum_pred_inv']  += prev['pinv']
            prev = cur
        return out


def parse_stat(path):
    """Return final-record key:value (last 'main' row of stat file)."""
    last = None
    with open(path) as f:
        for line in f:
            toks = line.split()
            if len(toks) > 10:
                last = toks
    if last is None: raise SystemExit(f"no main row in {path}")
    keys = {
        'invalidate_blocks:': 'inv',
        'compacted_blocks:': 'comp',
        'evicted_blocks:': 'evict',
        'global_valid_blocks:': 'gv',
        'write_size_to_cache:': 'host_bytes',
    }
    out = {}
    for i in range(len(last) - 1):
        k = last[i]
        if k in keys:
            try: out[keys[k]] = int(last[i+1])
            except ValueError: pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gsdec')
    ap.add_argument('gs_stat')
    ap.add_argument('lf_stat')
    ap.add_argument('--r',   type=float, default=R_DEFAULT)
    ap.add_argument('--seg', type=int,   default=SEG_DEFAULT)
    ap.add_argument('--blk', type=int,   default=BLK_DEFAULT)
    args = ap.parse_args()

    f = walk_gsdec(args.gsdec)
    gs = parse_stat(args.gs_stat)
    lf = parse_stat(args.lf_stat)

    BLK_TB = args.blk / 1e12
    def TB(blocks): return blocks * BLK_TB

    # Filtered sums in BLOCKS
    sum_gud_blocks      = f['sum_gud_ratio'] * args.seg
    sum_dC_blocks       = f['sum_dC']
    sum_pred_inv_blocks = f['sum_pred_inv']

    # System-level deltas (GS - LF)
    delta_gv_blocks  = gs['gv']  - lf['gv']
    delta_inv_blocks = gs['inv'] - lf['inv']
    host_GS_blocks   = gs['host_bytes'] / args.blk
    avg_inv_per_blk  = gs['inv'] / host_GS_blocks if host_GS_blocks else 0.0

    # TEC
    tec_lf = TB(lf['host_bytes']/args.blk) + 0.0           + args.r * TB(lf['evict'])
    tec_gs = TB(gs['host_bytes']/args.blk) + TB(gs['comp']) + args.r * TB(gs['evict'])
    delta_TEC = tec_lf - tec_gs

    print(f"# Inputs: r={args.r} seg={args.seg} blk={args.blk}B   "
          f"gsdec={args.gsdec}")
    print(f"#         GS_stat={args.gs_stat}")
    print(f"#         LF_stat={args.lf_stat}")
    print(f"# Filter: RAISE & K>0 & dC>0  (lag-1 sync: decision[T-1] ↔ dC[T])   "
          f"n_rows={f['n']}")
    print()

    comp_GS_blocks = gs['comp']
    print(f"(1) Σ(G_ud·seg)  vs  compacted_blocks (from GS stat file)")
    print(f"    Σ G_ud·seg          = {TB(sum_gud_blocks):8.3f} TB   (gsdec filter sum)")
    print(f"    compacted_blocks GS = {TB(comp_GS_blocks):8.3f} TB   (stat file, total)")
    print(f"    Σ dC (filter sum)   = {TB(sum_dC_blocks):8.3f} TB   (gsdec, RAISE & K>0 & dC>0)")
    if comp_GS_blocks > 0:
        print(f"    ratio Gud / compGS  = {sum_gud_blocks/comp_GS_blocks:.3f}x")
    print()

    pred2 = sum_pred_inv_blocks + avg_inv_per_blk * delta_gv_blocks
    print(f"(2) Σ pred_inv + avg_rate·Δgv  vs  Δinv  (stat file: inv_GS - inv_LF)")
    print(f"    Σ pred_inv_rate            = {TB(sum_pred_inv_blocks):8.3f} TB   (gsdec filter sum, pred_surv)")
    print(f"    avg_inv/blk                = {avg_inv_per_blk:.4f}   (= inv_GS / host_GS_blocks, stat)")
    print(f"    Δgv (gs-lf)                = {TB(delta_gv_blocks):+8.3f} TB ({delta_gv_blocks:+d} blk, stat)")
    print(f"    avg_rate · Δgv             = {TB(avg_inv_per_blk * delta_gv_blocks):+8.3f} TB")
    print(f"    LHS = pred + rate·Δgv      = {TB(pred2):8.3f} TB")
    print(f"    Δinv = GS - LF             = {TB(delta_inv_blocks):8.3f} TB   "
          f"(stat: inv_GS={TB(gs['inv']):.3f}  inv_LF={TB(lf['inv']):.3f})")
    if delta_inv_blocks > 0:
        print(f"    ratio LHS/Δinv             = {pred2/delta_inv_blocks:.3f}x")
    print()

    pred3_raw = (sum_gud_blocks - sum_pred_inv_blocks) + delta_gv_blocks
    pred3_r   = -sum_gud_blocks + args.r * (sum_pred_inv_blocks + delta_gv_blocks)
    print(f"(3) predicted savings  vs  ΔTEC (= TEC_LF - TEC_GS, stat file)")
    print(f"    Σ G_ud·seg                       = {TB(sum_gud_blocks):8.3f} TB")
    print(f"    Σ pred_inv_rate                  = {TB(sum_pred_inv_blocks):8.3f} TB")
    print(f"    Δgv                              = {TB(delta_gv_blocks):+8.3f} TB")
    print(f"    TEC_GS  = host + comp + r·evict  = {tec_gs:8.3f} TB")
    print(f"    TEC_LF  = host + 0    + r·evict  = {tec_lf:8.3f} TB")
    print(f"    ΔTEC                             = {delta_TEC:8.3f} TB")
    print()
    print(f"    (3a) raw:        Σ(Gud·seg - pred)  + Δgv     = {TB(pred3_raw):8.3f} TB")
    if delta_TEC > 0:
        print(f"         ratio LHS/ΔTEC                            = {TB(pred3_raw)/delta_TEC:.3f}x")
    print(f"    (3b) r-weighted: Σ(r·pred - Gud·seg) + r·Δgv  = {TB(pred3_r):8.3f} TB")
    print(f"         (conservation: ΔTEC = -comp_GS + r·Δinv + r·Δgv)")
    if delta_TEC > 0:
        print(f"         ratio LHS/ΔTEC                            = {TB(pred3_r)/delta_TEC:.3f}x")


if __name__ == '__main__':
    main()
