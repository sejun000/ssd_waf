#!/usr/bin/env python3
"""Sanity-check: print measured vs oracle side-by-side at a few timepoints."""
import sys, os, importlib.util
spec = importlib.util.spec_from_file_location("aoe", "A_oracle_err.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

oracles = m.load_oracles()
for s in [1, 4, 16]:
    path = m.GSDEC_FMT.format(s=s)
    rec = m.parse_gsdec(path)
    print(f"\n=== seg={s} ({len(rec)} records) ===")
    print(f"{'i':>4} {'ts':>11} {'u':>6} {'tgt':>6} | "
          f"{'G_u':>8} {'G_o(u)':>8} | {'G_ud':>8} {'G_o(tgt)':>8} | "
          f"{'F_u':>8} {'F_o(u)':>8} | {'F_ud':>8} {'F_o(tgt)':>8}")
    idxs = [int(len(rec)*x) for x in [0.1, 0.25, 0.5, 0.75, 0.9]]
    for i in idxs:
        r = rec[i]
        ts, u, tgt, Gu, Fu, Gud, Fud = r
        Go_u, Fo_u   = m.lookup_oracle(oracles, u, ts)
        Go_ud, Fo_ud = m.lookup_oracle(oracles, tgt, ts)
        print(f"{i:>4} {int(ts):>11} {u:>6.3f} {tgt:>6.3f} | "
              f"{Gu:>8.4f} {Go_u:>8.4f} | {Gud:>8.4f} {Go_ud:>8.4f} | "
              f"{Fu:>8.4f} {Fo_u:>8.4f} | {Fud:>8.4f} {Fo_ud:>8.4f}")
