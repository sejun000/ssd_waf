#!/usr/bin/env python3
"""
Merge two traces (dwpd0.3 + dwpd1) into one combined trace.
- dwpd1 trace LBAs are offset by the address space size of dwpd0.3
- Merge-sort by timestamp
- Stop at 8TB total writes
- Output to /mnt/ramdisk/alibaba_merged.trace
"""
import sys
import heapq

TRACE1 = "/mnt/ramdisk/alibaba_dwpd0.3.trace"
TRACE2 = "/home/sejun000/alibaba_trace/alibaba_dwpd1.trace"
OUTPUT = "/mnt/ramdisk/alibaba_merged.trace"

# LBA offset for trace2 (will be set from command line or default)
LBA_OFFSET = int(sys.argv[1]) if len(sys.argv) > 1 else 125058710241280  # ~116TB safe default

WRITE_LIMIT = 8 * 1000 * 1000 * 1000 * 1000  # 8TB in bytes

def line_gen(path, lba_offset=0):
    """Yield (timestamp, modified_line) from trace file."""
    with open(path) as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 5:
                continue
            ts = int(parts[4])
            if lba_offset > 0:
                parts[2] = str(int(parts[2]) + lba_offset)
                yield (ts, ','.join(parts) + '\n')
            else:
                yield (ts, line)

print("Opening traces...")
gen1 = line_gen(TRACE1, lba_offset=0)
gen2 = line_gen(TRACE2, lba_offset=LBA_OFFSET)

written_bytes = 0
line_count = 0
next_print = 100 * 1000 * 1000 * 1000  # 100GB

print(f"Merging with LBA_OFFSET={LBA_OFFSET} ({LBA_OFFSET/1e12:.2f} TB)")
print(f"Write limit: {WRITE_LIMIT/1e12:.0f} TB")

with open(OUTPUT, 'w') as out:
    # Use heapq.merge for efficient sorted merge
    for ts, line in heapq.merge(gen1, gen2, key=lambda x: x[0]):
        out.write(line)
        line_count += 1

        # Count write bytes
        parts = line.strip().split(',')
        op = parts[1]
        if op == 'W' or op == 'WS':
            written_bytes += int(parts[3])

        if written_bytes >= next_print:
            print(f"Written: {written_bytes/1e12:.2f} TB, lines: {line_count/1e6:.1f}M")
            next_print += 100 * 1000 * 1000 * 1000

        if written_bytes >= WRITE_LIMIT:
            print(f"Reached {WRITE_LIMIT/1e12:.0f} TB write limit. Stopping.")
            break

print(f"Done. Total lines: {line_count}, Total writes: {written_bytes/1e12:.2f} TB")
print(f"Output: {OUTPUT}")
