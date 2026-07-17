import csv
import matplotlib.pyplot as plt

def read_mrc(path):
    cache_gb, miss_rate = [], []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            cache_gb.append(int(row["CacheSize(blocks)"]) * 4096 / 1e9)
            miss_rate.append(float(row["MissRate(%)"]))
    return cache_gb, miss_rate

traces = [
    ("mrc_12tb_dwpd2_5x.csv", "Alibaba DWPD>=2 (5x)", "r"),
    ("mrc_12tb_dwpd1to2_4x.csv", "Alibaba DWPD 1~2 (4x)", "b"),
    ("mrc_12tb_dwpd05to1_7x.csv", "Alibaba DWPD 0.5~1 (7x)", "orange"),
    ("mrc_12tb_ssdtrace_16x.csv", "SSD Trace (16x)", "g"),
]

plt.figure(figsize=(14, 7))
for fname, label, color in traces:
    c, m = read_mrc(f"/home/sejun000/ssd_waf/{fname}")
    plt.plot(c, m, '-', linewidth=1.5, color=color, label=label)

plt.xlabel('Cache Size (GB)', fontsize=14)
plt.ylabel('Miss Rate (%)', fontsize=14)
plt.title('MRC: Scaled Traces (12TB writes, LRU, 4TB capacity)', fontsize=14)
plt.legend(fontsize=11)
plt.grid(True, alpha=0.3)
plt.ylim(0, 100)
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/mrc_scaled.png', dpi=150)
plt.close()
print("Saved: mrc_scaled.png")
