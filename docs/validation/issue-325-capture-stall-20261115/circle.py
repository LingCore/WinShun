"""Starts a stand-alone capture plugin, opens a capture, moves the real mouse
in circles over it and logs the system cursor every 2 ms.

  python circle.py --plugin <dir with WinShunCapture.exe> --app-dir <dir with Qt> --data <dir>
                   [--circles 3] [--seconds 4] [--renderer d3d11] [--env NAME=VALUE]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import math
import os
import subprocess
import threading
import time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--plugin', required=True)
ap.add_argument('--app-dir', required=True)
ap.add_argument('--data', required=True)
ap.add_argument('--circles', type=float, default=3)
ap.add_argument('--seconds', type=float, default=4)
ap.add_argument('--renderer', default='d3d11')
ap.add_argument('--env', action='append', default=[])
ap.add_argument('--log', default='')
ap.add_argument('--cover', action='store_true')  # another process's topmost window on the circle's right
args = ap.parse_args()

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


class CURSORINFO(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('flags', wt.DWORD), ('hCursor', ctypes.c_void_p), ('pt', wt.POINT)]


class ICONINFO(ctypes.Structure):
    _fields_ = [('fIcon', wt.BOOL), ('xHotspot', wt.DWORD), ('yHotspot', wt.DWORD),
                ('hbmMask', ctypes.c_void_p), ('hbmColor', ctypes.c_void_p)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', wt.LONG), ('dy', wt.LONG), ('mouseData', wt.DWORD), ('dwFlags', wt.DWORD),
                ('time', wt.DWORD), ('dwExtraInfo', ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [('mi', MOUSEINPUT), ('pad', ctypes.c_byte * 32)]
    _anonymous_ = ('u',)
    _fields_ = [('type', wt.DWORD), ('u', _U)]


user32.GetCursorInfo.argtypes = [ctypes.POINTER(CURSORINFO)]
user32.GetIconInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(ICONINFO)]
user32.WindowFromPoint.argtypes = [wt.POINT]
user32.WindowFromPoint.restype = ctypes.c_void_p
user32.GetAncestor.argtypes = [ctypes.c_void_p, wt.UINT]
user32.GetAncestor.restype = ctypes.c_void_p
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.DWORD)]
user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
gdi32 = ctypes.WinDLL('gdi32')
gdi32.DeleteObject.argtypes = [ctypes.c_void_p]

data = Path(args.data)
data.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['WS_APP_DIR'] = args.app_dir
env['WS_DATA_DIR'] = str(data)
env['QT_FORCE_STDERR_LOGGING'] = '1'
for pair in args.env:
    k, _, v = pair.partition('=')
    env[k] = v
exe = str(Path(args.plugin) / 'WinShunCapture.exe')
proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
began = time.perf_counter()
full = open(args.log, 'w', encoding='utf-8') if args.log else None


def pump_err():
    for raw in proc.stderr:
        line = raw.decode('utf-8', 'replace').rstrip()
        if full:
            full.write(f'{time.perf_counter() - began:8.3f} {line}\n')
            full.flush()


def pump_out():
    for raw in proc.stdout:
        try:
            m = json.loads(raw)
        except ValueError:
            continue
        if 'id' in m and 'method' in m:
            send({'jsonrpc': '2.0', 'id': m['id'], 'result': {}})


lock = threading.Lock()


def send(m):
    with lock:
        proc.stdin.write((json.dumps(m) + '\n').encode())
        proc.stdin.flush()


def cursor():
    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(ci)
    user32.GetCursorInfo(ctypes.byref(ci))
    return ci


def readable(h):
    if not h:
        return False
    ii = ICONINFO()
    ok = bool(user32.GetIconInfo(ctypes.c_void_p(h), ctypes.byref(ii)))
    if ok:
        if ii.hbmMask:
            gdi32.DeleteObject(ii.hbmMask)
        if ii.hbmColor:
            gdi32.DeleteObject(ii.hbmColor)
    return ok


def owner_pid(pt):
    h = user32.WindowFromPoint(pt)
    root = user32.GetAncestor(h, 2) if h else None
    pid = wt.DWORD()
    if root:
        user32.GetWindowThreadProcessId(root, ctypes.byref(pid))
    return pid.value


def move_abs(x, y):
    vx, vy = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)
    vw, vh = user32.GetSystemMetrics(78), user32.GetSystemMetrics(79)
    inp = INPUT()
    inp.type = 0
    inp.mi.dx = int((x - vx) * 65535 / (vw - 1))
    inp.mi.dy = int((y - vy) * 65535 / (vh - 1))
    inp.mi.dwFlags = 0x0001 | 0x8000 | 0x4000  # MOVE | ABSOLUTE | VIRTUALDESK
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(inp))


threading.Thread(target=pump_err, daemon=True).start()
send({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
    'dataDir': str(data), 'locale': 'zh', 'appearance': {'renderer': args.renderer},
    'capabilities': {'hostHotkeys': True}}})
threading.Thread(target=pump_out, daemon=True).start()
time.sleep(4)
start = cursor().pt
home = (start.x, start.y)
samples = []
stop = threading.Event()


def watch():
    while not stop.is_set():
        ci = cursor()
        samples.append((time.perf_counter() - began, ci.hCursor or 0, ci.flags, ci.pt.x, ci.pt.y))
        time.sleep(0.002)


try:
    send({'jsonrpc': '2.0', 'id': 11, 'method': 'action', 'params': {'item': {'id': 'start'}}})
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 3:
        if owner_pid(cursor().pt) == proc.pid:
            break
        time.sleep(0.02)
    else:
        raise SystemExit('overlay not under the pointer')
    time.sleep(0.4)
    threading.Thread(target=watch, daemon=True).start()
    sw, sh = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    cx, cy, r = sw / 2, sh / 2, min(sw, sh) * 0.42
    allowed = {proc.pid}
    if args.cover:
        import sys
        cover = subprocess.Popen([sys.executable, '-I', str(Path(__file__).with_name('cover.py')),
                                  str(int(cx + r - 100)), str(int(cy - 100)), '200', '200', str(args.seconds * 5 + 3)])
        allowed.add(cover.pid)
        time.sleep(0.8)
    steps = int(args.seconds * 250)
    for i in range(steps + 1):
        if owner_pid(cursor().pt) not in allowed:
            raise SystemExit('overlay gone from under the pointer')
        a = 2 * math.pi * args.circles * i / steps
        move_abs(cx + r * math.cos(a), cy + r * math.sin(a))
        time.sleep(args.seconds / steps)
    time.sleep(0.5)
finally:
    stop.set()
    move_abs(*home)
    send({'jsonrpc': '2.0', 'id': 12, 'method': 'action', 'params': {'item': {'id': 'start'}}})  # cancels
    time.sleep(0.8)
    send({'jsonrpc': '2.0', 'id': 13, 'method': 'shutdown', 'params': {}})
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

handles = {}
for t, h, f, x, y in samples:
    handles.setdefault(h, [0, t, t, f])
    handles[h][0] += 1
    handles[h][2] = t
changes = sum(1 for a, b in zip(samples, samples[1:]) if a[1] != b[1])
print(f'{len(samples)} samples, {changes} handle changes')
for h, (n, t1, t2, f) in handles.items():
    print(f'  cursor {h:#x}: {n} samples {t1:.2f}-{t2:.2f}s flags {f} readable {readable(h)}')
hidden = [s for s in samples if not (s[2] & 1) or (s[2] & 2) or not s[1]]
print('hidden/null samples:', len(hidden))
