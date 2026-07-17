#!/usr/bin/env python3
"""
Select high-write DWPD 0.1~1.0 devices, remap to ~4TB SSD, cut at 8TB writes.
Stream from front (already timestamp-sorted), no full load needed.
"""
import sys

INPUT_TRACE = "/mnt/ramdisk/alibaba_block_traces_2020/io_traces.csv"
OUTPUT_TRACE = "/mnt/ramdisk/alibaba_dwpd01to1.trace"

# Selected devices: sorted by write volume desc, cumWSS ~4.2TB
# (VolumeID, DeviceSizeGB)
SELECTED = [
    (804, 1024.0),
    (32,  500.0),
    (746, 500.0),
    (810, 1024.0),
    (4,   500.0),
    (740, 1000.0),
    (742, 300.0),
    (79,  300.0),
    (29,  500.0),
]

TARGET_WRITE_BYTES = 8 * 1024 * 1024 * 1024 * 1024  # 8TB

# Build offset map: each device gets a contiguous region
device_set = set()
offset_map = {}
current_offset = 0
for vol_id, size_gb in SELECTED:
    device_set.add(vol_id)
    offset_map[vol_id] = current_offset
    current_offset += int(size_gb * 1024 * 1024 * 1024)

total_capacity = current_offset
print(f"Total SSD capacity: {total_capacity / (1024**4):.2f} TB", flush=True)
print(f"Selected devices: {sorted(device_set)}", flush=True)

# Stream: read from front, filter, remap, write, stop at 8TB writes
print(f"\nStreaming {INPUT_TRACE} ...", flush=True)
write_bytes = 0
out_count = 0
read_count = 0
line_count = 0

with open(INPUT_TRACE, 'r') as fin, open(OUTPUT_TRACE, 'w') as fout:
    for line in fin:
        line_count += 1
        if line_count % 50_000_000 == 0:
            print(f"  {line_count // 1_000_000}M lines, out={out_count // 1_000_000}M, writes={write_bytes / (1024**4):.2f}TB", flush=True)

        parts = line.rstrip('\n').split(',')
        if len(parts) < 5:
            continue

        try:
            dev_id = int(parts[0])
        except ValueError:
            continue

        if dev_id not in device_set:
            continue

        rw = parts[1]
        try:
            offset = int(parts[2])
            size = int(parts[3])
            timestamp = int(parts[4])
        except ValueError:
            continue

        new_offset = offset_map[dev_id] + offset
        fout.write(f"0,{rw},{new_offset},{size},{timestamp}\n")
        out_count += 1

        if rw == 'W':
            write_bytes += size
            if write_bytes >= TARGET_WRITE_BYTES:
                print(f"  Reached 8TB writes at line {line_count}, record {out_count}", flush=True)
                break
        else:
            read_count += 1

print(f"\nDone!", flush=True)
print(f"  Lines scanned: {line_count}", flush=True)
print(f"  Records out: {out_count} (R={read_count}, W={out_count - read_count})", flush=True)
print(f"  Write volume: {write_bytes / (1024**4):.2f} TB", flush=True)
print(f"  Output: {OUTPUT_TRACE}", flush=True)
