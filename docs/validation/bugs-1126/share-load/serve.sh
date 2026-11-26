#!/bin/sh
# Starts smbd in a cgroup of its own, so its disk and CPU can be made slow.
#   serve.sh fast            no limits
#   serve.sh hdd IOPS        reads from the disk at most IOPS a second (a hard disk: ~100)
#   serve.sh cpu PERCENT     that share of one core (a small ARM NAS)
set -e
mode=${1:-fast}
value=${2:-0}
pkill smbd 2>/dev/null || true
sleep 1
cg=/sys/fs/cgroup/nas
mkdir -p "$cg"
echo "+io +cpu" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
dev=$(lsblk -no MAJ:MIN "$(findmnt -no SOURCE /)" | tr -d ' ')
echo "$dev riops=max" > "$cg/io.max" 2>/dev/null || true
echo "max 100000" > "$cg/cpu.max" 2>/dev/null || true
case "$mode" in
    hdd) echo "$dev riops=$value" > "$cg/io.max" ;;
    cpu) echo "$((value * 100)) 10000" > "$cg/cpu.max" ;;
esac
sh -c "echo \$\$ > $cg/cgroup.procs; exec smbd --foreground --no-process-group" >/var/log/smbd.out 2>&1 &
sleep 2
echo "mode=$mode value=$value dev=$dev io.max=$(cat "$cg/io.max" 2>/dev/null) cpu.max=$(cat "$cg/cpu.max")"
pgrep -a smbd | head -3
