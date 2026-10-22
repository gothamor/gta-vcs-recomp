"""Find and drive the game window (tools/screenshot.py, tools/sendkey.py). Windows only; standard library only."""
import ctypes
import platform
import struct
import sys
import time
import zlib

DEFAULT_TITLE = "GTA: Vice City Stories"


def require_windows():
    if platform.system() != "Windows":
        sys.exit("error: window automation is only implemented for Windows")


def _user32():
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.FindWindowW.restype = wintypes.HWND
    return user32


def find_window(title=DEFAULT_TITLE):
    require_windows()
    hwnd = _user32().FindWindowW(None, title)
    if not hwnd:
        sys.exit(f"error: no window titled '{title}' (is the game running?)")
    return hwnd


def capture_png(hwnd, path):
    """Saves the window (including GPU-composited content) as a PNG. Returns (width, height)."""
    from ctypes import wintypes
    user32 = _user32()
    gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
    try:
        user32.SetProcessDPIAware()
    except AttributeError:
        pass
    # 64-bit handles: every signature spelled out (the int default truncates them)
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.GetWindowDC.argtypes = [wintypes.HWND]
    user32.GetWindowDC.restype = wintypes.HDC
    user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi32.CreateCompatibleDC.restype = wintypes.HDC
    gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
    gdi32.SelectObject.restype = wintypes.HGDIOBJ
    gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p,
                                wintypes.UINT]
    gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
    gdi32.DeleteDC.argtypes = [wintypes.HDC]
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    wdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(wdc)
    bmp = gdi32.CreateCompatibleBitmap(wdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32), ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32), ("biSizeImage", ctypes.c_uint32),
                    ("biXPelsPerMeter", ctypes.c_int32), ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)  # top-down BGRA
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(info), 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, wdc)

    raw = bytearray()
    data = buf.raw
    for y in range(h):
        row = data[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        raw += b"\x00" + rgb

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    return w, h


# Virtual-key codes for the names the game's controls use (docs/CONTROLS.md); letters / digits / F1-F24 work too.
_VK = {"return": 0x0D, "enter": 0x0D, "shiftkey": 0x10, "shift": 0x10, "controlkey": 0x11, "ctrl": 0x11, "space": 0x20,
       "escape": 0x1B, "esc": 0x1B, "back": 0x08, "backspace": 0x08, "tab": 0x09, "left": 0x25, "up": 0x26, "right": 0x27,
       "down": 0x28}


def vk_code(name):
    key = name.lower()
    if key in _VK:
        return _VK[key]
    if len(name) == 1 and name.isalnum():
        return ord(name.upper())
    if key.startswith("f") and key[1:].isdigit() and 1 <= int(key[1:]) <= 24:
        return 0x70 + int(key[1:]) - 1
    sys.exit(f"error: unknown key '{name}'")


def press(hwnd, name, hold_ms=150, times=1):
    """Brings the window to the front and holds the key `times` times."""
    from ctypes import wintypes
    user32 = _user32()
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.1)
    vk = vk_code(name)
    scan = user32.MapVirtualKeyW(vk, 0)
    for _ in range(times):
        user32.keybd_event(vk, scan, 0, 0)
        time.sleep(hold_ms / 1000.0)
        user32.keybd_event(vk, scan, 2, 0)  # KEYEVENTF_KEYUP
        time.sleep(0.25)
