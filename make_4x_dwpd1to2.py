#!/usr/bin/env python3
"""
Filter 1<=DWPD<2 volumes from alibaba trace, duplicate 4x with LBA offsets.
Output limited to 8TB writes.
"""
import sys

TRACE = "/mnt/ramdisk/alibaba_block_traces_2020/io_traces.csv"
OUTPUT = "/mnt/ramdisk/alibaba_dwpd1to2_4x.trace"

# 1<=DWPD<2 volume IDs
VOLS = {141, 107, 178, 52, 721, 748, 316, 132, 223, 326}

# Total device size: 2430 GB
OFFSET = int(2430 * 1024 * 1024 * 1024)

N_COPIES = 4
WRITE_LIMIT = 16 * 1000 * 1000 * 1000 * 1000  # 16TB

written_bytes = 0
line_count = 0
next_print = 100 * 1000 * 1000 * 1000

print(f"Filtering volumes: {VOLS}")
print(f"LBA offset per copy: {OFFSET} bytes ({OFFSET/1e9:.1f} GB)")
print(f"Copies: {N_COPIES}, Write limit: {WRITE_LIMIT/1e12:.0f} TB")
sys.stdout.flush()

with open(TRACE) as fin, open(OUTPUT, 'w') as fout:
    for line in fin:
        parts = line.strip().split(',')
        if len(parts) < 5:
            continue

        vol_id = int(parts[0])
        if vol_id not in VOLS:
            continue

        op = parts[1]
        lba = int(parts[2])
        size = int(parts[3])
        ts = parts[4]

        for i in range(N_COPIES):
            new_lba = lba + i * OFFSET
            fout.write(f"0,{op},{new_lba},{size},{ts}\n")
            line_count += 1

            if op == 'W' or op == 'WS':
                written_bytes += size

        if written_bytes >= next_print:
            print(f"Written: {written_bytes/1e12:.2f} TB, lines: {line_count/1e6:.1f}M")
            sys.stdout.flush()
            next_print += 100 * 1000 * 1000 * 1000

        if written_bytes >= WRITE_LIMIT:
            print(f"Reached {WRITE_LIMIT/1e12:.0f} TB write limit.")
            break

print(f"Done. Lines: {line_count}, Writes: {written_bytes/1e12:.2f} TB")
print(f"Output: {OUTPUT}")
