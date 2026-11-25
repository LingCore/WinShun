# Checks the line winshun.topmost draws round a window it keeps on top, on the
# real screen: drives WinShunTopmost.exe as Win顺 would (JSON-RPC over stdio)
# on test windows of its own, moves them with SetWindowPos / ShowWindow and
# reads the pixels. Never sends keyboard or mouse input.
#   python border.py <WinShunTopmost.exe> <output dir>
import ctypes, json, subprocess, sys, time, os, threading, queue
from ctypes import wintypes
from PIL import ImageGrab

exe, out = sys.argv[1], sys.argv[2]
user32, dwm, shcore = ctypes.windll.user32, ctypes.windll.dwmapi, ctypes.windll.shcore
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
user32.FindWindowW.restype = wintypes.HWND
user32.GetWindow.restype = wintypes.HWND
user32.GetWindow.argtypes = [wintypes.HWND, ctypes.c_uint]
user32.MonitorFromWindow.restype = wintypes.HANDLE
user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
dwm.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
ENUM = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

TITLE, COVER = "置顶测试窗口 wsborder4410", "遮住它的置顶窗口 wsborder4411"
BLUE, GREEN = (0, 95, 184), (16, 124, 16)
results = []

def check(ok, what):
    results.append((bool(ok), what))
    print(("PASS " if ok else "FAIL ") + what, flush=True)

def tk_window(title, geometry, text):
    code = ("import tkinter as t;r=t.Tk();r.title(%r);r.geometry(%r);r.configure(bg='white');"
            "t.Label(r,text=%r,bg='white',font=('Microsoft YaHei UI',14)).pack(expand=True);r.mainloop()"
            % (title, geometry, text))
    process = subprocess.Popen([sys.executable, "-c", code])
    for _ in range(100):
        found = user32.FindWindowW(None, title)
        if found:
            return process, found
        time.sleep(0.1)
    return process, None

def frame_of(h):
    r = wintypes.RECT()
    if dwm.DwmGetWindowAttribute(h, 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        user32.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)

def monitor_of(h):
    class MI(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]
    mi = MI(); mi.cbSize = ctypes.sizeof(MI)
    monitor = user32.MonitorFromWindow(h, 2)
    user32.GetMonitorInfoW(monitor, ctypes.byref(mi))
    x, y = ctypes.c_uint(96), ctypes.c_uint(96)
    shcore.GetDpiForMonitor(monitor, 0, ctypes.byref(x), ctypes.byref(y))
    m, w = mi.rcMonitor, mi.rcWork
    return (m.left, m.top, m.right, m.bottom), (w.left, w.top, w.right, w.bottom), x.value / 96.0

def strips(pid):
    """The plugin's line pieces that are shown: their rectangles."""
    found = []
    def each(h, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, name, 64)
        if owner.value == pid and name.value == "WinShunTopmostBorder" and user32.IsWindowVisible(h):
            r = wintypes.RECT(); user32.GetWindowRect(h, ctypes.byref(r))
            found.append((r.left, r.top, r.right, r.bottom))
        return True
    user32.EnumWindows(ENUM(each), 0)
    return sorted(found)

def expected(frame, scale, square=False):
    l, t, r, b = frame
    thick = max(1, int(2 * scale + 0.5))
    band = thick if square else max(thick, int(8 * scale + 0.5))
    return sorted([(l, t, r, t + band), (l, b - band, r, b), (l, t + band, l + thick, b - band), (r - thick, t + band, r, b - band)])

def wait_for(test, timeout=2.0):
    end = time.time() + timeout
    while time.time() < end:
        if test():
            return True
        time.sleep(0.05)
    return test()

def pixel(x, y):
    return ImageGrab.grab(bbox=(x, y, x + 1, y + 1), all_screens=True, include_layered_windows=True).getpixel((0, 0))[:3]

