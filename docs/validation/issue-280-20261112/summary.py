"""The probe's reports by window, kind of opening and build.

    python summary.py data final-normal [0.3]

reads data/final-normal-<build>-<n>.json (idle-open.json of
`WinShun.exe --probe-runtime <folder> --idle-open-check`, see src/app/IdleOpenProbe.cpp).
Openings during which the whole machine was busier than the limit (0.3: 30 % of
all processors) are left out and counted under `busy`.
"""
import collections
import glob
import json
import os
import statistics as st
import sys

root, tag = sys.argv[1], sys.argv[2]
busy_limit = float(sys.argv[3]) if len(sys.argv) > 3 else 0.3
groups = collections.defaultdict(list)
for path in sorted(glob.glob(os.path.join(root, tag + '-*.json'))):
    build = os.path.basename(path)[len(tag) + 1:].rsplit('-', 1)[0]
    with open(path, encoding='utf-8') as file:
        report = json.load(file)
    for row in report['openings']:
        kind = row['kind']
        if kind == 'idle':
            kind = 'idle, given back' if row.get('trimmed') else 'idle, nothing to give'
            if row.get('pressed'):
                kind += ', short of memory'
        groups[(row['window'], kind, build)].append(row)
print('%-9s %-34s %-8s %3s %4s | %6s %6s %6s | %7s | %9s' % (
    'window', 'kind', 'build', 'n', 'busy', 'median', 'min', 'max', 'faults', 'MB before'))
for key in sorted(groups):
    rows = groups[key]
    quiet = [r for r in rows if r['cpu'] <= busy_limit]
    if not quiet:
        continue
    seen = sorted(r['seen_ms'] for r in quiet)
    print('%-9s %-34s %-8s %3d %4d | %6.1f %6.1f %6.1f | %7d | %9.1f' % (
        key[0], key[1], key[2], len(seen), len(rows) - len(quiet), st.median(seen), seen[0], seen[-1],
        st.median(r['faults'] for r in quiet), st.median(r['private_mb_before'] for r in quiet)))
