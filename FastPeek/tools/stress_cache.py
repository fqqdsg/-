import ctypes, time, sys, os, glob
from ctypes import wintypes

user32 = ctypes.windll.user32

EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
STRESS = r"C:\Users\dasde\Desktop\FastPeek\stress"
THUMBS = r"C:\Users\dasde\AppData\Local\FastPeek\thumbs"
LOG = r"C:\Users\dasde\Desktop\FastPeek\FastPeek-crash.log"

import subprocess
if os.path.exists(LOG):
    os.remove(LOG)

def cache_count():
    return len(glob.glob(THUMBS + r"\*.tc")) if os.path.isdir(THUMBS) else 0

def run_once(label):
    proc = subprocess.Popen([EXE, STRESS])
    hwnd = None
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    for _ in range(50):
        time.sleep(0.15)
        found = []
        def cb(h, l):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == proc.pid and user32.IsWindowVisible(h):
                found.append(h)
            return True
        user32.EnumWindows(WNDENUMPROC(cb), 0)
        if found:
            hwnd = found[0]
            break
    if not hwnd:
        print(f"{label}: no window")
        proc.terminate(); return -1

    scale = 1.0
    try: scale = user32.GetDpiForWindow(hwnd) / 96.0
    except Exception: pass

    def wheel(delta, x=600, y=400):
        lx = int(round(x/scale)); ly = int(round(y/scale))
        lp = (ly << 16) | (lx & 0xFFFF)
        wp = (delta & 0xFFFF) << 16
        user32.PostMessageW(hwnd, 0x020A, wp, lp)

    # scroll through the whole list a few times to force many thumbnails
    t0 = time.time()
    for i in range(1200):
        wheel(-120, 600, 400)
        if i % 20 == 0: time.sleep(0.003)
    time.sleep(1.5)  # let decode catch up
    dt = time.time() - t0

    alive = proc.poll() is None
    proc.terminate(); proc.wait()
    print(f"{label}: scroll+decode took {dt*1000:.0f}ms, alive={alive}")
    return dt

# --- cold ---
for f in glob.glob(THUMBS + r"\*.tc"):
    os.remove(f)
print(f"cold start: cache={cache_count()}")
cold = run_once("COLD")
print(f"after cold: cache={cache_count()}")

# --- warm ---
warm = run_once("WARM")
print(f"after warm: cache={cache_count()}")

if os.path.exists(LOG):
    print("--- crash log ---")
    print(open(LOG, encoding='utf-8', errors='replace').read())