def edge_pixels(frame, scale, colour, square=False):
    """Midpoints of the four edges, the stroke's every pixel, the first pixel
    inside it, and the corner."""
    l, t, r, b = frame
    thick = max(1, int(2 * scale + 0.5))
    cx, cy = (l + r) // 2, (t + b) // 2
    wrong = []
    for d in range(thick):
        for name, at in (("top", (cx, t + d)), ("bottom", (cx, b - 1 - d)), ("left", (l + d, cy)), ("right", (r - 1 - d, cy))):
            if pixel(*at) != colour:
                wrong.append("%s+%d is %s" % (name, d, pixel(*at)))
    for name, at in (("top", (cx, t + thick)), ("left", (l + thick, cy)), ("right", (r - 1 - thick, cy)), ("bottom", (cx, b - 1 - thick))):
        if pixel(*at) == colour:
            wrong.append("inside the %s edge is the line's colour" % name)
    if square:
        if pixel(l, t) != colour or pixel(r - 1, b - 1) != colour:
            wrong.append("square corner is %s / %s" % (pixel(l, t), pixel(r - 1, b - 1)))
    else:
        # On the arc at 45 degrees: the middle of the stroke.
        radius = int(8 * scale + 0.5)
        along = int(radius - (radius - thick / 2) * 0.7071)
        for name, at in (("top left", (l + along, t + along)), ("bottom right", (r - 1 - along, b - 1 - along))):
            if pixel(*at) != colour:
                wrong.append("%s arc (%d px in) is %s" % (name, along, pixel(*at)))
        if pixel(l, t) == colour:
            wrong.append("the corner outside the arc is the line's colour")
    return wrong

def topmost(h):
    return bool(user32.GetWindowLongPtrW(h, -20) & 0x8)

def move(h, x, y, w, h_):
    user32.SetWindowPos(h, None, x, y, w, h_, 0x0004 | 0x0010 | 0x4000)  # NOZORDER | NOACTIVATE | ASYNC

tk, hwnd = tk_window(TITLE, "640x360+300+200", "窗口置顶插件的测试窗口")
check(hwnd, "test window is up")
time.sleep(0.6)

p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
lines = queue.Queue()
threading.Thread(target=lambda: [lines.put(l) for l in iter(p.stdout.readline, b"")], daemon=True).start()
def send(m):
    m["jsonrpc"] = "2.0"
    p.stdin.write((json.dumps(m, ensure_ascii=False) + "\n").encode("utf-8")); p.stdin.flush()
