#!/bin/bash
set -e

# Disable ASLR (filebench 1.5-alpha3 segfault workaround)
echo 0 > /proc/sys/kernel/randomize_va_space

# Find the non-root NVMe device automatically
ROOT_DEV=$(lsblk -ndo PKNAME $(findmnt -n -o SOURCE /))
DEVICE=$(lsblk -ndo NAME -I 259 | grep -v "^${ROOT_DEV}" | grep -v "p[0-9]" | head -1)
if [ -z "$DEVICE" ]; then
    echo "ERROR: non-root NVMe device not found"
    exit 1
fi
echo "Root device: $ROOT_DEV, Using device: $DEVICE"

echo "Using /dev/$DEVICE for blktrace+filebench"

# Mount if not already mounted
MOUNTPOINT=$(lsblk -no MOUNTPOINT /dev/$DEVICE | head -1)
if [ -z "$MOUNTPOINT" ]; then
    MOUNTPOINT=/mnt/nvme_data
    mkdir -p $MOUNTPOINT
    mount /dev/$DEVICE $MOUNTPOINT
    echo "Mounted /dev/$DEVICE at $MOUNTPOINT"
fi

OUTDIR=/home/sejun000/ssd_waf/varmail_2tb_bt
rm -f $OUTDIR/trace.blktrace.* $OUTDIR/trace.bin
mkdir -p $OUTDIR

# Kill any leftover blktrace on this device
killall -q blktrace 2>/dev/null || true
sleep 1

echo "Starting blktrace on $DEVICE..."
blktrace -d /dev/$DEVICE -D $OUTDIR -o trace &
BT_PID=$!
sleep 3
if ! kill -0 $BT_PID 2>/dev/null; then
    echo "ERROR: blktrace failed to start"
    exit 1
fi
echo "blktrace running (PID $BT_PID)"

# Generate filebench config with correct mount path
FBCONF=/tmp/varmail_2tb_run.f
sed "s|^set \$dir=.*|set \$dir=$MOUNTPOINT|" /home/sejun000/ssd_waf/varmail_500g_2tb.f > $FBCONF

echo "Starting filebench varmail (2200s) on $MOUNTPOINT..."
filebench -f $FBCONF 2>&1 | tee /home/sejun000/ssd_waf/varmail_2tb_filebench.log

echo "Stopping blktrace..."
kill $BT_PID
wait $BT_PID 2>/dev/null || true

echo "Converting to binary..."
blkparse -i $OUTDIR/trace -D $OUTDIR -d $OUTDIR/trace.bin -q > /dev/null
echo "Done."
