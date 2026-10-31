# SPDX-FileCopyrightText: 2026 LingCore
# SPDX-License-Identifier: MIT

import os
import logging
import queue
import secrets
import time

import bpy

from .transport import FOLDER_PROPERTY, MODE_PROPERTY, PROPERTY, Server, Windows

_windows = None
_server = None
_views = {}
_log = logging.getLogger(__name__)


def _folder(space):
    return os.path.normpath(os.fsdecode(space.params.directory))


def _remove(window, entry):
    _windows.remove_prop(window, PROPERTY)
    _windows.remove_prop(window, FOLDER_PROPERTY)
    _windows.remove_prop(window, MODE_PROPERTY)
    if entry.get("atom"):
        _windows.delete_atom(entry["atom"])


def _publish_folder(window, entry):
    folder = _folder(entry["space"])
    if folder == entry.get("folder"):
        return
    _windows.remove_prop(window, FOLDER_PROPERTY)
    if entry.get("atom"):
        _windows.delete_atom(entry["atom"])
    # Windows atoms hold at most 255 UTF-16 units. Longer paths still navigate
    # through the pipe; only the optional "current folder" hint is unavailable.
    entry["atom"] = _windows.atom(folder) if len(folder.encode("utf-16-le")) <= 510 and os.path.isabs(folder) else 0
    entry["folder"] = folder
    if entry["atom"]:
        _windows.set_prop(window, FOLDER_PROPERTY, entry["atom"])


def _sync_views():
    native = _windows.file_view_candidates()
    native_windows = {hwnd for hwnd, _, _ in native}
    candidates = []
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            space = area.spaces.active
            if area.type == 'FILE_BROWSER' and getattr(space, "browse_mode", 'FILES') == 'FILES' and space.active_operator is not None:
                identity = (window.as_pointer(), area.as_pointer(), space.as_pointer(), space.active_operator.as_pointer())
                # Keep an established mapping while native resize events are
                # still reaching Blender (the attached search bar may resize it).
                known = [hwnd for hwnd, entry in _views.items() if entry['identity'] == identity
                         and hwnd in native_windows and _windows.get_prop(hwnd, PROPERTY) == entry['token']]
                matches = known or [hwnd for hwnd, width, height in native if width == window.width and height == window.height]
                if len(matches) == 1:
                    candidates.append((matches[0], window, area, space))
    valid = {}
    for hwnd, window, area, space in candidates:
        if sum(item[0] == hwnd for item in candidates) != 1:
            continue  # ambiguous overlapping file views: never address the wrong one
        old = _views.get(hwnd)
        identity = (window.as_pointer(), area.as_pointer(), space.as_pointer(), space.active_operator.as_pointer())
        if old is not None and old["identity"] == identity:
            entry = old
        else:
            if old is not None:
                _remove(hwnd, old)
            entry = {"identity": identity, "window": window, "area": area, "space": space,
                     "token": secrets.randbelow(0x7FFFFFFE) + 1}
        operator = space.active_operator
        # WM_operator_properties_filesel defines this property's RNA default
        # from FILE_SAVE. Inspect the default, not the caller's current value.
        properties = operator.properties.bl_rna.properties
        check_existing = properties.get('check_existing')
        entry["mode"] = 2 if check_existing is None or check_existing.default else 1
        if 'filepath' not in properties and 'directory' in properties and 'files' not in properties:
            entry["mode"] = 3
        _windows.set_prop(hwnd, MODE_PROPERTY, entry["mode"])
        _publish_folder(hwnd, entry)
        _windows.set_prop(hwnd, PROPERTY, entry["token"])  # advertise only the initialized view
        valid[hwnd] = entry
    for hwnd, entry in _views.items():
        if hwnd not in valid:
            _remove(hwnd, entry)
    _views.clear()
    _views.update(valid)


