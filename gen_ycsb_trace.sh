#!/bin/bash
set -e

DB_DIR=/mnt/brd0/rocksdb
TRACE_DIR=/mnt/ramdisk
DEVICE=/dev/ram0
BLKTRACE_DIR=/tmp/ycsb_blktrace
OUTPUT_TRACE=${TRACE_DIR}/ycsb_a_rocksdb.trace

# Parameters
# ~50M records * 1KB value = ~50GB DB
NUM_RECORDS=50000000
KEY_SIZE=16
VALUE_SIZE=1024
# Large enough ops to generate 8TB block writes
NUM_OPS=2000000000

echo "=== Phase 1: Loading DB with ${NUM_RECORDS} records ==="
date
db_bench \
  --db=${DB_DIR} \
  --benchmarks=fillrandom \
  --num=${NUM_RECORDS} \
  --key_size=${KEY_SIZE} \
  --value_size=${VALUE_SIZE} \
  --compression_type=none \
  --threads=8 \
  --batch_size=1000 \
  --open_files=-1 \
  --target_file_size_base=$((256*1024*1024)) \
  --write_buffer_size=$((128*1024*1024)) \
  --max_write_buffer_number=4 \
  2>&1

echo "=== Phase 1 Complete ==="
date
du -sh ${DB_DIR}
df -h /mnt/brd0

echo "=== Phase 2: Starting blktrace ==="
rm -rf ${BLKTRACE_DIR}
mkdir -p ${BLKTRACE_DIR}

# Start blktrace in background
# -D: output directory, -o: output file prefix
blktrace -d ${DEVICE} -D ${BLKTRACE_DIR} -o trace &
BLKTRACE_PID=$!
echo "blktrace PID: ${BLKTRACE_PID}"
sleep 2

echo "=== Phase 3: Running YCSB-A workload (mixgraph, Zipfian 0.99, 50/50 R/W) ==="
date

db_bench \
  --db=${DB_DIR} \
  --benchmarks=mixgraph \
  --use_existing_db=true \
  --key_size=${KEY_SIZE} \
  --value_size=${VALUE_SIZE} \
  --compression_type=none \
  --threads=8 \
  --mix_get_ratio=0.5 \
  --mix_put_ratio=0.5 \
  --mix_seek_ratio=0.0 \
  --mix_accesses=${NUM_OPS} \
  --key_dist_a=0.002312 \
  --key_dist_b=-0.9 \
  --open_files=-1 \
  --target_file_size_base=$((256*1024*1024)) \
  --write_buffer_size=$((128*1024*1024)) \
  --max_write_buffer_number=4 \
  --num=${NUM_RECORDS} \
  2>&1 || true

echo "=== Phase 3 Complete ==="
date

# Final block write stats
WRITE_SECTORS=$(awk '{print $7}' /sys/block/ram0/stat)
WRITE_BYTES=$((WRITE_SECTORS * 512))
echo "Total block writes: $((WRITE_BYTES / 1024 / 1024 / 1024)) GB"

# Stop blktrace
kill ${BLKTRACE_PID} 2>/dev/null || true
wait ${BLKTRACE_PID} 2>/dev/null || true
sleep 2

echo "=== Phase 4: Converting blktrace to alibaba format ==="
echo "Parsing blktrace binary..."

# Use blkparse with custom format:
# %d = direction (R/W/N/etc with S suffix for sync)
# %T = timestamp seconds, %t = timestamp nanoseconds
# %S = sector, %n = number of sectors
# Filter only completed I/Os (action C) or dispatched (D)
blkparse -i ${BLKTRACE_DIR}/trace -f '%a %d %T %t %S %n\n' -q -o ${BLKTRACE_DIR}/parsed.txt 2>&1 | tail -5

echo "Converting to alibaba trace format..."

python3 - <<'PYEOF'
import sys

input_file = "/tmp/ycsb_blktrace/parsed.txt"
output_file = "/mnt/ramdisk/ycsb_a_rocksdb.trace"

print(f"Converting {input_file} -> {output_file}")

count = 0
skipped = 0
with open(input_file, 'r') as fin, open(output_file, 'w') as fout:
    for line in fin:
        parts = line.split()
        if len(parts) < 6:
            continue

        action_code = parts[0]
        direction_raw = parts[1]
        sec_str = parts[2]
        nsec_str = parts[3]
        sector_str = parts[4]
        nsectors_str = parts[5]

        # Only dispatch events (D) - one per actual I/O
        if action_code != 'D':
            continue

        # Parse direction: WS=write sync, W=write, RS=read sync, R=read, etc.
        if direction_raw.startswith('W'):
            direction = 'W'
        elif direction_raw.startswith('R'):
            direction = 'R'
        else:
            skipped += 1
            continue

        try:
            offset_bytes = int(sector_str) * 512
            size_bytes = int(nsectors_str) * 512
            if size_bytes == 0:
                skipped += 1
                continue
            timestamp_ns = int(sec_str) * 1000000000 + int(nsec_str)
        except ValueError:
            skipped += 1
            continue

        fout.write(f"0,{direction},{offset_bytes},{size_bytes},{timestamp_ns}\n")
        count += 1

        if count % 10000000 == 0:
            print(f"  {count // 1000000}M records written...")

print(f"Done. {count} I/O records written, {skipped} skipped")
PYEOF

echo "=== Done ==="
ls -lh ${OUTPUT_TRACE}
wc -l ${OUTPUT_TRACE}
echo "First 10 lines:"
head -10 ${OUTPUT_TRACE}
echo "Last 10 lines:"
tail -10 ${OUTPUT_TRACE}
date