def answer_of(id_, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            m = json.loads(lines.get(timeout=max(end - time.time(), 0.01)).decode("utf-8"))
        except queue.Empty:
            return None
        if m.get("method") == "winshun/tool":
            send({"id": m["id"], "result": {}})
        elif m.get("id") == id_ and "method" not in m:
            return m
    return None

def look(accent):
    return {"locale": "zh", "theme": "light", "corners": "round",
            "nativeLook": {"style": "fluent", "dark": False, "rounded": True, "accent": accent, "accentFill": "#ff0f6cbd",
                           "dangerFill": "#ffc42b1c", "background": "#fff3f3f3", "windowEdge": "#24000000", "text": "#ff1b1b1b"}}
first = look("#ff005fb8")
send({"id": 1, "method": "initialize", "params": {"protocolVersion": "1", "capabilities": {"hostHotkeys": True, "details": True},
      "pluginDir": os.path.dirname(exe), "dataDir": out, "locale": "zh", "theme": "light",
      "appearance": {k: first[k] for k in ("corners", "nativeLook")}, "settings": {"hotkey": "", "border": True}}})
check(answer_of(1), "initialize answered")
next_id = [10]
def rows(text):
    next_id[0] += 1
    send({"id": next_id[0], "method": "query", "params": {"queryId": next_id[0], "command": "topmost", "trigger": "keyword", "keyword": "置顶", "text": text, "raw": "置顶 " + text}})
    r = answer_of(next_id[0])
    return r["result"]["items"] if r else []
def press(item):
    next_id[0] += 1
    send({"id": next_id[0], "method": "action", "params": {"command": "topmost", "action": "default", "text": "", "item": item}})
    return answer_of(next_id[0])

monitor, work, scale = monitor_of(hwnd)
print("scale %.2f, monitor %s" % (scale, monitor), flush=True)
check(strips(p.pid) == [], "no line before anything is on top")
items = rows("wsborder4410")
check(len(items) == 1, "the test window is listed")
press(items[0])
check(topmost(hwnd), "on top after Enter")

# 1. The line: four pieces on the window's edge, the right pixels.
frame = frame_of(hwnd)
check(wait_for(lambda: strips(p.pid) == expected(frame, scale)), "four pieces on the window's edge: %s (frame %s)" % (strips(p.pid), frame))
time.sleep(1.4)  # the "已置顶" note has gone
wrong = edge_pixels(frame, scale, BLUE)
check(not wrong, "the line is %d px of the accent, round at the corners %s" % (max(1, int(2 * scale + 0.5)), wrong))
l, t, r, b = frame
ImageGrab.grab(bbox=(l - 12, t - 12, r + 12, b + 12), all_screens=True, include_layered_windows=True).save(os.path.join(out, "border.png"))
ImageGrab.grab(bbox=(l - 6, t - 6, l + 58, t + 42), all_screens=True, include_layered_windows=True).resize((384, 288), 0).save(os.path.join(out, "border-corner.png"))
above = user32.GetWindow(hwnd, 3)
name = ctypes.create_unicode_buffer(64); user32.GetClassNameW(above, name, 64)
check(name.value == "WinShunTopmostBorder", "the pieces are right above their window in the z-order (%s)" % name.value)

# 2. It follows the window: moved, resized.
move(hwnd, 520, 260, 800, 480)
check(wait_for(lambda: frame_of(hwnd) != frame and strips(p.pid) == expected(frame_of(hwnd), scale)), "follows a move and a resize: %s" % strips(p.pid))
time.sleep(0.2)
wrong = edge_pixels(frame_of(hwnd), scale, BLUE)
check(not wrong, "and is drawn for the new size %s" % wrong)

# 3. Minimized and back.
user32.ShowWindow(hwnd, 6)
check(wait_for(lambda: strips(p.pid) == []), "minimized: no line")
user32.ShowWindow(hwnd, 9)
check(wait_for(lambda: strips(p.pid) == expected(frame_of(hwnd), scale)), "restored: the line is back")

# 4. Maximized: on the edge of the work area, square.
user32.ShowWindow(hwnd, 3)
check(wait_for(lambda: strips(p.pid) == expected(work, scale, True)), "maximized: square, on the work area's edge: %s (work %s)" % (strips(p.pid), work))
time.sleep(0.4)
wrong = edge_pixels(work, scale, BLUE, True)
check(not wrong, "maximized: drawn %s" % wrong)
user32.ShowWindow(hwnd, 9)
check(wait_for(lambda: strips(p.pid) == expected(frame_of(hwnd), scale)), "back from maximized")

# 5. Full screen (the window as large as its monitor): no line.
style = user32.GetWindowLongPtrW(hwnd, -16)
user32.SetWindowLongPtrW(hwnd, -16, 0x80000000 | 0x10000000)  # WS_POPUP | WS_VISIBLE: no frame, as a player in full screen
user32.SetWindowPos(hwnd, None, monitor[0], monitor[1], monitor[2] - monitor[0], monitor[3] - monitor[1], 0x0004 | 0x0010 | 0x0020 | 0x4000)
covers = lambda a, m: a[0] <= m[0] and a[1] <= m[1] and a[2] >= m[2] and a[3] >= m[3]
check(wait_for(lambda: covers(frame_of(hwnd), monitor) and strips(p.pid) == []), "as large as the monitor: no line (frame %s, pieces %s)" % (frame_of(hwnd), strips(p.pid)))
user32.SetWindowLongPtrW(hwnd, -16, style)
user32.SetWindowPos(hwnd, None, 520, 260, 800, 480, 0x0004 | 0x0010 | 0x0020 | 0x4000)
check(wait_for(lambda: strips(p.pid) == expected(frame_of(hwnd), scale)), "back from full screen")

# 6. Another window on top over it covers the line.
cover, chwnd = tk_window(COVER, "400x300+420+200", "另一个置顶窗口")
time.sleep(0.6)
user32.SetWindowPos(chwnd, wintypes.HWND(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)  # on top, above the first and its line
time.sleep(0.3)
f = frame_of(hwnd); c = frame_of(chwnd)
at = ((max(f[0], c[0]) + min(f[2], c[2])) // 2, f[3] - 1)  # on the first window's bottom edge, under the cover
check(c[0] < at[0] < c[2] and c[1] < at[1] < c[3] and topmost(chwnd), "the cover is over the bottom edge (cover %s, window %s, on top %s)" % (c, f, topmost(chwnd)))
check(wait_for(lambda: pixel(*at) != BLUE), "the line does not show through a window above its own: %s" % (pixel(*at),))
check(pixel(f[0] + 1, (f[1] + f[3]) // 2) == BLUE and not c[0] <= f[0] + 1, "and still shows where nothing covers it")
cover.kill()
check(wait_for(lambda: pixel(*at) == BLUE), "the cover gone, the line is whole again")

# 7. The colour follows Win顺's look.
send({"method": "appearance", "params": look("#ff107c10")})
check(wait_for(lambda: not edge_pixels(frame_of(hwnd), scale, GREEN)), "a new accent: the line is redrawn in it %s" % edge_pixels(frame_of(hwnd), scale, GREEN))

# 8. The setting.
send({"method": "settingsChanged", "params": {"settings": {"hotkey": "", "border": False}}})
check(wait_for(lambda: strips(p.pid) == []) and topmost(hwnd), "the setting off: no line, still on top")
send({"method": "settingsChanged", "params": {"settings": {"hotkey": "", "border": True}}})
check(wait_for(lambda: strips(p.pid) == expected(frame_of(hwnd), scale)), "the setting on: the line is back")

# 9. Taken off the top by another program: the line goes by itself.
user32.SetWindowPos(hwnd, wintypes.HWND(-2), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
check(wait_for(lambda: strips(p.pid) == []) and not topmost(hwnd), "taken off by another program: no line")

# 10. Off through the plugin, and a window that closes while on top.
item = [i for i in rows("wsborder4410") if i["title"] == TITLE][0]
press(item)
check(wait_for(lambda: strips(p.pid) == expected(frame_of(hwnd), scale)), "on top again: the line again")
item = [i for i in rows("wsborder4410") if i["title"] == TITLE][0]
press(item)
check(wait_for(lambda: strips(p.pid) == []) and not topmost(hwnd), "Enter on it takes it off: no line")
item = [i for i in rows("wsborder4410") if i["title"] == TITLE][0]
press(item)
check(wait_for(lambda: len(strips(p.pid)) == 4), "on top once more")
tk.kill()
check(wait_for(lambda: strips(p.pid) == []), "the window closed: no line left behind")

send({"id": 99, "method": "shutdown", "params": {}})
check(answer_of(99) is not None, "shutdown answered")
p.stdin.close()
try:
    p.wait(3)
except subprocess.TimeoutExpired:
    p.kill()
check(p.returncode == 0, "the plugin ended by itself: %s" % p.returncode)
bad = [w for ok, w in results if not ok]
print("RESULT %d passed, %d failed" % (len(results) - len(bad), len(bad)), flush=True)
sys.exit(1 if bad else 0)
