"""Measures what each slider does to the taskbar, without a human looking.

The taskbar is captured with PrintWindow(PW_RENDERFULLCONTENT) and the strip is
compared pixel by pixel against the untouched baseline:

    python tools/verify_taskbar.py             # every slider once
    python tools/verify_taskbar.py --exe path  # measure a different build

Two things about capturing the taskbar on Windows 11 are worth knowing, because
they both look like "the app does nothing":

  * BitBlt from the screen DC does not include the taskbar at all - it is
    composed on its own plane, so a screen copy shows the windows behind it.
  * After the acrylic parameters are touched, DWM's snapshot of the taskbar can
    come back completely black for a moment. That is a capture failure, not a
    black taskbar: the icons in the same snapshot would otherwise still be
    visible. Waiting a few seconds makes it readable again.

The taskbar registry key is restored when the script ends.
"""
import argparse
import ctypes
import json
import os
import struct
import subprocess
import sys
import time
import winreg
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(os.path.dirname(HERE), "build", "bin", "TaskbarTuner.exe")
KEY = r"Software\TaskbarTuner"
BACKUP = os.path.join(HERE, "verify_backup.json")

# Brightness is the value the window shows: 100 = unchanged, 0 = black.
CASES = [
    ("brightness 70", 70, 100, 100),
    ("brightness 40", 40, 100, 100),
    ("brightness 0", 0, 100, 100),
    ("opacity 70", 100, 70, 100),
    ("opacity 40", 100, 40, 100),
    ("icons 40", 100, 100, 40),
    ("all combined", 40, 40, 40),
]

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", ctypes.c_uint32 * 3)]


user32.FindWindowW.restype = ctypes.c_void_p
user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.PrintWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
user32.PrintWindow.restype = ctypes.c_int
user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(RECT)]
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p,
                                ctypes.c_void_p]
user32.GetDC.restype = ctypes.c_void_p
user32.GetDC.argtypes = [ctypes.c_void_p]
user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
gdi32.CreateCompatibleBitmap.restype = ctypes.c_void_p
gdi32.CreateCompatibleBitmap.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.GetDIBits.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                            ctypes.c_uint32, ctypes.c_void_p,
                            ctypes.POINTER(BITMAPINFO), ctypes.c_uint32]
gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
gdi32.DeleteDC.argtypes = [ctypes.c_void_p]


def capture():
    """The taskbar window's own pixels, as (width, height, BGRA bytes)."""
    hwnd = user32.FindWindowW("Shell_TrayWnd", None)
    if not hwnd:
        sys.exit("taskbar window not found")

    rect = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    if width <= 0 or height <= 0:
        sys.exit("taskbar has no size")

    screen = user32.GetDC(None)
    memory = gdi32.CreateCompatibleDC(screen)
    bitmap = gdi32.CreateCompatibleBitmap(screen, width, height)
    gdi32.SelectObject(memory, bitmap)
    if not user32.PrintWindow(hwnd, memory, 2):
        sys.exit("PrintWindow failed")

    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    info.bmiHeader.biHeight = -height
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32

    buffer = ctypes.create_string_buffer(width * height * 4)
    gdi32.GetDIBits(memory, bitmap, 0, height, buffer, ctypes.byref(info), 0)

    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(memory)
    user32.ReleaseDC(None, screen)
    return width, height, bytearray(buffer.raw)


def save_png(path, width, height, bgra):
    raw = bytearray()
    for row in range(height):
        start = row * width * 4
        raw.append(0)
        line = bgra[start:start + width * 4]
        for i in range(0, len(line), 4):
            raw += bytes((line[i + 2], line[i + 1], line[i]))

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        handle.write(chunk(b"IEND", b""))


def luminance(image):
    width, height, data = image
    total_r = total_g = total_b = 0
    for i in range(0, len(data), 4):
        total_b += data[i]
        total_g += data[i + 1]
        total_r += data[i + 2]
    pixels = width * height
    mean_r, mean_g, mean_b = total_r / pixels, total_g / pixels, total_b / pixels
    return 0.299 * mean_r + 0.587 * mean_g + 0.114 * mean_b


def compare(before, after):
    """Mean absolute channel difference and the share of pixels that moved."""
    a, b = before[2], after[2]
    pixels = before[0] * before[1]
    total = 0
    changed = 0
    for i in range(0, len(a), 4):
        delta = abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
        total += delta
        if delta > 24:
            changed += 1
    return total / (pixels * 3), 100.0 * changed / pixels


def snapshot():
    values = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as key:
            index = 0
            while True:
                try:
                    name, value, kind = winreg.EnumValue(key, index)
                except OSError:
                    break
                values[name] = [value, kind]
                index += 1
    except FileNotFoundError:
        pass
    with open(BACKUP, "w", encoding="utf-8") as handle:
        json.dump(values, handle, ensure_ascii=False, indent=1)
    return values


def restore(values):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_ALL_ACCESS) as key:
        index = 0
        while True:
            try:
                name, _, _ = winreg.EnumValue(key, index)
            except OSError:
                break
            if name not in values:
                winreg.DeleteValue(key, name)
                continue
            index += 1
        for name, (value, kind) in values.items():
            winreg.SetValueEx(key, name, 0, kind, value)


def set_settings(brightness, opacity, icons, enabled=True):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_ALL_ACCESS) as key:
        if brightness is not None:
            winreg.SetValueEx(key, "Brightness", 0, winreg.REG_DWORD, brightness)
        if opacity is not None:
            winreg.SetValueEx(key, "Opacity", 0, winreg.REG_DWORD, opacity)
        if icons is not None:
            winreg.SetValueEx(key, "IconOpacity", 0, winreg.REG_DWORD, icons)
        winreg.SetValueEx(key, "Enabled", 0, winreg.REG_DWORD, 1 if enabled else 0)


def close_app(process):
    host = user32.FindWindowW("TaskbarTunerHostWnd", None)
    if host:
        user32.PostMessageW(host, 0x0111, 1006, 0)  # WM_COMMAND, the "exit" id
    for _ in range(24):
        time.sleep(0.25)
        if process.poll() is not None:
            return
    process.terminate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--settle", type=float, default=6.0,
                        help="seconds to let the app apply before measuring")
    args = parser.parse_args()

    if not os.path.exists(args.exe):
        sys.exit(f"not built yet: {args.exe}")

    values = snapshot()

    set_settings(100, 100, 100, enabled=False)
    time.sleep(1.5)
    neutral = capture()
    base = luminance(neutral)
    print(f"neutral baseline: {base:.2f}")
    print()
    print(f"{'setting':18s} {'luminance':>10s} {'delta':>8s} {'changed':>9s}")

    for label, brightness, opacity, icons in CASES:
        set_settings(brightness, opacity, icons)
        process = subprocess.Popen([args.exe], cwd=os.path.dirname(args.exe))
        time.sleep(args.settle)

        image = capture()
        save_png(os.path.join(HERE, "verify_during.png"), *image)
        print(f"{label:18s} {luminance(image):10.2f} {luminance(image) - base:+8.2f} "
              f"{compare(neutral, image)[1]:8.1f}%")

        close_app(process)
        time.sleep(3)

    time.sleep(2)
    restored = capture()
    _, changed = compare(neutral, restored)
    print()
    print(f"after all runs   : {luminance(restored):.2f} "
          f"({luminance(restored) - base:+.2f}), {changed:.1f}% of the pixels differ")

    restore(values)


if __name__ == "__main__":
    main()
