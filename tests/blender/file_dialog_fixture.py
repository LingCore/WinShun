# SPDX-FileCopyrightText: 2026 LingCore
# SPDX-License-Identifier: MIT
"""Only launched by verify-blender-dialog.ps1 with private config and temp roots."""
import bpy
import ctypes
import json
import logging
import os
import pathlib
import sys
import time

logging.basicConfig(level=logging.DEBUG)

root = pathlib.Path(os.environ['WINSHUN_TEST_BLENDER_OUTPUT']).resolve()
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'integrations' / 'blender'))
import winshun_file_dialog as integration
integration.register()
sequence = 0
started = time.monotonic()
user32 = ctypes.windll.user32
user32.GetWindow.restype = ctypes.c_void_p
user32.GetForegroundWindow.restype = ctypes.c_void_p
user32.GetPropW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
user32.GetPropW.restype = ctypes.c_void_p
target = root / "blender-start"
target.mkdir(exist_ok=True)
temporary = root / "blender-temp"
temporary.mkdir(exist_ok=True)
bpy.context.preferences.filepaths.temporary_directory = str(temporary) + os.sep
sample = target / "sample.blend"

def poll():
    global sequence
    windows = []
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'FILE_BROWSER':
                p = area.spaces.active.params
                windows.append({"directory": os.fsdecode(p.directory), "filename": p.filename,
                                "area": [area.x, area.y, area.width, area.height],
                                "window_size": [window.width, window.height]})
    native = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def visit(hwnd, arg):
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == os.getpid():
            title = ctypes.create_unicode_buffer(512)
            cls = ctypes.create_unicode_buffer(128)
            user32.GetWindowTextW(hwnd, title, 512)
            user32.GetClassNameW(hwnd, cls, 128)
            native.append({"hwnd": int(hwnd), "title": title.value, "class": cls.value,
                           "owner": user32.GetWindow(hwnd, 4) or 0,
                           "token": user32.GetPropW(hwnd, integration.PROPERTY) or 0,
                           "visible": bool(user32.IsWindowVisible(hwnd))})
        return True
    user32.EnumWindows(visit, 0)
    data = {"pid": os.getpid(), "browsers": windows, "windows": native,
            "sample": str(sample), "filepath": bpy.data.filepath,
            "sequence": sequence,
            "extension_listening": bool(integration._server and integration._server.listening.is_set()),
            "foreground": user32.GetForegroundWindow() or 0}
    try:
        (root / "blender-state-new.json").write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
        os.replace(root / "blender-state-new.json", root / "blender-state.json")
    except OSError:
        pass
    command = root / "blender-command.json"
    if command.exists():
        try:
            action = json.loads(command.read_text(encoding='utf-8-sig'))
            if action.get('extension') == 'off':
                integration.unregister()
            elif action.get('extension') == 'on':
                integration.register()
            if action.get('open'):
                bpy.ops.wm.open_mainfile('INVOKE_DEFAULT')
            if action.get('save_as'):
                bpy.ops.wm.save_as_mainfile('INVOKE_DEFAULT', filepath=str(sample))
            if action.get('cancel'):
                for window in list(bpy.context.window_manager.windows):
                    for area in window.screen.areas:
                        if area.type == 'FILE_BROWSER':
                            with bpy.context.temp_override(window=window, area=area):
                                bpy.ops.file.cancel('INVOKE_DEFAULT')
            command.unlink()
            sequence += 1
        except (OSError, ValueError):
            pass
    if (root / "blender-stop").exists() or time.monotonic() - started > 7200:
        if integration._server is not None:
            integration.unregister()
        os._exit(0)  # fixture only: do not replace the user's global quit.blend recovery
        return None
    return 0.25

def begin():
    language = os.environ.get('WINSHUN_TEST_BLENDER_LANGUAGE', 'en_US')
    bpy.context.preferences.view.language = language
    bpy.context.preferences.view.use_translate_interface = language != 'en_US'
    bpy.ops.wm.save_as_mainfile(filepath=str(sample))
    bpy.ops.wm.open_mainfile('INVOKE_DEFAULT')
    bpy.app.timers.register(poll, first_interval=1.5, persistent=True)
    return None

bpy.app.timers.register(begin, first_interval=2.0)
