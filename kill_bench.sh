#!/bin/bash
# Kill all filebench/blktrace/run_varmail related processes

echo "Finding processes..."
pids=$(ps aux | grep -E "filebench|filereader|blktrace|run_varmail" | grep -v grep | awk '{print $2}')

if [ -z "$pids" ]; then
    echo "No benchmark processes found."
    exit 0
fi

ps aux | grep -E "filebench|filereader|blktrace|run_varmail" | grep -v grep
echo ""
read -p "Kill these processes? [y/N] " confirm
if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
    echo "Aborted."
    exit 1
fi

echo "Killing processes..."
echo "$pids" | xargs kill -9 2>/dev/null
sleep 1

# Verify
remaining=$(ps aux | grep -E "filebench|filereader|blktrace|run_varmail" | grep -v grep)
if [ -z "$remaining" ]; then
    echo "All processes killed."
else
    echo "WARNING: some processes still alive:"
    echo "$remaining"
fi
