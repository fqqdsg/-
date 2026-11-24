import ctypes, time, os, glob, subprocess, sys
from ctypes import wintypes

user32 = ctypes.windll.user32
EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
FOLDER = r"D:\我的文件\significant\aaa"
LOG = r"C:\Users\dasde\Desktop\FastPeek\FastPeek-crash.log"
THUMBS = r"C:\Users\dasde\AppData\Local\FastPeek\thumbs"

if os.path.exists(LOG):
    os.remove(LOG)

def cache_count():
    return len(glob.glob(THUMBS + r"\*.tc")) if os.path.isdir(THUMBS) else 0

proc = subprocess.Popen([EXE, FOLDER])
print(f"launched pid={proc.pid}")

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
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
    if found:
        hwnd = found[0]; break

if not hwnd:
    print("window never appeared")
    proc.terminate(); sys.exit(1)
print(f"hwnd=0x{hwnd:X}")

scale = 1.0
try: scale = user32.GetDpiForWindow(hwnd) / 96.0
except: pass

def wheel(delta, x=600, y=400):
    lx = int(round(x/scale)); ly = int(round(y/scale))
    lp = (ly << 16) | (lx & 0xFFFF)
    wp = (delta & 0xFFFF) << 16
    user32.PostMessageW(hwnd, 0x020A, wp, lp)

def key(vk):
    user32.PostMessageW(hwnd, 0x0100, vk, 0)
    user32.PostMessageW(hwnd, 0x0101, vk, 0)

def click(x, y):
    lx = int(round(x/scale)); ly = int(round(y/scale))
    lp = (ly << 16) | (lx & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)
    user32.PostMessageW(hwnd, 0x0202, 0, lp)

VK_HOME=0x24; VK_END=0x23; VK_PRIOR=0x21; VK_NEXT=0x22
VK_LEFT=0x25; VK_RIGHT=0x27; VK_ESC=0x1B; VK_DOWN=0x28; VK_UP=0x26

# wait for scan to finish (watch cache count stabilize or just wait)
print("waiting for scan to complete (large dir)...")
time.sleep(8)

def alive():
    return proc.poll() is None

# --- phase 1: rapid scroll fling to middle ---
print("phase 1: fling scroll to middle")
for i in range(2000):
    wheel(-120, 600, 400)
    if i % 25 == 0: time.sleep(0.002)
    if not alive():
        print("CRASHED during phase 1"); break
time.sleep(2)
print(f"  after fling: alive={alive()}, cache={cache_count()}")

# --- phase 2: page jumps (End/Home/PgDn/PgUp) ---
print("phase 2: page jumps")
for _ in range(30):
    key(VK_END); time.sleep(0.03)
    key(VK_HOME); time.sleep(0.03)
    key(VK_NEXT); time.sleep(0.03)
    key(VK_PRIOR); time.sleep(0.03)
    if not alive(): print("CRASHED during phase 2"); break
time.sleep(2)
print(f"  after jumps: alive={alive()}, cache={cache_count()}")

# --- phase 3: enter single view, rapid prev/next ---
print("phase 3: single view rapid navigation")
click(150, 200)
time.sleep(0.8)
for i in range(80):
    key(VK_RIGHT if (i//3)%2==0 else VK_LEFT)
    if i % 5 == 0: time.sleep(0.015)
    if not alive(): print("CRASHED during phase 3"); break
time.sleep(1)
key(VK_ESC)
time.sleep(0.5)
print(f"  after single: alive={alive()}, cache={cache_count()}")

# --- phase 4: arrow-key grid scroll down (rows) ---
print("phase 4: arrow key scrolling")
for i in range(200):
    key(VK_DOWN)
    if i % 10 == 0: time.sleep(0.01)
time.sleep(2)
print(f"  after arrows: alive={alive()}, cache={cache_count()}")

time.sleep(1.5)
code = proc.poll()
if code is None:
    print(f"\nRESULT: OK - process alive pid={proc.pid}, cache files={cache_count()}")
else:
    print(f"\nRESULT: EXITED code={code}")

if os.path.exists(LOG):
    print("=== crash log ===")
    print(open(LOG, encoding='utf-8', errors='replace').read())
else:
    print("no crash log")
