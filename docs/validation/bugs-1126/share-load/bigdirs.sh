#!/bin/sh
# Ten folders of 5000 files on the slow disk: one request for 64 KB of names
# reads many of them at once.
set -e
cd /srv/slow
if [ ! -d archive ]; then
    for a in $(seq -w 1 10); do
        mkdir -p "archive/year$a"
        cd "archive/year$a"
        for f in $(seq -w 1 5000); do : > "scan-$a-$f.pdf"; done
        cd ../..
    done
    sync
fi
find /srv/slow | wc -l
