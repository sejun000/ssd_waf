#!/bin/bash
set -e

YCSB=/opt/ycsb-0.17.0
DB_DIR=/mnt/brd0/rocksdb_ycsb
TRACE_DIR=/mnt/ramdisk
DEVICE=/dev/ram0
BLKTRACE_DIR=/tmp/ycsb_blktrace_z099
OUTPUT=${TRACE_DIR}/ycsb_a_512g_z099_rocksdb.trace

RECORD_COUNT=512000000
OP_COUNT=5000000000

echo "=== YCSB-A (50R/50W, Zipfian theta=0.99) ==="
echo "Start: $(date)"

# Run phase only (DB already loaded from z07)
echo "=== YCSB Run with blktrace ==="
rm -rf ${BLKTRACE_DIR}
mkdir -p ${BLKTRACE_DIR}

blktrace -d ${DEVICE} -D ${BLKTRACE_DIR} -o trace &
BLKTRACE_PID=$!
sleep 2

${YCSB}/bin/ycsb.sh run rocksdb -s \
  -P ${YCSB}/workloads/workloada \
  -p rocksdb.dir=${DB_DIR} \
  -p recordcount=${RECORD_COUNT} \
  -p operationcount=${OP_COUNT} \
  -p fieldcount=1 \
  -p fieldlength=1024 \
  -p requestdistribution=zipfian \
  -p zipfian.constant=0.99 \
  -threads 16 \
  2>&1 | tail -30

echo "=== Run Complete: $(date) ==="

kill ${BLKTRACE_PID} 2>/dev/null || true
wait ${BLKTRACE_PID} 2>/dev/null || true
sleep 2

# Convert blktrace -> CSV
echo "=== Converting blktrace ==="
blkparse -i ${BLKTRACE_DIR}/trace -f '%a %d %T %t %S %n\n' -q -o ${BLKTRACE_DIR}/parsed.txt 2>&1 | tail -5

python3 - ${BLKTRACE_DIR}/parsed.txt ${OUTPUT} <<'PYEOF'
import sys
input_file, output_file = sys.argv[1], sys.argv[2]
count = skipped = 0
with open(input_file) as fin, open(output_file, 'w') as fout:
    for line in fin:
        parts = line.split()
        if len(parts) < 6: continue
        if parts[0] != 'Q': continue
        d = parts[1]
        if d.startswith('W'): direction = 'W'
        elif d.startswith('R'): direction = 'R'
        else: skipped += 1; continue
        try:
            offset = int(parts[4]) * 512
            size = int(parts[5]) * 512
            if size == 0: skipped += 1; continue
            ts = int(parts[2]) * 1000000000 + int(parts[3])
        except ValueError: skipped += 1; continue
        fout.write(f"0,{direction},{offset},{size},{ts}\n")
        count += 1
        if count % 10000000 == 0: print(f"  {count//1000000}M records...")
print(f"Done. {count} records, {skipped} skipped")
PYEOF

echo "=== Trace saved ==="
ls -lh ${OUTPUT}
wc -l ${OUTPUT}
echo "=== All Done: $(date) ==="
