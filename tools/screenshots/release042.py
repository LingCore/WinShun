"""The posters 0.42.0 adds or redoes.

New: clipboard sync (the winshun.sync plugin, docs/images/sync.jpg). The
launcher's `同步` list comes from the release build's real program and the
real plugin: WinShun.exe as an isolated runtime probe on the real desktop
(release035.py's route: a private profile in the run's own TEMP, the HKCU of a
private registry key, no hotkeys, no instance mutex; the user's own Win顺 is
not touched), with the release's winshun.sync package installed in it
(--runtime-plugins). The other device of the group is sync_peer.exe (next to
this script, built here against the release build's wsshunsync.lib): the
ShunSync engine alone, named 办公室电脑. Both are put in one group before they
start: the script writes their identity.json (a new random group key,
protected with DPAPI for this user, as the engine does, and each one the other
as a known peer at this PC's own address), the probe's while it is held still
right after its first save of WinShun.ini, with the plugin's settings
(设备名 家里的电脑, 收到就放进剪贴板 off: what comes is only listed, nothing is
written to this PC's clipboard). Once they are connected the peer copies two
files and a folder (82 MB; as on Mac顺, files are only offered until they are
asked for) and a line of text; `show 同步` and a space (sent only
while the probe's own launcher is the foreground window) open the list, and
the launcher is taken by PrintWindow (its own pixels, nothing behind it).
What the plugin sends the peer (this PC's clipboard, if it changes meanwhile)
is counted there, never shown or kept.

New: a clipboard picture large (GitHub #414, docs/images/clipboard-picture.jpg).
The release build's production QML at 200% offscreen on a temporary history
(Preview.cpp checkClipboardPictureView with --gallery-image: the chart of
scenes/区域销售额.png copied as a picture, three entries around it), the
picture's row open in the preview and large beside the window; the two
windows' grabs are put side by side there, and each gets its own corners here.

Redone: 外观 (界面风格) and 管理员程序里照样粘贴, cut from settings pages that got a
row each (搜索框阴影 #412; 粘贴为纯文本的快捷键 #408). release039.py's route: the
release build's production QML renders every settings page at 200%
offscreen, on a private preview profile and demo data, with a neutral
USERPROFILE and TEMP. Put the release's WinShun folder at C:\\WinShun first (a
junction: mklink /J C:\\WinShun <release>\\WinShun) and pass that exe.

  python tools/screenshots/release042.py --exe C:/WinShun/WinShun.exe --build build/release042 --work build/release042-shots --capture-only
  # look at <work>/settings/p02-t0.png, p07-t1.png, <work>/sync/sync.png and
  # <work>/clipboard/clipboard-picture.png, then
  python tools/screenshots/release042.py --exe C:/WinShun/WinShun.exe --work build/release042-shots --export-only

No poster: typing in OneCommander (a real program's window), a selection over
several monitors (the capture's overlay over the whole real desktop), the
plain text key (a settings row, in admin-paste now), the gestures' shortcut
for selected text (a row at the foot of the page gestures-selection shows).
"""
import argparse
import base64
import ctypes
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import uuid
from ctypes import wintypes as wt

import numpy as np
from PIL import Image

sys.dont_write_bytecode = True

import release039  # noqa: F401  (patches the settings capture: favorites sorted, the exe's path)
import release036
import release035
import art
from release035 import Live, foreground_pid, pointer_away, print_window, user32
import shots
import stage

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent



def windows_apart(rgba):
    """The clipboard window and the large picture beside it (transparent between), each rounded as Windows 11
    rounds a window, where they are."""
    filled = (np.asarray(rgba.getchannel("A")) > 0).any(axis=0)
    out = Image.new("RGBA", rgba.size)
    x = 0
    while x < len(filled):
        if not filled[x]:
            x += 1
            continue
        end = x
        while end < len(filled) and filled[end]:
            end += 1
        piece = rgba.crop((x, 0, end, rgba.height))
        box = piece.getchannel("A").getbbox()
        out.alpha_composite(art.screen_piece(piece.crop(box), radius=16), (x + box[0], box[1]))
        x = end
    return out


