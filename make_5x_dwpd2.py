#!/usr/bin/env python3
"""
Filter DWPD>=2 volumes from alibaba trace, duplicate 5x with LBA offsets.
Output limited to 8TB writes.
"""
import sys

TRACE = "/mnt/ramdisk/alibaba_block_traces_2020/io_traces.csv"
OUTPUT = "/mnt/ramdisk/alibaba_dwpd2_5x.trace"

# DWPD>=2 volume IDs from write_volume_result.csv
DWPD2_VOLS = {225, 144, 58, 38, 277, 679, 177}

# Total device size of DWPD>=2 volumes: 1730 GB
# Use this as LBA offset between copies
OFFSET = int(1730 * 1024 * 1024 * 1024)  # 1730 GB in bytes

N_COPIES = 5
WRITE_LIMIT = 12 * 1000 * 1000 * 1000 * 1000  # 12TB

written_bytes = 0
line_count = 0
next_print = 100 * 1000 * 1000 * 1000  # 100GB

print(f"Filtering volumes: {DWPD2_VOLS}")
print(f"LBA offset per copy: {OFFSET} bytes ({OFFSET/1e9:.1f} GB)")
print(f"Copies: {N_COPIES}, Write limit: {WRITE_LIMIT/1e12:.0f} TB")
sys.stdout.flush()

with open(TRACE) as fin, open(OUTPUT, 'w') as fout:
    for line in fin:
        parts = line.strip().split(',')
        if len(parts) < 5:
            continue

        vol_id = int(parts[0])
        if vol_id not in DWPD2_VOLS:
            continue

        op = parts[1]
        lba = int(parts[2])
        size = int(parts[3])
        ts = parts[4]

        # Write 5 copies with different LBA offsets
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