def _handle(ticket):
    request = ticket.request
    try:
        hwnd = int(request["window"])
        entry = _views[hwnd]
        if request.get("token") != entry["token"]:
            return {"ok": False, "error": "stale window"}
        method = request.get("method")
        space = entry["space"]
        params = space.params
        if method == "state":
            return {"ok": True, "directory": _folder(space), "filename": params.filename}
        foreground = _windows.foreground()
        if foreground != hwnd:
            _log.debug('Foreground refusal: requested=%s actual=%s', hwnd, foreground)
            return {"ok": False, "error": "window is no longer foreground"}
        if method in {"navigate", "select_file"}:
            path = request.get("path")
            if not isinstance(path, str) or not path or '\0' in path or not os.path.isabs(path):
                return {"ok": False, "error": "invalid path"}
            path = os.path.normpath(path)
            # Existence was checked on the pipe worker. An unavailable network
            # path must never hold up Blender's UI thread.
            if method == "navigate":
                directory, filename = path, params.filename
            else:
                directory, filename = os.path.split(path)
            with bpy.context.temp_override(window=entry["window"], area=entry["area"]):
                params.directory = os.fsencode(directory + os.sep)
                bpy.ops.file.refresh()
                params.filename = filename
                entry["area"].tag_redraw()
            entry["selected"] = path if method == "select_file" else None
            _publish_folder(hwnd, entry)
            return {"ok": True, "directory": _folder(space), "filename": params.filename}
        if method == "accept":
            selected = entry.get("selected")
            if entry["mode"] != 1 or not selected or os.path.normcase(os.path.join(_folder(space), params.filename)) != os.path.normcase(selected):
                return {"ok": False, "error": "no matching file selection"}
            # A cold .blend load can take longer than the pipe deadline. Ack
            # the accepted action first, as PostMessage does for native Open.
            # Recheck the same view, filename, focus and deadline before acting.
            def execute(deadline):
                try:
                    _log.debug('Open dispatch: remaining=%.3f view=%s selection=%s foreground=%s',
                               deadline - time.monotonic(), _views.get(hwnd) is entry,
                               entry.get('selected') == selected, _windows.foreground() == hwnd)
                    if (time.monotonic() <= deadline and _views.get(hwnd) is entry
                            and entry.get("selected") == selected and _windows.foreground() == hwnd
                            and os.path.normcase(os.path.join(_folder(space), params.filename)) == os.path.normcase(selected)):
                        with bpy.context.temp_override(window=entry["window"], area=entry["area"]):
                            result = bpy.ops.file.execute('INVOKE_DEFAULT')
                            _log.debug('Open operator result: %s', result)
                except (ReferenceError, RuntimeError, OSError):
                    _log.warning('Blender could not execute the selected file operation', exc_info=True)
                return None
            ticket.execute = execute  # worker queues it only after the acknowledgement is written
            return {"ok": True}
        return {"ok": False, "error": "unknown operation"}
    except (KeyError, TypeError, ValueError, ReferenceError, RuntimeError, OSError):
        return {"ok": False, "error": "file view is unavailable"}


def _tick():
    if _server is None:
        return None
    if not _server.listening.is_set():
        for hwnd, entry in _views.items():
            _remove(hwnd, entry)
        _views.clear()
        return 0.1
    _sync_views()
    for _ in range(16):
        try:
            action = _server.actions.get_nowait()
        except queue.Empty:
            break
        if time.monotonic() <= action.execution_deadline:
            action.execute(action.execution_deadline)
        else:
            _log.debug('Acknowledged Open expired before Blender could dispatch it')
    for _ in range(16):
        try:
            ticket = _server.pending.get_nowait()
        except queue.Empty:
            break
        response = {"ok": False, "error": "expired"} if time.monotonic() > ticket.deadline else _handle(ticket)
        ticket.reply.put_nowait(response)
    return 0.05


class WinShunPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    def draw(self, context):
        chinese = context.preferences.view.language.startswith('zh')
        self.layout.label(text="在 Win顺中启用文件对话框搜索栏与 Ctrl+G 跳转即可使用。" if chinese else "Enable the file-dialog search bar and Ctrl+G in WinShun.")
        self.layout.label(text="适用于独立的打开、保存和导入窗口。" if chinese else "Works with separate Open, Save and Import file-browser windows.")


def register():
    global _windows, _server
    if os.name != 'nt':
        raise RuntimeError("WinShun integration is available on Windows only")
    _windows = Windows()
    _server = Server(_windows)
    bpy.utils.register_class(WinShunPreferences)
    bpy.app.timers.register(_tick, first_interval=0.05, persistent=True)


def unregister():
    global _server, _windows
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if _server is not None:
        _server.close()
        _server = None
    for hwnd, entry in _views.items():
        _remove(hwnd, entry)
    _views.clear()
    _windows = None
    bpy.utils.unregister_class(WinShunPreferences)
