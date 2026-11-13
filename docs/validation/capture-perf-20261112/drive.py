"""Drives a stand-alone WinShunCapture.exe through its plugin protocol and
times the overlay: start, wait, cancel, with idle gaps in between.

  python drive.py --exe <WinShunCapture.exe> --app-dir <Win顺 with Qt> --data <dir>
                  --gaps 2,2,50,50 [--hold 0.6] [--env NAME=VALUE ...]

Every gap is the idle time (s) before a capture; the capture is cancelled
after --hold seconds. The plugin's log lines about the capture are printed
with the time they arrived.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True)
parser.add_argument('--app-dir', required=True)
parser.add_argument('--data', required=True)
parser.add_argument('--gaps', default='2,2,2')
parser.add_argument('--hold', type=float, default=0.6)
parser.add_argument('--env', action='append', default=[])
parser.add_argument('--log', default='')
parser.add_argument('--renderer', default='d3d11')
parser.add_argument('--compare', action='store_true')  # the screen before against the overlay
args = parser.parse_args()

data = Path(args.data)
data.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['WS_APP_DIR'] = args.app_dir
env['WS_DATA_DIR'] = str(data)
env['QT_FORCE_STDERR_LOGGING'] = '1'
for pair in args.env:
    name, _, value = pair.partition('=')
    env[name] = value

proc = subprocess.Popen([args.exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
began = time.perf_counter()
full = open(args.log, 'w', encoding='utf-8') if args.log else None
KEYS = ("warmed", 'overlay up', 'Idle trim', 'prepared in', 'Failed', 'failed', 'warning', 'cursor')


def pump_err():
    for raw in proc.stderr:
        line = raw.decode('utf-8', 'replace').rstrip()
        stamp = f'{time.perf_counter() - began:8.3f}'
        if full:
            full.write(f'{stamp} {line}\n')
            full.flush()
        if any(k in line for k in KEYS):
            print(stamp, line[:400], flush=True)


def pump_out():
    for raw in proc.stdout:  # the plugin's requests to the host: answered with nothing
        try:
            message = json.loads(raw)
        except ValueError:
            continue
        if 'id' in message and 'method' in message:
            send({'jsonrpc': '2.0', 'id': message['id'], 'result': {}})


lock = threading.Lock()


def send(message):
    with lock:
        proc.stdin.write((json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8'))
        proc.stdin.flush()


threading.Thread(target=pump_err, daemon=True).start()
send({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
    'dataDir': str(data), 'locale': 'zh', 'appearance': {'renderer': args.renderer},
    'capabilities': {'hostHotkeys': True}}})
threading.Thread(target=pump_out, daemon=True).start()
time.sleep(4)  # the overlays warm up
serial = 10
try:
    for gap in [float(g) for g in args.gaps.split(',') if g]:
        time.sleep(gap)
        print(f'{time.perf_counter() - began:8.3f} -- start after {gap:g} s idle', flush=True)
        serial += 1
        before = None
        if args.compare:
            from PIL import ImageGrab
            import numpy
            before = numpy.asarray(ImageGrab.grab().convert('RGB')).astype(numpy.int16)
        send({'jsonrpc': '2.0', 'id': serial, 'method': 'action', 'params': {'item': {'id': 'start'}}})
        time.sleep(0.2 + args.hold + 1.2)  # the action's own 200 ms, a slow cold start
        if before is not None:
            after = numpy.asarray(ImageGrab.grab().convert('RGB')).astype(numpy.int16)
            same = (before == after).all(axis=2)
            tinted = (numpy.abs(after - numpy.round(before * 0.55)) <= 1).all(axis=2) & ~same
            total = same.size
            print(f'compare: identical {same.sum() / total:.1%}, dimmed 45% {tinted.sum() / total:.1%}, other {(total - same.sum() - tinted.sum()) / total:.2%}', flush=True)
        serial += 1
        send({'jsonrpc': '2.0', 'id': serial, 'method': 'action', 'params': {'item': {'id': 'start'}}})  # again: cancels
        time.sleep(0.6)
finally:
    serial += 1
    try:
        send({'jsonrpc': '2.0', 'id': serial, 'method': 'shutdown', 'params': {}})
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    print('exit', proc.returncode)
