import glob, re, numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

WARMUP_TB = 6
WARMUP_BYTES = WARMUP_TB * (1024**4)
BLK_TO_TB = 4096 / (1000**4)

BASE = '/home/sejun000/ssd_waf'

def load_trace(pattern):
    files = glob.glob(pattern)
    data = {}
    for f in files:
        m = re.search(r'dp\.([\d.]+)$', f)
        if not m:
            continue
        dp_num = float(m.group(1))
        if dp_num < 0.55:
            continue
        baseline = None
        last = None
        with open(f) as fh:
            for line in fh:
                if 'compacted_blocks:' not in line or 'evicted_blocks:' not in line:
                    continue
                wm = re.search(r'write_size_to_cache:\s*(\d+)', line)
                cm = re.search(r'compacted_blocks:\s*(\d+)', line)
                if not (wm and cm):
                    continue
                w, c = int(wm.group(1)), int(cm.group(1))
                if baseline is None and w >= WARMUP_BYTES:
                    baseline = c
                last = c
        if last is not None and baseline is not None:
            data[round(dp_num, 2)] = (last - baseline) * BLK_TO_TB
    return data


def compute_ratio(data, max_u=0.78):
    xs, ys = [], []
    for u in sorted(data.keys()):
        if u > max_u:
            break
        u_plus = round(u + 0.10, 2)
        if u_plus in data and data[u] > 0:
            xs.append(u)
            ys.append(data[u_plus] / data[u])
    return xs, ys


# Agarwal's model: f(u) = (1+OP)/(2*OP) - 1, OP = 1/u - 1
# simplifies to f(u) = (2u-1)/(2(1-u))
def agarwal_model(u):
    return (2 * u - 1) / (2 * (1 - u))


workloads = [
    ('ssdtrace', 'YCSB-A'),
    ('dwpd1', 'Alibaba1'),
    ('dwpd2', 'Alibaba2'),
    ('dwpd3', 'Alibaba3'),
]

greedy_traces = [(load_trace(f'{BASE}/greedy_11_{w}/dp.*'), label) for w, label in workloads]
cb_traces = [(load_trace(f'{BASE}/cb_11_{w}/dp.*'), label) for w, label in workloads]

colors = ['tab:blue', 'tab:orange', 'tab:green', 'tab:red']
markers = ['o', 's', '^', 'D']

plt.rcParams.update({'font.size': 50})
fig, axes = plt.subplots(2, 2, figsize=(32, 16))

us_model = np.linspace(0.55, 0.88, 300)
HOST_WRITE_TB = 8.0
ys_model_gc = [agarwal_model(u) * HOST_WRITE_TB for u in us_model]

us_ratio = np.arange(0.55, 0.79, 0.01)
theo_ratio = [agarwal_model(u + 0.1) / agarwal_model(u) for u in us_ratio]

# Top row: GC(u), Bottom row: GC(u+0.1)/GC(u)
# Left col: Greedy, Right col: REFlash
panels = [
    (axes[0][0], greedy_traces, 'Greedy'),
    (axes[0][1], cb_traces, 'REFlash'),
]

for ax, traces_list, ftl_name in panels:
    for (data, label), c, mk in zip(traces_list, colors, markers):
        xs = sorted([u for u in data.keys() if u <= 0.88])
        xs = xs[::3]  # every 3rd point for sparser top plot
        ys = [data[u] for u in xs]
        ax.plot(xs, ys, color=c, marker=mk, markersize=10, linewidth=2, label=label)
    ax.plot(us_model, ys_model_gc, color='black', linestyle='--', linewidth=2.5,
            label="Agarwal's Model")
    ax.set_xlabel('')
    ax.set_ylabel('Host-level\nGC writes (TB)')
    ax.set_ylim(bottom=0)
    ax.set_title('')
    ax.set_xticks([0.55, 0.65, 0.75, 0.85])
    ax.grid(True, alpha=0.3, color='black', linestyle='--')
    ax.set_axisbelow(True)

axes[0][1].set_ylabel('')

panels_ratio = [
    (axes[1][0], greedy_traces, 'Greedy'),
    (axes[1][1], cb_traces, 'REFlash'),
]

for ax, traces_list, ftl_name in panels_ratio:
    for (data, label), c, mk in zip(traces_list, colors, markers):
        xs, ys = compute_ratio(data)
        ax.plot(xs, ys, color=c, marker=mk, markersize=8, linewidth=2, label=label)
    ax.plot(us_ratio, theo_ratio, color='black', linestyle='--', linewidth=2.5,
            label="Agarwal's Model")
    ax.set_xlabel(r'$U_B$ (u)')
    ax.set_ylabel('GC(u+0.10) / GC(u)')
    ax.set_yticks([0, 2, 4, 6, 8])
    ax.set_ylim(0, 9)
    ax.set_title('')
    ax.grid(True, alpha=0.3, color='black', linestyle='--')
    ax.set_axisbelow(True)

axes[1][1].set_ylabel('')

# Add column labels below bottom row
axes[1][0].text(0.5, -0.30, '(a) Greedy', transform=axes[1][0].transAxes,
                ha='center', fontsize=50, va='top')
axes[1][1].text(0.5, -0.30, '(b) REFlash', transform=axes[1][1].transAxes,
                ha='center', fontsize=50, va='top')

# Shared legend at top
handles, labels = axes[0][0].get_legend_handles_labels()
fig.legend(handles, labels, loc='upper center', ncol=len(labels),
           frameon=False, fontsize=50,
           bbox_to_anchor=(0.5, 0.998),
           handlelength=1.2, columnspacing=1.0)
plt.subplots_adjust(wspace=0.15, hspace=0.18, left=0.10, right=0.95, bottom=0.18, top=0.91)
plt.savefig(f'{BASE}/A_dp_gc_expfit.pdf', dpi=150)
print('Saved A_dp_gc_expfit.pdf')
