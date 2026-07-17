#!/usr/bin/env python3
"""
Filter DWPD>=2 volumes from tencent trace, duplicate 6x with LBA offsets.
Tencent format: Timestamp,Offset(sectors),Size(sectors),IOType,VolumeID
Output as csv format: 0,op,lba_bytes,size_bytes,timestamp
Limited to 8TB writes.
"""
import sys
import os
import csv

TRACE_DIR = "/mnt/nvme0/alibaba_block_traces_2018/cbs_trace1/atc_2020_trace/trace_ori"
OUTPUT = "/mnt/ramdisk/tencent_dwpd2_6x.trace"

# Load DWPD>=2 volume IDs
DWPD2_VOLS = set()
with open('/mnt/nvme0/write_volume_result_tencent.csv') as f:
    reader = csv.DictReader(f)
    for row in reader:
        if float(row['DWPD']) >= 2.0:
            DWPD2_VOLS.add(int(row['VolumeID']))

# Total device size of DWPD>=2: 8023 GB
OFFSET = int(8023 * 1024 * 1024 * 1024)  # bytes
OFFSET_SECTORS = OFFSET // 512

N_COPIES = 6
WRITE_LIMIT = 8 * 1000 * 1000 * 1000 * 1000  # 8TB

print(f"Filtering {len(DWPD2_VOLS)} volumes with DWPD>=2")
print(f"LBA offset per copy: {OFFSET_SECTORS} sectors ({OFFSET/1e9:.0f} GB)")
print(f"Copies: {N_COPIES}, Write limit: {WRITE_LIMIT/1e12:.0f} TB")
sys.stdout.flush()

# Get sorted file list
files = sorted([f for f in os.listdir(TRACE_DIR) if not f.startswith('.')])
print(f"Found {len(files)} trace files")
sys.stdout.flush()

written_bytes = 0
line_count = 0
next_print = 100 * 1000 * 1000 * 1000

with open(OUTPUT, 'w') as fout:
    for fname in files:
        fpath = os.path.join(TRACE_DIR, fname)
        print(f"  reading {fname}...")
        sys.stdout.flush()
        with open(fpath) as fin:
            for line in fin:
                parts = line.strip().split(',')
                if len(parts) < 5:
                    continue

                vol_id = int(parts[4])
                if vol_id not in DWPD2_VOLS:
                    continue

                ts = parts[0]
                offset_sec = int(parts[1])
                size_sec = int(parts[2])
                io_type = int(parts[3])

                op = "W" if io_type == 1 else "R"
                size_bytes = size_sec * 512

                for i in range(N_COPIES):
                    new_offset_sec = offset_sec + i * OFFSET_SECTORS
                    lba_bytes = new_offset_sec * 512
                    fout.write(f"0,{op},{lba_bytes},{size_bytes},{ts}\n")
                    line_count += 1

                    if io_type == 1:
                        written_bytes += size_bytes

                if written_bytes >= next_print:
                    print(f"Written: {written_bytes/1e12:.2f} TB, lines: {line_count/1e6:.1f}M")
                    sys.stdout.flush()
                    next_print += 100 * 1000 * 1000 * 1000

                if written_bytes >= WRITE_LIMIT:
                    break

        if written_bytes >= WRITE_LIMIT:
            print(f"Reached {WRITE_LIMIT/1e12:.0f} TB write limit.")
            break

print(f"Done. Lines: {line_count}, Writes: {written_bytes/1e12:.2f} TB")
print(f"Output: {OUTPUT}")
