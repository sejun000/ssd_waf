#!/usr/bin/env python3
"""
Read varmail 2TB blktrace, extract Q events, scale LBA*6 and size*6.
Output csv format: dev,direction,offset_bytes,size_bytes,timestamp_ns
"""
import sys

input_file = "/home/sejun000/ssd_waf/varmail_2tb_bt/trace"
output_file = "/mnt/ramdisk/varmail_2tb_6x.trace"

SCALE = 6

print(f"Input:  {input_file} (blktrace)")
print(f"Output: {output_file}")
print(f"Scale: LBA*{SCALE}, size*{SCALE}")
sys.stdout.flush()

count = 0
writes = 0
reads = 0
write_bytes = 0
read_bytes = 0

import subprocess

proc = subprocess.Popen(
    ["blkparse", "-i", input_file, "-q", "-f", "%a %d %S %n %T.%t\n"],
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    text=True,
    bufsize=1024*1024
)

with open(output_file, 'w', buffering=1024*1024) as fout:
    for line in proc.stdout:
        parts = line.split()
        if len(parts) < 5:
            continue

        action = parts[0]
        if action != 'Q':
            continue

        direction_raw = parts[1]
        if direction_raw.startswith('W'):
            direction = 'W'
        elif direction_raw.startswith('R'):
            direction = 'R'
        else:
            continue

        try:
            lba = int(parts[2])
            nsectors = int(parts[3])
        except (ValueError, IndexError):
            continue

        if nsectors == 0:
            continue

        # Parse timestamp
        timestamp_str = parts[4]
        sec_frac = timestamp_str.split('.')
        if len(sec_frac) == 2:
            timestamp_ns = int(sec_frac[0]) * 1000000000 + int(sec_frac[1])
        else:
            timestamp_ns = 0

        # Scale: LBA*6, size*6
        offset_bytes = lba * 512 * SCALE
        size_bytes = nsectors * 512 * SCALE

        if direction == 'W':
            write_bytes += size_bytes
            writes += 1
        else:
            read_bytes += size_bytes
            reads += 1

        fout.write(f"0,{direction},{offset_bytes},{size_bytes},{timestamp_ns}\n")
        count += 1

        if count % 10000000 == 0:
            print(f"  {count//1000000}M IOs, writes={writes:,}, write_data={write_bytes/1024/1024/1024:.1f}GB")
            sys.stdout.flush()

proc.wait()

print(f"\nDone.")
print(f"Total IOs:   {count:,}")
print(f"Reads:       {reads:,} ({read_bytes/1024/1024/1024:.1f} GB)")
print(f"Writes:      {writes:,} ({write_bytes/1024/1024/1024:.1f} GB)")
print(f"Expected WSS: ~{395*SCALE:.0f} GB, Write volume: ~{1493*SCALE:.0f} GB")
