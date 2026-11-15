"""A topmost popup of another process that takes no activation, for N seconds.

  python cover.py x y w h seconds
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

x, y, w, h = (int(v) for v in sys.argv[1:5])
seconds = float(sys.argv[5])
user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32')
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t


class WNDCLASSW(ctypes.Structure):
    _fields_ = [('style', wt.UINT), ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
                ('hInstance', wt.HINSTANCE), ('hIcon', wt.HICON), ('hCursor', ctypes.c_void_p), ('hbrBackground', ctypes.c_void_p),
                ('lpszMenuName', wt.LPCWSTR), ('lpszClassName', wt.LPCWSTR)]


proc = WNDPROC(lambda hwnd, msg, wp, lp: user32.DefWindowProcW(hwnd, msg, wp, lp))
wc = WNDCLASSW()
wc.lpfnWndProc = proc
wc.hInstance = kernel32.GetModuleHandleW(None)
wc.hCursor = user32.LoadCursorW(None, ctypes.c_void_p(32512))  # IDC_ARROW
wc.hbrBackground = ctypes.c_void_p(2)  # COLOR_ACTIVECAPTION + 1
wc.lpszClassName = 'WsCoverProbe'
user32.RegisterClassW(ctypes.byref(wc))
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
hwnd = user32.CreateWindowExW(0x8 | 0x80 | 0x08000000, 'WsCoverProbe', 'cover', 0x80000000, x, y, w, h,
                              None, None, wc.hInstance, None)  # TOPMOST | TOOLWINDOW | NOACTIVATE, WS_POPUP
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
user32.SetWindowPos(hwnd, wt.HWND(-1), x, y, w, h, 0x0010 | 0x0040)  # HWND_TOPMOST, NOACTIVATE | SHOWWINDOW
msg = wt.MSG()
end = time.time() + seconds
while time.time() < end:
    while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))
    time.sleep(0.01)
user32.DestroyWindow(hwnd)
