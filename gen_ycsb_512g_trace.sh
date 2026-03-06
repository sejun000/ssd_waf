#!/bin/bash
set -e

DB_DIR=/mnt/brd0/rocksdb
TRACE_DIR=/mnt/ramdisk
DEVICE=/dev/ram0
BLKTRACE_DIR=/tmp/ycsb_blktrace

# Parameters
# ~512M records * 1KB value = ~512GB DB
NUM_RECORDS=512000000
KEY_SIZE=16
VALUE_SIZE=1024
NUM_OPS=2000000000

CONVERT_TRACE() {
    local input_dir=$1
    local output_file=$2

    echo "Parsing blktrace binary..."
    blkparse -i ${input_dir}/trace -f '%a %d %T %t %S %n\n' -q -o ${input_dir}/parsed.txt 2>&1 | tail -5

    echo "Converting to trace format -> ${output_file}"
    python3 - <<PYEOF
import sys

input_file = "${input_dir}/parsed.txt"
output_file = "${output_file}"

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

        if action_code != 'D':
            continue

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
    echo "Trace saved:"
    ls -lh ${output_file}
    wc -l ${output_file}
}

# ============================================================
# Phase 1: Load DB (512GB)
# ============================================================
echo "=== Phase 1: Loading DB with ${NUM_RECORDS} records (~512GB) ==="
date

# Clean old DB
rm -rf ${DB_DIR}
mkdir -p ${DB_DIR}

db_bench \
  --db=${DB_DIR} \
  --benchmarks=fillrandom \
  --num=${NUM_RECORDS} \
  --key_size=${KEY_SIZE} \
  --value_size=${VALUE_SIZE} \
  --compression_type=none \
  --threads=32 \
  --batch_size=1000 \
  --open_files=-1 \
  --disable_wal=true \
  --target_file_size_base=$((256*1024*1024)) \
  --write_buffer_size=$((256*1024*1024)) \
  --max_write_buffer_number=8 \
  --max_background_compactions=16 \
  --max_background_flushes=8 \
  --level0_file_num_compaction_trigger=8 \
  --level0_slowdown_writes_trigger=20 \
  --level0_stop_writes_trigger=36 \
  2>&1

echo "=== Phase 1 Complete ==="
date
du -sh ${DB_DIR}
df -h /mnt/brd0

# ============================================================
# Phase 2: YCSB-A (50% read, 50% update, Zipfian)
# ============================================================
echo "=== Phase 2: YCSB-A workload ==="
date

rm -rf ${BLKTRACE_DIR}
mkdir -p ${BLKTRACE_DIR}

# Reset block stats
echo 0 > /sys/block/ram0/stat 2>/dev/null || true

blktrace -d ${DEVICE} -D ${BLKTRACE_DIR} -o trace &
BLKTRACE_PID=$!
sleep 2

db_bench \
  --db=${DB_DIR} \
  --benchmarks=mixgraph \
  --use_existing_db=true \
  --key_size=${KEY_SIZE} \
  --value_size=${VALUE_SIZE} \
  --compression_type=none \
  --threads=16 \
  --mix_get_ratio=0.5 \
  --mix_put_ratio=0.5 \
  --mix_seek_ratio=0.0 \
  --mix_accesses=${NUM_OPS} \
  --key_dist_a=0.002312 \
  --key_dist_b=-0.9 \
  --disable_wal=true \
  --open_files=-1 \
  --target_file_size_base=$((256*1024*1024)) \
  --write_buffer_size=$((256*1024*1024)) \
  --max_write_buffer_number=8 \
  --max_background_compactions=16 \
  --max_background_flushes=8 \
  --num=${NUM_RECORDS} \
  2>&1 || true

echo "=== YCSB-A Complete ==="
date

kill ${BLKTRACE_PID} 2>/dev/null || true
wait ${BLKTRACE_PID} 2>/dev/null || true
sleep 2

CONVERT_TRACE ${BLKTRACE_DIR} ${TRACE_DIR}/ycsb_a_512g_rocksdb.trace

# ============================================================
# Phase 3: YCSB-F (50% read, 50% read-modify-write, Zipfian)
# ============================================================
echo "=== Phase 3: YCSB-F workload ==="
date

rm -rf ${BLKTRACE_DIR}
mkdir -p ${BLKTRACE_DIR}

blktrace -d ${DEVICE} -D ${BLKTRACE_DIR} -o trace &
BLKTRACE_PID=$!
sleep 2

# YCSB-F: read-modify-write = readwhilewriting approximation
# mix_get_ratio=0.5, mix_put_ratio=0.5 (same keys pattern)
# db_bench doesn't have explicit RMW, but at block level it's equivalent
db_bench \
  --db=${DB_DIR} \
  --benchmarks=mixgraph \
  --use_existing_db=true \
  --key_size=${KEY_SIZE} \
  --value_size=${VALUE_SIZE} \
  --compression_type=none \
  --threads=16 \
  --mix_get_ratio=0.5 \
  --mix_put_ratio=0.5 \
  --mix_seek_ratio=0.0 \
  --mix_accesses=${NUM_OPS} \
  --key_dist_a=0.002312 \
  --key_dist_b=0.0 \
  --disable_wal=true \
  --open_files=-1 \
  --target_file_size_base=$((256*1024*1024)) \
  --write_buffer_size=$((256*1024*1024)) \
  --max_write_buffer_number=8 \
  --max_background_compactions=16 \
  --max_background_flushes=8 \
  --num=${NUM_RECORDS} \
  2>&1 || true

echo "=== YCSB-F Complete ==="
date

kill ${BLKTRACE_PID} 2>/dev/null || true
wait ${BLKTRACE_PID} 2>/dev/null || true
sleep 2

CONVERT_TRACE ${BLKTRACE_DIR} ${TRACE_DIR}/ycsb_f_512g_rocksdb.trace

echo "=== All Done ==="
date
ls -lh ${TRACE_DIR}/ycsb_*_512g_rocksdb.trace
