#!/bin/bash
# Detached supervisor: waits for the two ali2 sims to finish, then patches
# plot_age_stddev.py and regenerates A_age_stddev_cdf.pdf. Survives session
# close (launch with: setsid nohup ... &).
cd /home/sejun000/ssd_waf || exit 1

THRESH=${1:-0}
REFLASH_PID=${2:-659577}
SEPBIT_PID=${3:-659600}
DEADLINE=$(( $(date +%s) + 6*3600 ))   # 6h safety cap

echo "[auto_finish] supervisor start pid=$$ at $(date)"
echo "[auto_finish] waiting on REFLASH=$REFLASH_PID SEPBIT=$SEPBIT_PID thresh=$THRESH"

while kill -0 "$REFLASH_PID" 2>/dev/null || kill -0 "$SEPBIT_PID" 2>/dev/null; do
    if [ "$(date +%s)" -ge "$DEADLINE" ]; then
        echo "[auto_finish] DEADLINE hit, proceeding anyway at $(date)"
        break
    fi
    sleep 30
done

echo "[auto_finish] sims no longer running at $(date); flushing 5s"
sleep 5
python3 auto_finish_age_stddev.py "$THRESH"
echo "[auto_finish] supervisor done at $(date) (exit=$?)"
