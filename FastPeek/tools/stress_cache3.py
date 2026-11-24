import ctypes, time, os, glob, subprocess
from ctypes import wintypes

user32 = ctypes.windll.user32
EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
STRESS = r"C:\Users\dasde\Desktop\FastPeek\stress"
THUMBS = r"C:\Users\dasde\AppData\Local\FastPeek\thumbs"

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

def snapshot_mtimes():
    return {os.path.basename(f): os.path.getmtime(f) for f in glob.glob(THUMBS + r"\*.tc")}

def launch_and_scroll():
    proc = subprocess.Popen([EXE, STRESS])
    hwnd = None
    for _ in range(60):
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
        proc.terminate(); return
    scale = 1.0
    try: scale = user32.GetDpiForWindow(hwnd) / 96.0
    except: pass
    def wheel(delta, x=600, y=400):
        lx = int(round(x/scale)); ly = int(round(y/scale))
        lp = (ly << 16) | (lx & 0xFFFF)
        wp = (delta & 0xFFFF) << 16
        user32.PostMessageW(hwnd, 0x020A, wp, lp)
    for i in range(60):
        wheel(-120, 600, 400)
        if i % 10 == 0: time.sleep(0.01)
    time.sleep(2.0)
    proc.terminate(); proc.wait()

# clean + cold
for f in glob.glob(THUMBS + r"\*.tc"): os.remove(f)
launch_and_scroll()
cold = snapshot_mtimes()
print(f"cold: {len(cold)} files")
time.sleep(1.5)  # ensure mtimes are distinct

# warm
launch_and_scroll()
warm = snapshot_mtimes()
print(f"warm: {len(warm)} files")

# count how many files were "touched" (mtime changed) during warm = cache HITS
touched = 0
for name, mt in warm.items():
    if name in cold and mt > cold[name] + 0.5:
        touched += 1
new_files = len(set(warm) - set(cold))
print(f"cache HITS (touched): {touched}")
print(f"new files (re-decode): {new_files}")
if touched > 0:
    print("PASS: disk cache hits confirmed (files touched on read)")
else:
    print("FAIL: no cache hits detected")