release036.SOURCES = {
    "appearance": ("L-appearance", "settings/p02-t0.png", 1556),
    "admin-paste": ("L-admin-paste", "settings/p07-t1.png", 1440),
    "sync": ("L-sync", "sync/sync.png", None),
    "clipboard-picture": ("L-clipboard-picture", "clipboard/clipboard-picture.png", windows_apart),
}
shots.README_ONLY = tuple(shots.README_ONLY) + (
    ("sync", "L-sync", "剪贴板同步，[跨电脑粘贴]", "Windows 和 Mac 互通 · 文件也行 · 端到端加密"),
    ("clipboard-picture", "L-clipboard-picture", "剪贴板图片，[点开看大图]", "滚轮缩放 · 双击看原始大小 · ↑↓ 跟着换"),
)

PEER_PORT = 37231
PLUGIN_PORT = 37219  # the plugin's own (taken: it picks another and the peer finds it when it dials)


# --- DPAPI, as the engine's protectForUser (Crypto.cpp) --------------------------

class DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", wt.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32")
crypt32.CryptProtectData.argtypes = [ctypes.POINTER(DATA_BLOB), wt.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.c_void_p, wt.DWORD, ctypes.POINTER(DATA_BLOB)]
kernel32.LocalFree.argtypes = [ctypes.c_void_p]


def protect_for_user(secret):
    buffer = ctypes.create_string_buffer(secret, len(secret))
    blob_in = DATA_BLOB(len(secret), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_char)))
    blob_out = DATA_BLOB()
    if not crypt32.CryptProtectData(ctypes.byref(blob_in), "ShunSync group key", None, None, None, 1,
                                    ctypes.byref(blob_out)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        kernel32.LocalFree(blob_out.pbData)


def identity(device_id, key, peer_id, peer_name, host, port):
    return json.dumps({"deviceId": device_id, "groupKey": "dpapi:" + base64.b64encode(protect_for_user(key)).decode(),
                       "revokedDeviceIds": [], "knownPeers": [
                           {"deviceId": peer_id, "deviceName": peer_name, "platform": "windows", "host": host,
                            "port": port, "lastSeenMs": int(time.time() * 1000)}]}, ensure_ascii=False)


def own_address():
    """This PC's address on its network, as the poster shows it on the device's row: the one its router
    gave it (DHCP). The route to the outside can leave through a proxy's virtual adapter (198.18.0.0/15,
    a TUN), which is no address a device on the LAN would know this PC by."""
    found = subprocess.run(["pwsh", "-NoProfile", "-Command",
                            "Get-NetIPAddress -AddressFamily IPv4 -PrefixOrigin Dhcp | "
                            "Sort-Object InterfaceMetric | Select-Object -ExpandProperty IPAddress"],
                           capture_output=True, text=True).stdout.split()
    if found:
        return found[0]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 9))  # TEST-NET-1: only picks the route; nothing is sent
        address = s.getsockname()[0]
    if address.startswith(("198.18.", "198.19.")):
        raise RuntimeError(f"only a proxy's virtual adapter ({address}) found; pass a LAN address")
    return address


def clipboard_shot(exe, work):
    """clipboard/clipboard-picture.png from the release's production QML (offscreen, a temporary history)."""
    folder = work / "clipboard"
    temp = folder / "temp"
    temp.mkdir(parents=True)
    env = {**os.environ, "TEMP": str(temp), "TMP": str(temp), "QT_FONT_DPI": "96"}
    env.pop("QT_QPA_PLATFORM", None)
    result = subprocess.run([str(exe), "--preview-results", str(folder), "--clipboard-picture-view",
                             "--gallery-image", str(HERE / "scenes/区域销售额.png"), "--scale", "2", "--renderer", "d3d11"],
                            env=env)
    shutil.rmtree(temp, ignore_errors=True)
    if result.returncode or not (folder / "clipboard-picture.png").is_file():
        raise RuntimeError(f"the clipboard picture was not shot (exit {result.returncode}): see {folder / 'preview.log'}")
    return {"exit": result.returncode}


# --- the peer -------------------------------------------------------------------

