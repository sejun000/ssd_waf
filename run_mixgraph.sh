#!/bin/bash
set -e

DB_DIR=/mnt/brd0/rocksdb
DEVICE=/dev/ram0
BLKTRACE_DIR=/tmp/ycsb_blktrace
OUTPUT_TRACE=/mnt/ramdisk/ycsb_a_rocksdb.trace

NUM_RECORDS=50000000
NUM_OPS=8000000000

echo "=== Starting blktrace on ${DEVICE} ==="
rm -rf ${BLKTRACE_DIR}
mkdir -p ${BLKTRACE_DIR}
blktrace -d ${DEVICE} -D ${BLKTRACE_DIR} -o trace &
BLKTRACE_PID=$!
echo "blktrace PID: ${BLKTRACE_PID}"
sleep 2

echo "=== Running YCSB-A mixgraph (Zipfian ~0.99, 50/50 R/W) ==="
date

db_bench \
  --db=${DB_DIR} \
  --benchmarks=mixgraph \
  --use_existing_db=true \
  --key_size=16 \
  --value_size=1024 \
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
  --write_buffer_size=$((4*1024*1024)) \
  --max_write_buffer_number=2 \
  --use_direct_reads=true \
  --use_direct_io_for_flush_and_compaction=true \
  --num=1000000000 \
  2>&1 || true

echo "=== mixgraph done ==="
date

# Stop blktrace
kill ${BLKTRACE_PID} 2>/dev/null || true
wait ${BLKTRACE_PID} 2>/dev/null || true
sleep 2

echo "=== Converting blktrace to alibaba format ==="
blkparse -i ${BLKTRACE_DIR}/trace -f '%a %d %T %t %S %n\n' -q -o ${BLKTRACE_DIR}/parsed.txt 2>&1 | tail -5

python3 - <<'PYEOF'
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

        # brd uses Q (queue) events, not D (dispatch)
        if action_code != 'Q':
            continue
        if direction_raw.startswith('W'):
            direction = 'W'
        elif direction_raw.startswith('R') and not direction_raw.startswith('RM'):
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

print(f"Done. {count} I/O records, {skipped} skipped")
PYEOF

echo "=== Done ==="
ls -lh ${OUTPUT_TRACE}
wc -l ${OUTPUT_TRACE}
head -10 ${OUTPUT_TRACE}
tail -5 ${OUTPUT_TRACE}
date
