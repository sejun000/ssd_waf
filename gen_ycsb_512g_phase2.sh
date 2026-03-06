#!/bin/bash
set -e

DB_DIR=/mnt/brd0/rocksdb
TRACE_DIR=/mnt/ramdisk
DEVICE=/dev/ram0
BLKTRACE_DIR=/tmp/ycsb_blktrace

NUM_RECORDS=512000000
KEY_SIZE=16
VALUE_SIZE=1024
NUM_OPS=75000000000

WORKLOAD=${1:-A}  # A or F, default A

CONVERT_TRACE() {
    local input_dir=$1
    local output_file=$2

    echo "Parsing blktrace binary..."
    blkparse -i ${input_dir}/trace -f '%a %d %T %t %S %n\n' -q -o ${input_dir}/parsed.txt 2>&1 | tail -5

    echo "Converting to trace format -> ${output_file}"
    python3 - "${input_dir}/parsed.txt" "${output_file}" <<'PYEOF'
import sys

input_file = sys.argv[1]
output_file = sys.argv[2]

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

        if action_code != 'Q':
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

if [ "$WORKLOAD" = "A" ]; then
    DIST_A=0.002312
    DIST_B=-0.9
    OUTPUT=${TRACE_DIR}/ycsb_a_512g_rocksdb.trace
    echo "=== YCSB-A (50R/50W, Zipfian ~0.99) ==="
elif [ "$WORKLOAD" = "F" ]; then
    DIST_A=0.002312
    DIST_B=-0.7
    OUTPUT=${TRACE_DIR}/ycsb_f_512g_rocksdb.trace
    echo "=== YCSB-F (50R/50RMW, Zipfian ~0.7) ==="
else
    echo "Usage: $0 [A|F]"
    exit 1
fi

echo "Start: $(date)"

rm -rf ${BLKTRACE_DIR}
mkdir -p ${BLKTRACE_DIR}

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
  --key_dist_a=${DIST_A} \
  --key_dist_b=${DIST_B} \
  --disable_wal=true \
  --open_files=-1 \
  --target_file_size_base=$((256*1024*1024)) \
  --write_buffer_size=$((4*1024*1024)) \
  --max_write_buffer_number=2 \
  --use_direct_reads=true \
  --use_direct_io_for_flush_and_compaction=true \
  --max_background_compactions=16 \
  --max_background_flushes=8 \
  --num=${NUM_RECORDS} \
  2>&1 || true

echo "=== Workload Complete ==="
echo "End: $(date)"

kill ${BLKTRACE_PID} 2>/dev/null || true
wait ${BLKTRACE_PID} 2>/dev/null || true
sleep 2

CONVERT_TRACE ${BLKTRACE_DIR} ${OUTPUT}
