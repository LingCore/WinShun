#!/bin/sh
# A disk that answers each read after MS milliseconds (dm-delay), like a hard
# disk seeking, with the same tree on it, shared as [slow].
#   slowdisk.sh MS
set -e
ms=${1:-10}
modprobe dm-delay
pkill smbd 2>/dev/null || true
sleep 1
umount /srv/slow 2>/dev/null || true
dmsetup remove slow 2>/dev/null || true
if [ ! -f /srv/slow.img ]; then
    truncate -s 2G /srv/slow.img
    mkfs.ext4 -q -F -N 400000 /srv/slow.img
    made=no
fi
loop=$(losetup -j /srv/slow.img | cut -d: -f1 | head -1)
[ -n "$loop" ] || loop=$(losetup -f --show /srv/slow.img)
sectors=$(blockdev --getsz "$loop")
# reads wait $ms, writes do not
echo "0 $sectors delay $loop 0 $ms $loop 0 0" | dmsetup create slow
mkdir -p /srv/slow
mount -o noatime /dev/mapper/slow /srv/slow
if [ ! -f /srv/slow/.made ]; then
    cd /srv/slow
    for a in $(seq -w 1 20); do
        for b in $(seq -w 1 60); do
            mkdir -p "dept$a/project$b"
            cd "dept$a/project$b"
            for f in $(seq -w 1 100); do : > "report-$a-$b-$f.docx"; done
            cd ../..
        done
    done
    : > /srv/slow/.made
    sync
fi
grep -q '^\[slow\]' /etc/samba/smb.conf || cat >> /etc/samba/smb.conf <<'EOF'
[slow]
   path = /srv/slow
   read only = no
   valid users = nas
   force user = root
EOF
echo "delay $ms ms on $loop: $(dmsetup table slow)"
find /srv/slow | wc -l
