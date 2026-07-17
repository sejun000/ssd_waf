#!/usr/bin/env python3
"""
Filter 0.5<=DWPD<1 volumes from alibaba trace, duplicate 7x with LBA offsets.
Output as csv format, limited to 8TB writes.
"""
import sys
import csv

SOURCE = "/mnt/ramdisk/alibaba_block_traces_2020/io_traces.csv"
OUTPUT = "/mnt/ramdisk/alibaba_dwpd05to1_7x.trace"

# Load 0.5<=DWPD<1 volume IDs
VOLS = set()
with open('/mnt/nvme0/write_volume_result.csv') as f:
    reader = csv.DictReader(f)
    for row in reader:
        d = float(row['DWPD'])
        if 0.5 <= d < 1.0:
            VOLS.add(int(row['VolumeID']))

# Total device size: 1350 GB
OFFSET = int(1350 * 1024 * 1024 * 1024)
OFFSET_SECTORS = OFFSET // 512

N_COPIES = 7
WRITE_LIMIT = 12 * 1000 * 1000 * 1000 * 1000  # 12TB

print(f"Filtering {len(VOLS)} volumes with 0.5<=DWPD<1: {VOLS}")
print(f"LBA offset per copy: {OFFSET_SECTORS} sectors ({OFFSET/1e9:.0f} GB)")
print(f"Copies: {N_COPIES}, Write limit: {WRITE_LIMIT/1e12:.0f} TB")
sys.stdout.flush()

written_bytes = 0
line_count = 0
next_print = 100 * 1000 * 1000 * 1000

with open(OUTPUT, 'w') as fout:
    with open(SOURCE) as fin:
        for line in fin:
            parts = line.strip().split(',')
            if len(parts) < 5:
                continue
            vol_id = int(parts[0])
            if vol_id not in VOLS:
                continue

            op = parts[1]  # "W" or "R"
            offset_bytes = int(parts[2])
            size_bytes = int(parts[3])
            ts = parts[4]

            for i in range(N_COPIES):
                new_offset = offset_bytes + i * OFFSET
                fout.write(f"0,{op},{new_offset},{size_bytes},{ts}\n")
                line_count += 1
                if op == "W":
                    written_bytes += size_bytes

            if written_bytes >= next_print:
                print(f"Written: {written_bytes/1e12:.2f} TB, lines: {line_count/1e6:.1f}M")
                sys.stdout.flush()
                next_print += 100 * 1000 * 1000 * 1000
            if written_bytes >= WRITE_LIMIT:
                break

print(f"Done. Lines: {line_count}, Writes: {written_bytes/1e12:.2f} TB")
print(f"Output: {OUTPUT}")
