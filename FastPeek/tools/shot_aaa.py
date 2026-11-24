import ctypes, time, subprocess, sys
from ctypes import wintypes

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
FOLDER = r"D:\我的文件\significant\aaa"

proc = subprocess.Popen([EXE, FOLDER])
time.sleep(3)

# find window
hwnd = None
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
for _ in range(50):
    time.sleep(0.2)
    found = []
    def cb(h, l):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value == proc.pid and user32.IsWindowVisible(h):
            found.append(h)
        return True
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    if found: hwnd = found[0]; break

if not hwnd:
    print("no window"); proc.terminate(); sys.exit(1)

user32.SetForegroundWindow(hwnd)
time.sleep(0.5)

# get rect
class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]
r = RECT()
user32.GetWindowRect(hwnd, ctypes.byref(r))
w = r.right - r.left
h = r.bottom - r.top
print(f"window {w}x{h}")

# capture via PrintWindow
hdc_screen = user32.GetWindowDC(hwnd)
hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
bmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
gdi32.SelectObject(hdc_mem, bmp)

PW_RENDERFULLCONTENT = 2
user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)

# save BMP -> PNG via raw DIB
import struct
class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]
bmi = BITMAPINFOHEADER()
bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
bmi.biWidth = w
bmi.biHeight = -h
bmi.biPlanes = 1
bmi.biBitCount = 32
bmi.biCompression = 0

buf = ctypes.create_string_buffer(w * h * 4)
gdi32.GetDIBits(hdc_mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)

# write BMP file (simplest, then convert)
out = r"C:\Users\dasde\Desktop\FastPeek\tools\aaa_shot.bmp"
with open(out, "wb") as f:
    # BMP header
    filesize = 14 + 40 + w*h*4
    f.write(b"BM")
    f.write(struct.pack("<IHHI", filesize, 0, 0, 54))
    f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, w*h*4, 0, 0, 0, 0))
    f.write(buf.raw)

gdi32.DeleteObject(bmp)
gdi32.DeleteDC(hdc_mem)
user32.ReleaseDC(hwnd, hdc_screen)
print("saved", out)
proc.terminate()
