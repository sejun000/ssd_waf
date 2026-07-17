#!/usr/bin/env python3
"""
Duplicate ssdtrace (blktrace) 16x with LBA offsets.
Assumes ~512GB device, so offset = 512GB per copy.
Output limited to 8TB writes. Only Q events are output.
"""
import sys

TRACE = "/home/sejun000/ssdtrace/rawfile"
OUTPUT = "/mnt/ramdisk/ssdtrace_16x.trace"

OFFSET_SECTORS = 512 * 1024 * 1024 * 1024 // 512  # 512GB in 512B sectors
N_COPIES = 16
WRITE_LIMIT = 12 * 1000 * 1000 * 1000 * 1000  # 12TB

written_bytes = 0
line_count = 0
next_print = 100 * 1000 * 1000 * 1000

print(f"LBA offset per copy: {OFFSET_SECTORS} sectors ({OFFSET_SECTORS*512/1e9:.0f} GB)")
print(f"Copies: {N_COPIES}, Write limit: {WRITE_LIMIT/1e12:.0f} TB")
sys.stdout.flush()

with open(TRACE) as fin, open(OUTPUT, 'w') as fout:
    for line in fin:
        parts = line.split()
        if len(parts) < 10:
            continue
        # only Q events
        if parts[5] != 'Q':
            continue

        op = parts[6]  # R, W, WS, RS, etc.
        try:
            lba_sector = int(parts[7])
            size_sector = int(parts[9])
        except:
            continue

        for i in range(N_COPIES):
            new_lba = lba_sector + i * OFFSET_SECTORS
            # output as csv format: 0,op,lba_bytes,size_bytes,timestamp
            lba_bytes = new_lba * 512
            size_bytes = size_sector * 512
            ts = parts[3]
            fout.write(f"0,{op},{lba_bytes},{size_bytes},{ts}\n")
            line_count += 1

            if op == 'W' or op == 'WS':
                written_bytes += size_bytes

        if written_bytes >= next_print:
            print(f"Written: {written_bytes/1e12:.2f} TB, lines: {line_count/1e6:.1f}M")
            sys.stdout.flush()
            next_print += 100 * 1000 * 1000 * 1000

        if written_bytes >= WRITE_LIMIT:
            print(f"Reached {WRITE_LIMIT/1e12:.0f} TB write limit.")
            break

print(f"Done. Lines: {line_count}, Writes: {written_bytes/1e12:.2f} TB")
print(f"Output: {OUTPUT}")
