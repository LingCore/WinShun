#!/bin/sh
# Throwaway Samba server for the share-load measurement (build/nas-load).
set -e
smbd --version
adduser -D -H nas 2>/dev/null || true
printf 'Ws-nas-2611\nWs-nas-2611\n' | smbpasswd -s -a nas >/dev/null
echo "user ok"
# A tree like a small NAS: 20 x 60 folders of 100 files, 120k files, 1220 folders.
if [ ! -f /srv/tree/.made ]; then
    mkdir -p /srv/tree
    cd /srv/tree
    for a in $(seq -w 1 20); do
        for b in $(seq -w 1 60); do
            mkdir -p "dept$a/project$b"
            cd "dept$a/project$b"
            for f in $(seq -w 1 100); do : > "report-$a-$b-$f.docx"; done
            cd ../..
        done
    done
    # What the "file manager" opens: folders the indexer's pass also reads.
    : > /srv/tree/.made
fi
find /srv/tree | wc -l
lsblk -o NAME,MAJ:MIN,SIZE,MOUNTPOINT
mount | grep ' / '
