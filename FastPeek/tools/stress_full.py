import ctypes, time, sys, random, subprocess, os
from ctypes import wintypes

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

EXE = r"C:\Users\dasde\Desktop\FastPeek\FastPeek.exe"
STRESS = r"C:\Users\dasde\Desktop\FastPeek\stress"
LOG = r"C:\Users\dasde\Desktop\FastPeek\FastPeek-crash.log"

# clean old log
if os.path.exists(LOG):
    os.remove(LOG)

# launch
proc = subprocess.Popen([EXE, STRESS])
print(f"launched pid={proc.pid}")

# wait for window to appear
hwnd = None
for _ in range(50):
    time.sleep(0.2)
    # enumerate windows of this pid
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_cb(h, l):
        nonlocal_cb = None
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
    print("window never appeared")
    proc.terminate()
    sys.exit(1)

print(f"hwnd=0x{hwnd:X}")

scale = 1.0
try: scale = user32.GetDpiForWindow(hwnd) / 96.0
except Exception: pass

def wheel(delta, x=600, y=400):
    lx = int(round(x/scale)); ly = int(round(y/scale))
    lp = (ly << 16) | (lx & 0xFFFF)
    wp = (delta & 0xFFFF) << 16
    user32.PostMessageW(hwnd, 0x020A, wp, lp)

def key(vk):
    user32.PostMessageW(hwnd, 0x0100, vk, 0)
    user32.PostMessageW(hwnd, 0x0101, vk, 0)

VK_HOME=0x24; VK_END=0x23; VK_PRIOR=0x21; VK_NEXT=0x22
VK_LEFT=0x25; VK_RIGHT=0x27; VK_ESC=0x1B

def click(x, y):
    lx = int(round(x/scale)); ly = int(round(y/scale))
    lp = (ly << 16) | (lx & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)
    user32.PostMessageW(hwnd, 0x0202, 0, lp)

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 1

for r in range(rounds):
    # fling to middle
    for i in range(500):
        wheel(-120, random.randint(300,900), random.randint(200,600))
        if i % 25 == 0: time.sleep(0.002)
    # page jumps
    for _ in range(15):
        key(VK_END); time.sleep(0.02); key(VK_HOME); time.sleep(0.02)
    # enter single view, rapid next
    click(150, 200)
    time.sleep(0.3)
    for i in range(40):
        key(VK_RIGHT if (i//3)%2==0 else VK_LEFT)
        if i % 5 == 0: time.sleep(0.01)
    time.sleep(0.5)
    key(VK_ESC)
    time.sleep(0.3)
    # check alive mid-run
    if proc.poll() is not None:
        print(f"ROUND {r}: process exited code={proc.returncode}")
        break
    print(f"round {r} done, still alive")

time.sleep(1.0)
code = proc.poll()
if code is None:
    print(f"OK alive pid={proc.pid} after {rounds} round(s)")
else:
    print(f"EXITED code={code}")

if os.path.exists(LOG):
    print("--- crash log ---")
    print(open(LOG, encoding='utf-8', errors='replace').read())
else:
    print("no crash log")
