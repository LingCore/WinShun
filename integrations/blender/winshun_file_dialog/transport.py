# SPDX-FileCopyrightText: 2026 LingCore
# SPDX-License-Identifier: MIT

"""Local, per-process Windows pipe. No bpy calls on the I/O thread."""

import ctypes as C
from ctypes import wintypes as W
import json
import logging
import os
import queue
import struct
import threading
import time

MAX_PACKET = 65536
PROPERTY = "WinShun.Blender.FileBrowser.v1"
FOLDER_PROPERTY = "WinShun.Blender.Folder.v1"
MODE_PROPERTY = "WinShun.Blender.Mode.v1"
_log = logging.getLogger(__name__)


def bind(dll, name, arguments, result):
    function = getattr(dll, name)
    function.argtypes = arguments
    function.restype = result
    return function


class Windows:
    def __init__(self):
        kernel = C.WinDLL("kernel32", use_last_error=True)
        user = C.WinDLL("user32", use_last_error=True)
        advapi = C.WinDLL("advapi32", use_last_error=True)
        pointer = C.c_void_p
        self.close = bind(kernel, "CloseHandle", [W.HANDLE], W.BOOL)
        self.process = bind(kernel, "GetCurrentProcess", [], W.HANDLE)
        self.open_process = bind(kernel, "OpenProcess", [W.DWORD, W.BOOL, W.DWORD], W.HANDLE)
        self.client_pid = bind(kernel, "GetNamedPipeClientProcessId", [W.HANDLE, C.POINTER(W.DWORD)], W.BOOL)
        self.token = bind(advapi, "OpenProcessToken", [W.HANDLE, W.DWORD, C.POINTER(W.HANDLE)], W.BOOL)
        self.token_info = bind(advapi, "GetTokenInformation", [W.HANDLE, W.DWORD, pointer, W.DWORD, C.POINTER(W.DWORD)], W.BOOL)
        self.sid_string = bind(advapi, "ConvertSidToStringSidW", [pointer, C.POINTER(pointer)], W.BOOL)
        self.security = bind(advapi, "ConvertStringSecurityDescriptorToSecurityDescriptorW", [W.LPCWSTR, W.DWORD, C.POINTER(pointer), pointer], W.BOOL)
        self.free = bind(kernel, "LocalFree", [pointer], pointer)
        self.create_pipe = bind(kernel, "CreateNamedPipeW", [W.LPCWSTR, W.DWORD, W.DWORD, W.DWORD, W.DWORD, W.DWORD, W.DWORD, pointer], W.HANDLE)
        self.connect = bind(kernel, "ConnectNamedPipe", [W.HANDLE, pointer], W.BOOL)
        self.peek = bind(kernel, "PeekNamedPipe", [W.HANDLE, pointer, W.DWORD, pointer, C.POINTER(W.DWORD), pointer], W.BOOL)
        self.read = bind(kernel, "ReadFile", [W.HANDLE, pointer, W.DWORD, C.POINTER(W.DWORD), pointer], W.BOOL)
        self.write = bind(kernel, "WriteFile", [W.HANDLE, pointer, W.DWORD, C.POINTER(W.DWORD), pointer], W.BOOL)
        self.open = bind(kernel, "CreateFileW", [W.LPCWSTR, W.DWORD, W.DWORD, pointer, W.DWORD, W.DWORD, W.HANDLE], W.HANDLE)
        self.get_prop = bind(user, "GetPropW", [W.HWND, W.LPCWSTR], W.HANDLE)
        self.set_prop = bind(user, "SetPropW", [W.HWND, W.LPCWSTR, W.HANDLE], W.BOOL)
        self.remove_prop = bind(user, "RemovePropW", [W.HWND, W.LPCWSTR], W.HANDLE)
        self.foreground = bind(user, "GetForegroundWindow", [], W.HWND)
        self.owner = bind(user, "GetWindow", [W.HWND, W.UINT], W.HWND)
        self.pid = bind(user, "GetWindowThreadProcessId", [W.HWND, C.POINTER(W.DWORD)], W.DWORD)
        self.class_name = bind(user, "GetClassNameW", [W.HWND, W.LPWSTR, C.c_int], C.c_int)
        self.client_rect = bind(user, "GetClientRect", [W.HWND, C.POINTER(W.RECT)], W.BOOL)
        self.enum_callback = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        self.enum_windows = bind(user, "EnumWindows", [self.enum_callback, W.LPARAM], W.BOOL)
        self.atom = bind(kernel, "GlobalAddAtomW", [W.LPCWSTR], W.WORD)
        self.delete_atom = bind(kernel, "GlobalDeleteAtom", [W.WORD], W.WORD)

    def _token_sid(self, token, information):
        size = W.DWORD()
        self.token_info(token, information, None, 0, C.byref(size))
        data = C.create_string_buffer(size.value)
        if not self.token_info(token, information, data, size, C.byref(size)):
            raise C.WinError(C.get_last_error())
        sid = C.cast(data, C.POINTER(C.c_void_p))[0]
        text = C.c_void_p()
        if not self.sid_string(sid, C.byref(text)):
            raise C.WinError(C.get_last_error())
        try:
            return C.wstring_at(text)
        finally:
            self.free(text)

    def client_allowed(self, pipe):
        number = W.DWORD()
        if not self.client_pid(pipe, C.byref(number)):
            return False
        process = self.open_process(0x1000, False, number.value)
        if not process:
            return False
        token = W.HANDLE()
        try:
            if not self.token(process, 8, C.byref(token)):
                return False
            # A lower-integrity client must not drive an elevated Blender.
            return int(self._token_sid(token, 25).rsplit('-', 1)[1]) >= self.integrity
        except OSError:
            return False
        finally:
            if token:
                self.close(token)
            self.close(process)

    def security_attributes(self):
        class Attributes(C.Structure):
            _fields_ = [("length", W.DWORD), ("descriptor", C.c_void_p), ("inherit", W.BOOL)]
        token = W.HANDLE()
        if not self.token(self.process(), 8, C.byref(token)):
            raise C.WinError(C.get_last_error())
        try:
            sddl = "D:P(A;;GA;;;SY)(A;;GA;;;" + self._token_sid(token, 1) + ")"
            self.integrity = int(self._token_sid(token, 25).rsplit('-', 1)[1])
        finally:
            self.close(token)
        descriptor = C.c_void_p()
        if not self.security(sddl, 1, C.byref(descriptor), None):
            raise C.WinError(C.get_last_error())
        return Attributes(C.sizeof(Attributes), descriptor, False)

    def file_view_candidates(self):
        result = []
        @self.enum_callback
        def visit(window, _):
            pid = W.DWORD()
            self.pid(window, C.byref(pid))
            if pid.value == os.getpid() and self.owner(window, 4):
                name = C.create_unicode_buffer(64)
                rect = W.RECT()
                if self.class_name(window, name, 64) and name.value == "GHOST_WindowClass" and self.client_rect(window, C.byref(rect)):
                    result.append((int(window), rect.right, rect.bottom))
            return True
        self.enum_windows(visit, 0)
        return result


