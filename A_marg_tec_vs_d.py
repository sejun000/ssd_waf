#!/usr/bin/env python3
# TEC vs dN(D) for the marginal forward-diff rule, per periodic_ratio r.
# Reads the last stat line of each run; TEC = host + comp + R*evict.
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BLK = 4096
Ds  = [1, 2, 4, 8, 16]
RS  = {"288": 2.88, "864": 8.64}
ORIG = {"288": 32.25, "864": 55.13}   # orig Fpred-Fghost baseline TEC

def last_stat(path):
    last = None
    with open(path) as f:
        for line in f:
            if "write_size_to_cache:" in line:
                last = line
    return last

def parse(line):
    t = line.split()
    d = {}
    for i, tok in enumerate(t):
        if tok.endswith(":"):
            try: d[tok[:-1]] = float(t[i+1])
            except (ValueError, IndexError): pass
    return d

def tec_of(D, rt):
    R = RS[rt]
    f = f"LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_ewma_hl1572864_marg_gsD{D}_r{rt}.stat"
    d = parse(last_stat(f))
    host  = d["write_size_to_cache"] / 1e12
    comp  = d["compacted_blocks"] * BLK / 1e12
    evict = d["evicted_blocks"]   * BLK / 1e12
    return host + comp + R * evict

fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
for ax, rt in zip(axes, ["288", "864"]):
    tecs = [tec_of(D, rt) for D in Ds]
    ax.plot(Ds, tecs, "o-", lw=2, ms=7, color="C0", label="marginal rule")
    ax.axhline(ORIG[rt], ls="--", color="C3", lw=1.5,
               label=f"orig Fpred-Fghost ({ORIG[rt]:.2f})")
    # annotate each point
    for D, te in zip(Ds, tecs):
        ax.annotate(f"{te:.2f}", (D, te), textcoords="offset points",
                    xytext=(0, 8), ha="center", fontsize=8)
    # mark the min
    bi = tecs.index(min(tecs))
    ax.plot(Ds[bi], tecs[bi], "*", ms=16, color="gold",
            markeredgecolor="k", zorder=5)
    ax.set_ylim(0, 65)                       # 0-base y-axis
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ds); ax.set_xticklabels(Ds)
    ax.set_xlabel("dN (= gs_decision_period_segs, marginal breadth)")
    ax.set_ylabel("TEC  (host + comp + R*evict)  [TB]")
    ax.set_title(f"r = {RS[rt]:.2f}")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9)

fig.suptitle("REFLASH marginal rule: TEC vs dN", fontweight="bold")
fig.tight_layout()
fig.savefig("A_marg_tec_vs_d.pdf")
fig.savefig("A_marg_tec_vs_d.png", dpi=130)
print("wrote A_marg_tec_vs_d.pdf / .png")
for rt in ["288", "864"]:
    print(f"r={RS[rt]:.2f}:", " ".join(f"D{D}={tec_of(D,rt):.2f}" for D in Ds))
