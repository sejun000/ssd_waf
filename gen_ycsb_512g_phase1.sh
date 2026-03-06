#!/bin/bash
set -e

DB_DIR=/mnt/brd0/rocksdb
NUM_RECORDS=512000000
KEY_SIZE=16
VALUE_SIZE=1024

echo "=== Phase 1: Loading DB with ${NUM_RECORDS} records (~512GB) ==="
echo "Start: $(date)"

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
echo "End: $(date)"
du -sh ${DB_DIR}
df -h /mnt/brd0