class Ticket:
    def __init__(self, request):
        self.request = request
        self.deadline = time.monotonic() + 1.5
        self.reply = queue.Queue(maxsize=1)
        self.execute = None
        self.execution_deadline = 0.0


class Server:
    def __init__(self, windows):
        self.windows = windows
        self.name = rf"\\.\pipe\WinShun.Blender.FileDialog.v1.{os.getpid()}"
        self.pending = queue.Queue(maxsize=16)
        self.actions = queue.Queue(maxsize=16)
        self.stopped = threading.Event()
        self.listening = threading.Event()
        self.thread = threading.Thread(target=self._run, name="WinShun file dialog", daemon=True)
        self.thread.start()

    def _read(self, handle, count):
        result = bytearray()
        deadline = time.monotonic() + 1.5
        while len(result) < count and not self.stopped.is_set():
            available = W.DWORD()
            if not self.windows.peek(handle, None, 0, None, C.byref(available), None):
                raise OSError("Pipe disconnected")
            if not available.value:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Incomplete request")
                self.stopped.wait(0.005)
                continue
            size = min(available.value, count - len(result))
            data = C.create_string_buffer(size)
            read = W.DWORD()
            if not self.windows.read(handle, data, size, C.byref(read), None) or not read.value:
                raise OSError("Pipe read failed")
            result.extend(data.raw[:read.value])
        if len(result) != count:
            raise OSError("Stopped")
        return bytes(result)

    def _run(self):
        attributes = None
        try:
            attributes = self.windows.security_attributes()
            while not self.stopped.is_set():
                # One local instance, restricted to this account; reject remote clients.
                handle = self.windows.create_pipe(self.name, 3 | 0x80000, 8, 1, MAX_PACKET + 4, MAX_PACKET + 4, 0, C.byref(attributes))
                if handle == C.c_void_p(-1).value:
                    break
                self.listening.set()
                try:
                    if not self.windows.connect(handle, None) and C.get_last_error() != 535:
                        continue
                    if not self.windows.client_allowed(handle):
                        continue
                    size = struct.unpack("<I", self._read(handle, 4))[0]
                    if not 0 < size <= MAX_PACKET:
                        continue
                    request = json.loads(self._read(handle, size).decode("utf-8"))
                    if not isinstance(request, dict):
                        continue
                    ticket = Ticket(request)
                    if request.get("method") in {"navigate", "select_file"}:
                        path = request.get("path")
                        if not isinstance(path, str) or not path or '\0' in path or not os.path.isabs(path):
                            continue
                        request["path"] = os.path.normpath(path)
                        exists = os.path.isdir if request["method"] == "navigate" else os.path.isfile
                        if not exists(request["path"]) or time.monotonic() > ticket.deadline:
                            continue
                    self.pending.put_nowait(ticket)
                    response = None
                    while not self.stopped.is_set() and time.monotonic() <= ticket.deadline:
                        try:
                            response = ticket.reply.get(timeout=0.02)
                            break
                        except queue.Empty:
                            pass
                    data = json.dumps(response or {"ok": False, "error": "busy"}, ensure_ascii=False).encode("utf-8")
                    if len(data) > MAX_PACKET:
                        data = b'{"ok":false,"error":"response too large"}'
                    packet = struct.pack("<I", len(data)) + data
                    written = W.DWORD()
                    if (self.windows.write(handle, packet, len(packet), C.byref(written), None)
                            and written.value == len(packet) and ticket.execute is not None
                            and time.monotonic() <= ticket.deadline):
                        ticket.execution_deadline = time.monotonic() + 2.0
                        self.actions.put_nowait(ticket)
                        _log.debug('Open acknowledged and queued, remaining=%.3f', ticket.deadline - time.monotonic())
                except (OSError, ValueError, queue.Full):
                    pass
                finally:
                    self.windows.close(handle)
        finally:
            self.listening.clear()
            if attributes is not None:
                self.windows.free(attributes.descriptor)

    def close(self):
        self.stopped.set()
        handle = self.windows.open(self.name, 0xC0000000, 0, None, 3, 0, None)
        if handle != C.c_void_p(-1).value:
            self.windows.close(handle)  # wake a listener blocked in ConnectNamedPipe
        self.thread.join(timeout=0.5)
