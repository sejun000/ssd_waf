#!/usr/bin/env python3
"""
Read ssdtrace rawfile, extract Q events, scale LBA*4 and size*4,
stop after 2TB of writes. Output to csv format: dev,direction,offset_bytes,size_bytes,timestamp_ns
"""
import sys

input_file = "/home/sejun000/ssdtrace/rawfile"
output_file = "/mnt/ramdisk/ssdtrace_scaled_4x.trace"
WRITE_LIMIT = 8 * 1024 * 1024 * 1024 * 1024  # 8TB (원본 2TB * size 4x)

print(f"Input:  {input_file}")
print(f"Output: {output_file}")
print(f"Write limit: 2TB")
print(f"Scale: LBA*4, size*4")

count = 0
writes = 0
reads = 0
write_bytes = 0
read_bytes = 0

with open(input_file, 'r') as fin, open(output_file, 'w') as fout:
    for line in fin:
        parts = line.split()
        if len(parts) < 10:
            continue

        # Format: dev cpu seq timestamp pid action direction lba + size [proc]
        # e.g.: 259,2  0  1  0.000000000  4020  Q  R  282624 + 8 [java]
        action = parts[5]
        if action != 'Q':
            continue

        direction_raw = parts[6]
        if direction_raw.startswith('W'):
            direction = 'W'
        elif direction_raw.startswith('R'):
            direction = 'R'
        else:
            continue

        try:
            lba = int(parts[7])
            nsectors = int(parts[9])
        except (ValueError, IndexError):
            continue

        if nsectors == 0:
            continue

        # Scale: LBA*4, size*4
        offset_bytes = lba * 512 * 4
        size_bytes = nsectors * 512 * 4

        # Parse timestamp
        timestamp_str = parts[3]
        sec_frac = timestamp_str.split('.')
        if len(sec_frac) == 2:
            timestamp_ns = int(sec_frac[0]) * 1000000000 + int(sec_frac[1])
        else:
            timestamp_ns = 0

        if direction == 'W':
            write_bytes += size_bytes
            writes += 1
        else:
            read_bytes += size_bytes
            reads += 1

        fout.write(f"0,{direction},{offset_bytes},{size_bytes},{timestamp_ns}\n")
        count += 1

        if count % 10000000 == 0:
            print(f"  {count//1000000}M IOs, writes={writes}, write_data={write_bytes/1024/1024/1024:.1f}GB")

        if write_bytes >= WRITE_LIMIT:
            print(f"  Write limit 2TB reached!")
            break

print(f"\nDone.")
print(f"Total IOs:   {count:,}")
print(f"Reads:       {reads:,} ({read_bytes/1024/1024/1024:.1f} GB)")
print(f"Writes:      {writes:,} ({write_bytes/1024/1024/1024:.1f} GB)")
