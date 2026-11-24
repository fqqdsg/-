import ctypes, time, os, glob, subprocess, sys
from ctypes import wintypes

user32 = ctypes.windll.user32
EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
FOLDER = r"D:\我的文件\significant\aaa"
THUMBS = r"C:\Users\dasde\AppData\Local\FastPeek\thumbs"

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

def snapshot():
    return {os.path.basename(f): os.path.getmtime(f) for f in glob.glob(THUMBS + r"\*.tc")}

def launch_scroll_wait(waitscan):
    proc = subprocess.Popen([EXE, FOLDER])
    hwnd = None
    for _ in range(100):
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
        proc.terminate(); return None
    time.sleep(waitscan)
    scale = 1.0
    try: scale = user32.GetDpiForWindow(hwnd) / 96.0
    except: pass
    def wheel(delta):
        lx = int(round(600/scale)); ly = int(round(400/scale))
        lp = (ly << 16) | (lx & 0xFFFF)
        wp = (delta & 0xFFFF) << 16
        user32.PostMessageW(hwnd, 0x020A, wp, lp)
    t0 = time.time()
    for i in range(120):
        wheel(-120)
        if i % 10 == 0: time.sleep(0.005)
    time.sleep(2.0)
    dt = time.time() - t0
    proc.terminate(); proc.wait()
    return dt

before = snapshot()
print(f"current cache: {len(before)} files")

print("run 1 (may decode new)...")
t1 = launch_scroll_wait(8)
mid = snapshot()
print(f"  {t1*1000:.0f}ms, cache now {len(mid)} files")
time.sleep(1.5)

print("run 2 (should hit the files run 1 just cached)...")
t2 = launch_scroll_wait(8)
after = snapshot()
print(f"  {t2*1000:.0f}ms, cache now {len(after)} files")

# hits = files that already existed in 'mid' and were touched (mtime bumped) in run 2
touched = sum(1 for n, mt in after.items() if n in mid and mt > mid[n] + 0.5)
newf = len(set(after) - set(mid))
print(f"\ncache HITS (touched on read in run 2): {touched}")
print(f"new files (decoded in run 2): {newf}")
print(f"time: run1={t1*1000:.0f}ms vs run2={t2*1000:.0f}ms")
