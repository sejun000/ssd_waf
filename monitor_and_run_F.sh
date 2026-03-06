#!/bin/bash
# Monitor YCSB-A, when done: convert trace + backup raw, then run F

BLKTRACE_DIR=/tmp/ycsb_blktrace
BACKUP_DIR=/home/sejun000/ssd_waf/blktrace_backup
TRACE_DIR=/mnt/ramdisk
PHASE2_SCRIPT=/home/sejun000/ssd_waf/gen_ycsb_512g_phase2.sh
A_LOG=/home/sejun000/ssd_waf/gen_ycsb_512g_phase2_A.log
F_LOG=/home/sejun000/ssd_waf/gen_ycsb_512g_phase2_F.log

mkdir -p ${BACKUP_DIR}

echo "=== Monitoring YCSB-A ==="
echo "Start: $(date)"

# Wait for A to finish (check if db_bench is still running)
while true; do
    if ! pgrep -f "db_bench.*mixgraph" > /dev/null 2>&1; then
        echo "$(date): db_bench finished"
        break
    fi

    BLKTRACE_SIZE=$(du -sm ${BLKTRACE_DIR} 2>/dev/null | awk '{print $1}')
    OPS=$(grep -oP 'finished \K[0-9]+' ${A_LOG} 2>/dev/null | tail -1)
    echo "$(date): blktrace=${BLKTRACE_SIZE}MB, ops=${OPS:-0}"

    sleep 120
done

# Wait for blktrace to flush
sleep 10

echo "$(date): === Converting A trace ==="
# Parse blktrace binary
blkparse -i ${BLKTRACE_DIR}/trace -f '%a %d %T %t %S %n\n' -q -o ${BLKTRACE_DIR}/parsed.txt 2>&1 | tail -5

# Convert to trace format
python3 - "${BLKTRACE_DIR}/parsed.txt" "${TRACE_DIR}/ycsb_a_512g_rocksdb.trace" <<'PYEOF'
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

echo "$(date): A trace saved:"
ls -lh ${TRACE_DIR}/ycsb_a_512g_rocksdb.trace
wc -l ${TRACE_DIR}/ycsb_a_512g_rocksdb.trace

# Backup A raw blktrace + parsed
echo "$(date): === Backing up A raw files ==="
cp -r ${BLKTRACE_DIR} ${BACKUP_DIR}/ycsb_a_blktrace
echo "$(date): Backup done:"
du -sh ${BACKUP_DIR}/ycsb_a_blktrace

# Now run F
echo "$(date): === Starting F workload ==="
bash ${PHASE2_SCRIPT} F > ${F_LOG} 2>&1

echo "$(date): === F workload done, converting F trace ==="

# F's trace conversion is handled by the script now (bug fixed)
# But let's also backup F raw blktrace
echo "$(date): === Backing up F raw files ==="
cp -r ${BLKTRACE_DIR} ${BACKUP_DIR}/ycsb_f_blktrace
echo "$(date): Backup done:"
du -sh ${BACKUP_DIR}/ycsb_f_blktrace

echo "$(date): === ALL DONE ==="
