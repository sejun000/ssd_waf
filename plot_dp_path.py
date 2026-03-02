import matplotlib.pyplot as plt
import csv
import sys

filename = sys.argv[1] if len(sys.argv) > 1 else "cb_11_dwpd2_dp_qlc864.txt"

ts, cs = [], []
with open(filename) as f:
    for line in f:
        line = line.strip()
        if line.startswith("t,"):
            continue  # header
        parts = line.split(", ")
        if len(parts) != 5:
            continue
        try:
            t = int(parts[0])
            c = float(parts[1])
            ts.append(t)
            cs.append(c)
        except ValueError:
            continue

fig, ax = plt.subplots(figsize=(14, 5))
ax.plot(ts, cs, linewidth=0.8, color='tab:blue', alpha=0.8)
ax.scatter(ts, cs, s=3, color='tab:blue', zorder=3)
ax.set_xlabel('Time Step')
ax.set_ylabel('Chosen Valid Ratio')
ax.set_ylim(0.0, 1.0)
ax.grid(True, alpha=0.3)
ax.set_title(f'DP Optimal Path (QLC_FACTOR=8.64, {len(ts)} steps)')
plt.tight_layout()
outfile = filename.replace('.txt', '_path.png')
plt.savefig(outfile, dpi=150)
print(f"Saved to {outfile}")
