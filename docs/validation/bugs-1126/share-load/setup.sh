#!/bin/sh
# Throwaway Samba server for the share-load measurement (build/nas-load).
#   wsl --import wsnas <dir> alpine-minirootfs-3.22.x-x86_64.tar.gz --version 2
#   wsl -d wsnas -e sleep 86400      (kept running: the VM stops without a process)
#   wsl -d wsnas -e sh setup.sh ; wsl -d wsnas -e sh serve.sh fast
#   net use \\<the distro's eth0 address>\tree /user:nas Ws-nas-2611
set -e
apk add --no-cache samba samba-common-tools kmod util-linux coreutils device-mapper e2fsprogs iproute2-tc >/dev/null
smbd --version
cat > /etc/samba/smb.conf <<'EOF'
[global]
   workgroup = WORKGROUP
   server string = wsnas
   security = user
   map to guest = never
   load printers = no
   disable spoolss = yes
[tree]
   path = /srv/tree
   read only = no
   valid users = nas
   force user = root
EOF
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
