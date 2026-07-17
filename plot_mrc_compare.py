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

c1, m1 = read_mrc("/home/sejun000/ssd_waf/mrc_8tb.csv")
c2, m2 = read_mrc("/home/sejun000/ssd_waf/mrc_8tb_dwpd1.csv")

plt.figure(figsize=(12, 6))
plt.plot(c1, m1, 'b-', linewidth=1.5, label='dwpd0.3')
plt.plot(c2, m2, 'r-', linewidth=1.5, label='dwpd1')
plt.xlabel('Cache Size (GB)', fontsize=14)
plt.ylabel('Miss Rate (%)', fontsize=14)
plt.title('MRC Comparison: dwpd0.3 vs dwpd1 (8TB writes, LRU)', fontsize=14)
plt.legend(fontsize=12)
plt.grid(True, alpha=0.3)
plt.xlim(0, max(max(c1), max(c2)))
plt.ylim(0, 100)
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/mrc_compare.png', dpi=150)
plt.close()
print("Saved: mrc_compare.png")
