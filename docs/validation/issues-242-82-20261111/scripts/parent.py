"""Stands in for the launcher: a foreground window that starts a plugin-like child with pipes
and reports whether the foreground leaves it within 4 s."""
import ctypes, os, subprocess, sys, time, tkinter as tk
from ctypes import wintypes
u = ctypes.windll.user32; k = ctypes.windll.kernel32
u.GetForegroundWindow.restype = wintypes.HWND
u.GetAncestor.restype = wintypes.HWND
def cls(h):
    b = ctypes.create_unicode_buffer(256); u.GetClassNameW(wintypes.HWND(h), b, 256); return b.value
def pid(h):
    p = wintypes.DWORD(); u.GetWindowThreadProcessId(wintypes.HWND(h), ctypes.byref(p)); return p.value
root = tk.Tk(); root.title("ws-exp-parent"); root.geometry("420x120+200+200"); root.update()
me = u.GetAncestor(wintypes.HWND(root.winfo_id()), 2)
fg = u.GetForegroundWindow()
t = u.GetWindowThreadProcessId(wintypes.HWND(fg), None)
u.AttachThreadInput(k.GetCurrentThreadId(), t, True); u.SetForegroundWindow(wintypes.HWND(me)); u.AttachThreadInput(k.GetCurrentThreadId(), t, False)
root.update(); time.sleep(0.3); root.update()
if u.GetForegroundWindow() != me:
    print("parent could not take the foreground"); sys.exit(2)
lock = "--lock" in sys.argv
args = [a for a in sys.argv[1:] if a not in ("--lock", "--action", "--grant")]
if lock:
    print("host lock:", u.LockSetForegroundWindow(1))
child = subprocess.Popen([sys.executable] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, creationflags=0x08000000)
start = time.time(); lost = None
while time.time() - start < 4:
    root.update(); time.sleep(0.02)
    now = u.GetForegroundWindow()
    if now != me and lost is None:
        lost = (round((time.time() - start) * 1000), cls(now), bool(u.IsWindowVisible(wintypes.HWND(now))), pid(now) != os.getpid())
if lost is None and "--action" in sys.argv:
    import json
    if "--grant" in sys.argv:
        u.AllowSetForegroundWindow(-1)  # as the host does on an action (ASFW_ANY)
    for i, (method, params) in enumerate([("initialize", {"settings": {}}), ("query", {"command": "x", "query": ""}), ("action", {"command": "x", "item": {"id": "open"}})]):
        child.stdin.write((json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": method, "params": params}) + chr(10)).encode()); child.stdin.flush()
        time.sleep(0.3)
    end = time.time() + 2.5
    while time.time() < end:
        root.update(); time.sleep(0.02)
    shown = []
    def each(h, l):
        if u.IsWindowVisible(wintypes.HWND(h)) and cls(h) == "TkTopLevel" and pid(h) != os.getpid():
            shown.append(h)
        return True
    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)(each), 0)
    print("plugin windows shown:", len(shown), "; in front:", u.GetForegroundWindow() in shown)
    now = u.GetForegroundWindow()
    print("after the action: foreground", cls(now), "visible", bool(u.IsWindowVisible(wintypes.HWND(now))), "the plugin's", pid(now) != os.getpid())
child.kill()
if lock:
    u.LockSetForegroundWindow(2)
print(" ".join(sys.argv[1:]), "->", "KEPT the foreground" if lost is None else "LOST after %d ms to %s (visible %s, the child %s)" % lost)
root.destroy()
