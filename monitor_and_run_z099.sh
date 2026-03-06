#!/bin/bash
# z07 끝나면 자동으로 z099 실행

LOG=/home/sejun000/ssd_waf/run_ycsb_z07.log

echo "Waiting for z07 to finish..."
while true; do
    if grep -q "All Done" ${LOG} 2>/dev/null; then
        echo "z07 completed at $(date)"
        break
    fi
    sleep 60
done

echo "Starting z099..."
nohup /home/sejun000/ssd_waf/run_ycsb_z099.sh > /home/sejun000/ssd_waf/run_ycsb_z099.log 2>&1 &
echo "z099 started with PID: $!"
