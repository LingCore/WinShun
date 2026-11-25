# Drives WinShunTopmost.exe as Win顺 would (JSON-RPC over stdio), on a test
# window of its own. Never sends input; the hotkey step runs only while the
# test window is the foreground window.
import ctypes, json, subprocess, sys, time, os, threading, queue
from ctypes import wintypes
from PIL import ImageGrab

exe, out = sys.argv[1], sys.argv[2]
theme = sys.argv[3] if len(sys.argv) > 3 else "light"
user32 = ctypes.windll.user32
ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
user32.FindWindowW.restype = wintypes.HWND
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
TITLE = "置顶测试窗口 wstop7731"
results = []

def check(ok, what):
    results.append((bool(ok), what))
    print(("PASS " if ok else "FAIL ") + what, flush=True)

tk = subprocess.Popen([sys.executable, "-c",
    "import tkinter as t;r=t.Tk();r.title(%r);r.geometry('640x360+300+200');t.Label(r,text='窗口置顶插件的测试窗口',font=('Microsoft YaHei UI',14)).pack(expand=True);r.after(400,r.focus_force);r.mainloop()" % TITLE])
hwnd = None
for _ in range(100):
    hwnd = user32.FindWindowW(None, TITLE)
    if hwnd: break
    time.sleep(0.1)
check(hwnd, "test window is up")
time.sleep(0.8)

def topmost():
    return bool(user32.GetWindowLongPtrW(hwnd, -20) & 0x8)

p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
lines = queue.Queue()
threading.Thread(target=lambda: [lines.put(l) for l in iter(p.stdout.readline, b"")], daemon=True).start()
seen = []
def send(m):
    m["jsonrpc"] = "2.0"
    p.stdin.write((json.dumps(m, ensure_ascii=False) + "\n").encode("utf-8")); p.stdin.flush()
def read(timeout=3.0):
    try:
        m = json.loads(lines.get(timeout=timeout).decode("utf-8"))
    except queue.Empty:
        return None
    seen.append(m)
    return m