def build_peer(build, out):
    """sync_peer.exe against the build's wsshunsync.lib (the C runtime it was built with: /MD)."""
    lib = next(Path(build).rglob("wsshunsync.lib"))
    exe = out / "sync_peer.exe"
    sync, localsend, sdk = ROOT / "src/sync-plugin", ROOT / "src/localsend-plugin", ROOT / "src/plugins-sdk"
    command = (f". '{ROOT / 'scripts/devshell.ps1'}' | Out-Null; "
               f"cl /nologo /std:c++20 /EHsc /utf-8 /O2 /MD /DUNICODE /D_UNICODE /DNOMINMAX /W3 "
               f"/I '{sync}' /I '{localsend}' /I '{sdk}' '{HERE / 'sync_peer.cpp'}' /Fo'{out}\\' /Fe'{exe}' "
               f"/link '{lib}' ws2_32.lib iphlpapi.lib bcrypt.lib crypt32.lib ole32.lib oleaut32.lib shell32.lib "
               f"shlwapi.lib advapi32.lib user32.lib windowscodecs.lib; exit $LASTEXITCODE")
    result = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if result.returncode or not exe.is_file():
        raise RuntimeError("sync_peer.exe did not build:\n" + result.stdout[-3000:] + result.stderr[-2000:])
    return exe


def unicode_key(char):
    """One character as typed (KEYEVENTF_UNICODE: past the IME), down and up."""
    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                    ("dwExtraInfo", ctypes.c_size_t)]

    class INPUT(ctypes.Structure):
        class _U(ctypes.Union):
            _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
        _anonymous_ = ("u",)
        _fields_ = [("type", wt.DWORD), ("u", _U)]

    inputs = (INPUT * 2)()
    for i, flags in enumerate((0x0004, 0x0004 | 0x0002)):  # KEYEVENTF_UNICODE, | KEYUP
        inputs[i].type = 1
        inputs[i].ki = KEYBDINPUT(0, ord(char), flags, 0, 0)
    user32.SendInput.argtypes = [wt.UINT, ctypes.c_void_p, ctypes.c_int]
    if user32.SendInput(2, ctypes.byref(inputs), ctypes.sizeof(INPUT)) != 2:
        raise ctypes.WinError(ctypes.get_last_error())


class SyncLive(Live):
    """release035's Live with the sync plugin installed and its group seeded."""

    def __init__(self, exe, work, plugins, plugin_identity):
        super().__init__(exe, work, "sync", 2, (), ())
        self.plugins = plugins
        self.plugin_identity = plugin_identity

    def __enter__(self):
        env = {**os.environ, "TEMP": str(self.temp), "TMP": str(self.temp), "QT_FONT_DPI": "96"}
        log = (self.dir / "process.log").open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(self.exe), "--probe-runtime", str(self.live), "--live", "--scale", str(self.scale), "--renderer", "d3d11",
             "--runtime-plugins", str(self.plugins)], env=env, stdout=log, stderr=subprocess.STDOUT)
        import threading
        thread = threading.Thread(target=self._seed, daemon=True)
        thread.start()
        thread.join(90)
        if "profile" not in self.seeded:
            self.stop()
            raise RuntimeError(f"the probe's profile was not seeded: {self.seeded}")
        return self

    def _seed(self):
        deadline = time.time() + 60
        profile = None
        while profile is None and time.time() < deadline and self.proc.poll() is None:
            with os.scandir(self.temp) as entries:
                profile = next((Path(e.path) for e in entries if e.name.startswith("WinShun-preview-") and e.is_dir()), None)
            if profile is None:
                time.sleep(0.0005)
        if profile is None:
            return
        ini = profile / "WinShun" / "WinShun.ini"
        while time.time() < deadline and not (ini.exists() and ini.stat().st_size > 0):
            time.sleep(0.0005)
        handle = release035.kernel32.OpenProcess(0x0800, False, self.proc.pid)  # PROCESS_SUSPEND_RESUME
        release035.ntdll.NtSuspendProcess(handle)
        try:
            if not (profile / "plugins" / "winshun.sync" / "plugin.json").is_file():
                self.seeded["problem"] = "the plugin was not installed yet when WinShun.ini was saved"
                return
            text = ini.read_text(encoding="utf-8")
            text = stage.ini_set(text, "Appearance", {"Theme": "light", "Transparency": "off", "UiSize": "standard",
                                                      "Corners": "round"})
            text = stage.ini_set(text, "Plugin.winshun.sync", {"Setting.name": "家里的电脑", "Setting.autoWrite": "false"})
            ini.write_text(text, encoding="utf-8")
            data = profile / "plugins" / "winshun.sync.data"
            data.mkdir(parents=True, exist_ok=True)
            (data / "identity.json").write_text(self.plugin_identity, encoding="utf-8")
        finally:
            release035.ntdll.NtResumeProcess(handle)
            release035.kernel32.CloseHandle(handle)
        self.seeded["profile"] = str(profile)


