import ctypes, time, os, glob, subprocess
from ctypes import wintypes

user32 = ctypes.windll.user32
EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
STRESS = r"C:\Users\dasde\Desktop\FastPeek\stress"
THUMBS = r"C:\Users\dasde\AppData\Local\FastPeek\thumbs"

def cache_count():
    return len(glob.glob(THUMBS + r"\*.tc"))

# clean cache
for f in glob.glob(THUMBS + r"\*.tc"):
    os.remove(f)

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

def launch_and_scroll(fixed):
    proc = subprocess.Popen([EXE, STRESS])
    hwnd = None
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
            hwnd = found[0]; break
    if not hwnd:
        proc.terminate(); return None
    scale = 1.0
    try: scale = user32.GetDpiForWindow(hwnd) / 96.0
    except: pass
    def wheel(delta, x=600, y=400):
        lx = int(round(x/scale)); ly = int(round(y/scale))
        lp = (ly << 16) | (lx & 0xFFFF)
        wp = (delta & 0xFFFF) << 16
        user32.PostMessageW(hwnd, 0x020A, wp, lp)
    # scroll a FIXED amount to the top area only (first screen)
    for i in range(60):
        wheel(-120, 600, 400)
        if i % 10 == 0: time.sleep(0.01)
    time.sleep(1.5)
    proc.terminate(); proc.wait()
    return True

print("=== COLD: scroll to top (decode + write cache) ===")
launch_and_scroll(True)
c1 = cache_count()
print(f"cache files after cold: {c1}")

print("=== WARM: same scroll (should be pure cache hit) ===")
launch_and_scroll(True)
c2 = cache_count()
print(f"cache files after warm: {c2} (should equal {c1} if no re-decode)")

if c2 == c1:
    print("PASS: warm run did not re-decode any new thumbnails -> cache hits work")
else:
    print(f"FAIL: warm added {c2-c1} new files -> some cache misses occurred")
