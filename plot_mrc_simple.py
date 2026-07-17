import csv
import matplotlib.pyplot as plt

cache_gb = []
miss_rate = []

with open("/home/sejun000/ssd_waf/mrc_8tb.csv") as f:
    reader = csv.DictReader(f)
    for row in reader:
        cache_gb.append(int(row["CacheSize(blocks)"]) * 4096 / 1e9)
        miss_rate.append(float(row["MissRate(%)"]))

plt.figure(figsize=(12, 6))
plt.plot(cache_gb, miss_rate, 'b-', linewidth=1.5)
plt.xlabel('Cache Size (GB)', fontsize=14)
plt.ylabel('Miss Rate (%)', fontsize=14)
plt.title('MRC: Cache Size vs Miss Rate (alibaba_dwpd0.3, 8TB writes, LRU)', fontsize=14)
plt.grid(True, alpha=0.3)
plt.xlim(0, max(cache_gb))
plt.ylim(0, 100)
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/mrc_simple.png', dpi=150)
plt.close()
print("Saved: mrc_simple.png")