def sync_shot(exe, build, work):
    folder = work / "sync"
    folder.mkdir(parents=True)
    peer_exe = build_peer(build, folder)
    package = next(Path(build).glob("library-plugins/winshun.sync"))
    plugins = folder / "plugins"
    shutil.copytree(package, plugins / "winshun.sync")
    host = own_address()
    key = os.urandom(32)
    plugin_id, peer_id = str(uuid.uuid4()), str(uuid.uuid4())
    peer_data = folder / "peer-data"
    peer_data.mkdir()
    (peer_data / "identity.json").write_text(identity(peer_id, key, plugin_id, "家里的电脑", host, PLUGIN_PORT),
                                             encoding="utf-8")
    stop = folder / "peer-stop"
    peer_log = (folder / "peer.log").open("w", encoding="utf-8")
    peer = subprocess.Popen([str(peer_exe), str(peer_data), str(PEER_PORT), str(folder / "peer-files"), str(stop)],
                            stdout=peer_log, stderr=subprocess.STDOUT)
    report = {"host": host, "peer_port": PEER_PORT}
    try:
        with SyncLive(exe, work, plugins, identity(plugin_id, key, peer_id, "办公室电脑", host, PEER_PORT)) as probe:
            end = time.time() + 90
            while time.time() < end and "text copied" not in (folder / "peer.log").read_text(encoding="utf-8"):
                if peer.poll() is not None:
                    raise RuntimeError("the peer ended: see " + str(folder / "peer.log"))
                time.sleep(0.5)
            if "text copied" not in (folder / "peer.log").read_text(encoding="utf-8"):
                raise RuntimeError("the plugin and the peer did not connect: see peer.log and the probe's log")
            time.sleep(1.5)
            for attempt in range(1, 4):
                hwnd, info = probe.raise_launcher("同步")
                probe.require_foreground()
                unicode_key(" ")
                time.sleep(2.5)
                pointer_away([hwnd])
                time.sleep(0.6)
                now = [w for w in probe.windows() if w["class"].startswith("Main")]
                if now and foreground_pid() == probe.proc.pid:
                    break  # still up after the key, the pointer moved and the frame settled
                print(f"attempt {attempt}: the list did not open ({now})", flush=True)
                probe.send("hide")
                time.sleep(1.5)
            else:
                raise RuntimeError("the 同步 list was not shot")
            image = print_window(int(now[0]["hwnd"]))
            image.save(folder / "sync.png")
            report.update({"window": now[0], "size": image.size, "attempts": attempt, "seeded": dict(probe.seeded)})
        report["registry_keys_net_change"] = probe.seeded["registry_keys_net_change"]
    finally:
        stop.write_text("")
        try:
            peer.wait(20)
        except subprocess.TimeoutExpired:
            peer.kill()
        peer_log.close()
        shutil.rmtree(folder / "peer-files", ignore_errors=True)  # 82 MB of zeros
    return report


def main():
    if sys.stdout:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--build", type=Path, help="the release's build folder (wsshunsync.lib, library-plugins)")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--only", choices=("settings", "sync", "clipboard"), nargs="+")
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    work = args.work.resolve()
    if args.export_only:
        release036.export(work)
        return
    if work.exists():
        parser.error("--work must be a fresh directory when capturing")
    if not args.build:
        parser.error("--build is needed for capturing")
    exe = Path(args.exe).absolute()
    only = args.only or ("settings", "sync", "clipboard")
    report = {}
    if "settings" in only:
        release036.capture(exe, work, Path("C:/Temp"), Path("C:/Users/Public"))
    work.mkdir(parents=True, exist_ok=True)
    if "clipboard" in only:
        report["clipboard"] = clipboard_shot(exe, work)
    if "sync" in only:
        report["sync"] = sync_shot(exe, args.build.resolve(strict=True), work)
    (work / "capture-042.json").write_text(json.dumps({"exe": str(exe), "sha256": release035.sha256(exe), **report},
                                                      ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
