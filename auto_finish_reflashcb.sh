#!/bin/bash
# Detached supervisor: waits for the REFlash-CB (sqrt) ali2 sim to finish, then
# repoints the REFlash-CB entries in plot_age_stddev.py and regenerates the PDF.
# Survives session close (launch with: setsid nohup ... &).
cd /home/sejun000/ssd_waf || exit 1

THRESH=${1:-0}
CB_PID=${2:-0}
DEADLINE=$(( $(date +%s) + 6*3600 ))   # 6h safety cap

echo "[cb_finish] supervisor start pid=$$ at $(date); waiting CB=$CB_PID thresh=$THRESH"

while kill -0 "$CB_PID" 2>/dev/null; do
    if [ "$(date +%s)" -ge "$DEADLINE" ]; then
        echo "[cb_finish] DEADLINE hit, proceeding anyway at $(date)"
        break
    fi
    sleep 30
done

echo "[cb_finish] CB sim no longer running at $(date); flushing 5s"
sleep 5
python3 auto_finish_reflashcb.py "$THRESH"
echo "[cb_finish] supervisor done at $(date) (exit=$?)"