def answer_of(id_, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        m = read(end - time.time())
        if m is None: return None
        if "method" in m and m["method"] == "winshun/tool":
            tool = m["params"]["name"]
            send({"id": m["id"], "result": {"errors": hotkey_errors} if tool == "register_hotkeys" else {}})
            continue
        if m.get("id") == id_ and "method" not in m:
            return m
    return None

dark = theme == "dark"
native = {"style": "fluent", "dark": dark, "rounded": True,
          "accentFill": "#ff0f6cbd", "dangerFill": "#ffc42b1c",
          "background": "#ff202020" if dark else "#fff3f3f3", "windowEdge": "#33ffffff" if dark else "#24000000",
          "text": "#ffffffff" if dark else "#ff1b1b1b"}
hotkey_errors = {}
send({"id": 1, "method": "initialize", "params": {"protocolVersion": "1", "capabilities": {"hostHotkeys": True, "details": True},
      "pluginDir": os.path.dirname(exe), "dataDir": out, "locale": "zh", "theme": theme,
      "appearance": {"corners": "round", "nativeLook": native}, "settings": {"hotkey": "Win+Ctrl+T"}}})
r = answer_of(1)
check(r and r["result"].get("name") == "winshun.topmost", "initialize answered")
# The tool call for the shortcut follows the answer.
send({"id": 2, "method": "query", "params": {"queryId": 1, "command": "topmost", "trigger": "keyword", "keyword": "置顶", "text": "wstop7731", "raw": "置顶 wstop7731"}})
r = answer_of(2)
reported = [m for m in seen if m.get("method") == "winshun/hotkeys"]
asked = [m for m in seen if m.get("method") == "winshun/tool" and m["params"]["name"] == "register_hotkeys"]
check(reported and reported[0]["params"]["keys"] == ["Win+Ctrl+T"], "winshun/hotkeys reports the shortcut")
check(asked and asked[0]["params"]["arguments"]["keys"] == ["Win+Ctrl+T"], "register_hotkeys asks for the shortcut")
items = r["result"]["items"] if r else []
check(len(items) == 1 and items[0]["title"] == TITLE and "Enter 置顶" in items[0]["subtitle"], "query finds the window, not on top: %s" % [i.get("subtitle") for i in items])
check(items and items[0]["icon"].lower().startswith("file:") and items[0]["icon"].lower().endswith("python.exe"), "the row shows the program's icon")

def shot(name, wait=0.35):
    time.sleep(wait)
    rect = wintypes.RECT(); user32.GetWindowRect(hwnd, ctypes.byref(rect))
    cx = (rect.left + rect.right) // 2
    box = (cx - 330, rect.top, cx + 330, rect.top + 90)
    ImageGrab.grab(bbox=box, all_screens=True).save(os.path.join(out, name))

send({"id": 3, "method": "action", "params": {"command": "topmost", "action": "default", "text": "wstop7731", "item": items[0]}})
r = answer_of(3)
check(r and r["result"]["effects"] == [{"type": "hide"}], "action answers with hide")
check(topmost(), "the window is on top after Enter")
shot("pin-%s.png" % theme)

send({"id": 4, "method": "query", "params": {"queryId": 2, "command": "topmost", "trigger": "keyword", "keyword": "置顶", "text": "", "raw": "置顶 "}})
r = answer_of(4)
items = r["result"]["items"] if r else []
check(items and items[0]["title"] == TITLE and items[0]["subtitle"].startswith("已置顶"), "listed first as on top: %s" % (items[0]["subtitle"] if items else None))
check(all(i["id"] in ("window", "none") for i in items), "only windows (and the take-all-off row) are listed: %d rows" % len(items))
mine = [i for i in items if i["title"] == TITLE][0]

time.sleep(1.4)  # the first note has gone
front = user32.GetForegroundWindow()
if front == hwnd:
    send({"method": "hotkey", "params": {"key": "Win+Ctrl+T"}})
    time.sleep(0.25)
    check(not topmost(), "the shortcut takes the window in front off the top")
    shot("unpin-%s.png" % theme, 0.15)
    time.sleep(1.3)
    send({"method": "hotkey", "params": {"key": "Win+Ctrl+T"}})
    time.sleep(0.25)
    check(topmost(), "the shortcut puts it on top again")
else:
    print("SKIP hotkey steps: the test window is not in front (someone is using the PC)", flush=True)
    send({"id": 5, "method": "action", "params": {"command": "topmost", "action": "default", "text": "", "item": mine}})
    answer_of(5)
    check(not topmost(), "Enter on a window on top takes it off")
    shot("unpin-%s.png" % theme)
    time.sleep(1.2)
    mine["data"]["on"] = True
    send({"id": 6, "method": "action", "params": {"command": "topmost", "action": "default", "text": "", "item": mine}})
    answer_of(6)
    check(topmost(), "and Enter puts it back")

# A shortcut another program holds: said in the log.
hotkey_errors = {"Ctrl+Alt+F9": 1409}
send({"method": "settingsChanged", "params": {"settings": {"hotkey": "Ctrl+Alt+F9"}}})
time.sleep(0.5)
send({"id": 7, "method": "query", "params": {"queryId": 3, "command": "topmost", "trigger": "keyword", "keyword": "置顶", "text": "没有这样的窗口zzz", "raw": "x"}})
r = answer_of(7)
check(r and r["result"]["items"][0]["id"] == "nothing" and r["result"]["items"][0].get("valid") is False, "no match: one row that says so")
logs = [m["params"]["text"] for m in seen if m.get("method") == "winshun/log"]
check(any("Ctrl+Alt+F9" in t and "1409" in t for t in logs), "a shortcut not had is logged: %s" % logs)
time.sleep(0.4)
shot("taken-%s.png" % theme, 0)

# A stale row: the window's handle with another process id.
stale = json.loads(json.dumps(mine)); stale["data"]["pid"] = 4; stale["data"]["on"] = False
send({"id": 8, "method": "action", "params": {"command": "topmost", "action": "default", "text": "", "item": stale}})
answer_of(8)
check(topmost(), "a row of another process's window changes nothing")

send({"id": 9, "method": "shutdown", "params": {}})
r = answer_of(9)
check(r is not None, "shutdown answered")
time.sleep(0.3)
check(not topmost(), "stopping the plugin takes its windows off the top")
p.stdin.close()
try:
    p.wait(3)
except subprocess.TimeoutExpired:
    p.kill()
check(p.returncode == 0, "the plugin ended by itself: %s" % p.returncode)
tk.kill()
bad = [w for ok, w in results if not ok]
print("RESULT %d passed, %d failed" % (len(results) - len(bad), len(bad)), flush=True)
sys.exit(1 if bad else 0)
