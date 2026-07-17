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
    ("mrc_8tb.csv", "alibaba dwpd0.3", "b"),
    ("mrc_8tb_dwpd1.csv", "alibaba dwpd1 (old)", "r"),
    ("mrc_8tb_alibaba_dwpd1.csv", "alibaba dwpd1.0", "m"),
    ("mrc_8tb_tencent.csv", "tencent high_dwpd", "g"),
    ("mrc_8tb_tencent_dwpd1.csv", "tencent dwpd1.0", "orange"),
    ("mrc_8tb_ssdtrace.csv", "ssdtrace (blktrace)", "cyan"),
]

plt.figure(figsize=(14, 7))
for fname, label, color in traces:
    c, m = read_mrc(f"/home/sejun000/ssd_waf/{fname}")
    plt.plot(c, m, '-', linewidth=1.5, color=color, label=label)

plt.xlabel('Cache Size (GB)', fontsize=14)
plt.ylabel('Miss Rate (%)', fontsize=14)
plt.title('MRC Comparison: All Traces (8TB writes, LRU, 4TB capacity)', fontsize=14)
plt.legend(fontsize=11)
plt.grid(True, alpha=0.3)
plt.ylim(0, 100)
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/mrc_all.png', dpi=150)
plt.close()
print("Saved: mrc_all.png")
