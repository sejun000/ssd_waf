#!/bin/bash
# Monitor blktrace output and db_bench progress
# When estimated block writes reach ~8TB, kill db_bench

BLKTRACE_DIR=/tmp/ycsb_blktrace
LOG=/home/sejun000/ssd_waf/monitor_ycsb_trace.log

echo "=== YCSB Trace Monitor Started ===" | tee -a $LOG
date | tee -a $LOG

while true; do
    # Check if db_bench is running
    if ! pgrep -f "db_bench.*mixgraph" > /dev/null 2>&1; then
        # Check if still in load phase
        if pgrep -f "db_bench.*fillrandom" > /dev/null 2>&1; then
            DB_SIZE=$(du -sh /mnt/brd0/rocksdb 2>/dev/null | awk '{print $1}')
            echo "[$(date '+%H:%M:%S')] Phase 1 (load): DB size = ${DB_SIZE}" | tee -a $LOG
            sleep 30
            continue
        fi
        echo "[$(date '+%H:%M:%S')] db_bench not running. Exiting monitor." | tee -a $LOG
        break
    fi

    # Check blktrace files size (rough estimate of captured I/O volume)
    BT_SIZE=$(du -sb ${BLKTRACE_DIR}/ 2>/dev/null | tail -1 | awk '{print $1}')
    BT_SIZE_GB=$(echo "scale=2; ${BT_SIZE:-0} / 1024/1024/1024" | bc 2>/dev/null)

    # DB size on brd
    DB_SIZE=$(du -sh /mnt/brd0/rocksdb 2>/dev/null | awk '{print $1}')

    # Estimate total block I/O from blktrace size
    # Each blktrace event is ~48 bytes. Each event ~= one I/O op
    # Average I/O size for RocksDB is ~4KB-256KB, let's estimate ~32KB avg
    # So total_block_io ≈ (bt_size / 48) * 32768
    if [ -n "${BT_SIZE}" ] && [ "${BT_SIZE}" -gt 0 ] 2>/dev/null; then
        NUM_EVENTS=$((BT_SIZE / 48))
        # More accurate: parse a sample to get actual avg I/O size
        EST_BLOCK_IO_TB=$(echo "scale=2; ${NUM_EVENTS} * 32768 / 1024/1024/1024/1024" | bc 2>/dev/null)
    else
        EST_BLOCK_IO_TB="0"
    fi

    echo "[$(date '+%H:%M:%S')] Phase 3 running | blktrace=${BT_SIZE_GB}GB | DB=${DB_SIZE} | est_block_io~${EST_BLOCK_IO_TB}TB" | tee -a $LOG

    # If blktrace file gets very large, we probably have enough data
    # 8TB of I/O with avg 32KB = ~250M events * 48 bytes = ~12GB blktrace
    # Be conservative: if blktrace > 8GB, start checking more carefully
    if [ -n "${BT_SIZE}" ] && [ "${BT_SIZE}" -gt 24000000000 ] 2>/dev/null; then
        echo "[$(date '+%H:%M:%S')] blktrace > 24GB. Likely reached ~16TB block writes." | tee -a $LOG
        echo "[$(date '+%H:%M:%S')] Stopping db_bench..." | tee -a $LOG
        pkill -f "db_bench.*mixgraph" || true
        sleep 5
        echo "[$(date '+%H:%M:%S')] db_bench stopped." | tee -a $LOG
        break
    fi

    sleep 60
done

echo "=== Monitor Complete ===" | tee -a $LOG
date | tee -a $LOG
