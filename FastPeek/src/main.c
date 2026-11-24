/* FastPeek - window, message loop and command dispatch. */
#include "app.h"
#include <stdarg.h>
#include <dbghelp.h>

/* -------------------------------------------------------- crash capture ---
 * When the process dies we have to know *where*, not guess.  A handler writes
 * a minidump (FastPeek-crash.dmp, beside the log) before the OS tears the
 * process down; load the dump in a debugger and the faulting frame is exact.
 * Both files go to %LocalAppData%\FastPeek, never next to the exe: FastPeek is
 * normally run from a read-only USB stick.  dbghelp is loaded lazily so a
 * machine without it still starts. */
typedef BOOL(WINAPI *PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, DWORD,
                                            PVOID, PVOID, PVOID);
typedef BOOL(WINAPI *PFN_SymInitialize)(HANDLE, PCSTR, BOOL);
typedef BOOL(WINAPI *PFN_SymCleanup)(HANDLE);
typedef USHORT(WINAPI *PFN_RtlCaptureStackBackTrace)(ULONG, ULONG, PVOID *, PULONG);

/* Walk the stack with RtlCaptureStackBackTrace (kernel32, always present) so the
 * crash log names the faulting function even without a minidump.
 *
 * The whole walk runs inside the crash filter, where the heap may already be
 * corrupted and any non-trivial API (dbghelp's SymInitialize allocates, and
 * GetModuleHandleExW can take the loader lock) can throw a *second* fault and
 * clobber the very report we are trying to write.  That second fault is what
 * the previous logs recorded - the top frames were this function, not the real
 * crash site.  So: no symbols, no loader queries, just the raw frame addresses
 * plus the exception record, both of which need nothing but kernel32. */
static void CrashLogStack(FILE *lf, PEXCEPTION_POINTERS ep)
{
    /* The exception record is the single most valuable datum and needs no
     * allocation at all - write it first, before anything can go wrong. */
    if (ep && ep->ExceptionRecord) {
        EXCEPTION_RECORD *r = ep->ExceptionRecord;
        fprintf(lf, "exception code=0x%08X addr=0x%p tid=%lu\n",
                (unsigned)r->ExceptionCode, r->ExceptionAddress,
                (unsigned long)GetCurrentThreadId());
#ifdef _M_X64
        if (ep->ContextRecord)
            fprintf(lf, "  rip=0x%p rsp=0x%p rbp=0x%p\n",
                    (void *)ep->ContextRecord->Rip,
                    (void *)ep->ContextRecord->Rsp,
                    (void *)ep->ContextRecord->Rbp);
#endif
        /* the file being decoded when a codec crashes on an MF worker thread */
        const WCHAR *dec = VideoDecodingPath();
        if (dec && dec[0])
            fprintf(lf, "  decoding=%ls\n", dec);
    }

    PVOID frames[32];
    memset(frames, 0, sizeof(frames));
    ULONG hash = 0;
    ULONG n = 0;

    __try {
        PFN_RtlCaptureStackBackTrace backtrace =
            (PFN_RtlCaptureStackBackTrace)(void *)GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "RtlCaptureStackBackTrace");
        if (backtrace) n = backtrace(0, 32, frames, &hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;   /* a fault while faulting: keep only the exception record */
    }

    for (ULONG i = 0; i < n; i++) {
        /* Best-effort module + RVA.  GetModuleHandleEx can take the loader
         * lock, which is exactly the kind of call that deadlocks or faults
         * inside a crash handler on a corrupted heap, so guard it. */
        char mod[64] = "?";
        ULONG_PTR rva = 0;
        __try {
            HMODULE hm = NULL;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)frames[i], &hm) && hm) {
                WCHAR path[MAX_PATH_LONG];
                DWORD m = GetModuleFileNameW(hm, path, MAX_PATH_LONG);
                if (m) {
                    WCHAR *name = PathFileName(path);
                    WideCharToMultiByte(CP_ACP, 0, name, -1, mod, sizeof(mod), NULL, NULL);
                }
                rva = (ULONG_PTR)frames[i] - (ULONG_PTR)hm;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        fprintf(lf, "  #%02lu 0x%p %s+0x%llX\n", (unsigned long)i, frames[i],
                mod, (unsigned long long)rva);
    }
    fflush(lf);
}

static LONG WINAPI CrashFilter(PEXCEPTION_POINTERS ep)
{
    /* First: a plain-text note that needs nothing but the CRT, so even if the
     * heap is mangled or dbghelp cannot load we still learn the exception.
     * The cache/log directory is %LocalAppData%\FastPeek (create it on demand);
     * the exe folder is only a last resort when there is no profile at all. */
    WCHAR base[MAX_PATH_LONG];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH_LONG);
    if (n && n < MAX_PATH_LONG) {
        WCHAR sub[MAX_PATH_LONG];
        PathJoin(sub, _countof(sub), base, L"FastPeek");
        CreateDirectoryW(sub, NULL);
        wcsncpy_s(base, _countof(base), sub, _TRUNCATE);
    } else {
        n = GetModuleFileNameW(NULL, base, MAX_PATH_LONG);
        if (n) {
            for (int i = (int)n - 1; i >= 0; i--)
                if (base[i] == L'\\') { base[i + 1] = 0; break; }
        } else {
            base[0] = 0;
        }
    }
    WCHAR logPath[MAX_PATH_LONG];
    wcsncpy_s(logPath, MAX_PATH_LONG, base, _TRUNCATE);
    wcsncat_s(logPath, MAX_PATH_LONG, L"FastPeek-crash.log", _TRUNCATE);

    FILE *lf = NULL;
    if (_wfopen_s(&lf, logPath, L"a") == 0 && lf) {
        CrashLogStack(lf, ep);
        fclose(lf);
    }

    /* Then try for a minidump (best effort).  Full-memory dumps of a process
     * that has just leaked gigabytes are slow and huge (hundreds of MB); a
     * normal dump keeps the faulting thread, its stack and the exception and is
     * a fraction of the size while still identifying the crash site. */
    HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_MiniDumpWriteDump writeDump =
        dbg ? (PFN_MiniDumpWriteDump)(void *)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    if (writeDump) {
        WCHAR dumpPath[MAX_PATH_LONG];
        wcsncpy_s(dumpPath, MAX_PATH_LONG, base, _TRUNCATE);
        wcsncat_s(dumpPath, MAX_PATH_LONG, L"FastPeek-crash.dmp", _TRUNCATE);
        HANDLE f = CreateFileW(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
            writeDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MiniDumpNormal, ep ? &mei : NULL, NULL, NULL);
            CloseHandle(f);
        }
    }
    if (dbg) FreeLibrary(dbg);
    return EXCEPTION_EXECUTE_HANDLER;   /* let the OS finish the crash normally */
}

static void CrashInstall(void)
{
    SetUnhandledExceptionFilter(CrashFilter);
}

/* --bench logs timings to %TEMP%\fastpeek-bench.txt so the startup claim in
 * the readme can be re-measured on any machine. */
static BOOL      s_bench;
static ULONGLONG s_t0;
static BOOL      s_firstFrame;

void BenchLog(const char *fmt, ...)
{
    if (!s_bench) return;
    WCHAR dir[MAX_PATH_LONG], path[MAX_PATH_LONG];
    DWORD n = GetEnvironmentVariableW(L"TEMP", dir, MAX_PATH_LONG);
    if (!n || n >= MAX_PATH_LONG) return;
    PathJoin(path, _countof(path), dir, L"fastpeek-bench.txt");

    FILE *out = NULL;
    if (_wfopen_s(&out, path, L"a") != 0 || !out) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fclose(out);
}

/* TEMP scroll benchmark: flings the grid on a timer and reports how the ui
 * thread coped, so "scrolling 4000 folder cards stalls" becomes a number. */
static BOOL      s_scrollBench;
static BOOL      s_sbSlow;
static int       s_sbStep;
static ULONGLONG s_sbT0;
static int       s_sbDec0;
static int       s_sbReq0;

static void ScrollBenchTick(void)
{
    int limit = s_sbSlow ? 200 : 600;
    if (s_sbStep == limit) {        /* fling done: let the covers fill in */
        s_sbStep++;
        KillTimer(g_app.hwnd, IDT_SCROLLBENCH);
        SetTimer(g_app.hwnd, IDT_SCROLLBENCH, 2500, NULL);
        return;
    }
    if (s_sbStep > limit || (g_app.scrollY == 0 && s_sbStep > 10)) {
        KillTimer(g_app.hwnd, IDT_SCROLLBENCH);
        BenchLog("scroll wall=%llu ms paints=%d avg=%llu worst=%llu decodes=%d scrollY=%d",
                 GetTickCount64() - s_sbT0, PaintStatsCount(), PaintStatsAvg(),
                 PaintStatsWorst(), g_thumbDecodeN - s_sbDec0, g_app.scrollY);
        PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    s_sbStep++;
    GridScroll(s_sbSlow ? 120 : 240);   /* one notch, or two: a fling */
}

static int s_thumbGen = 1;

int ThumbGeneration(void) { return s_thumbGen; }

static void ThumbsClear(void)
{
    for (int i = 0; i < THUMB_MAX; i++) {
        if (g_app.thumbs[i].bits) free(g_app.thumbs[i].bits);
        g_app.thumbs[i].bits = NULL;
        g_app.thumbs[i].item = -1;
        g_app.thumbs[i].w = g_app.thumbs[i].h = 0;
    }
    g_app.thumbBytes = 0;      /* the running total must not survive a reload */
    s_thumbGen++;              /* in-flight video jobs belong to the old folder */
}

void AppInvalidate(void)
{
    if (g_app.hwnd) InvalidateRect(g_app.hwnd, NULL, FALSE);
}

static void UpdateTitle(void)
{
    WCHAR title[MAX_PATH_LONG + 64];
    const WCHAR *dir = g_app.curDir[0] ? g_app.curDir : g_app.lib.root;
    int n = g_app.folderMode ? FolderCells() : g_app.viewCount;
    if (dir[0])
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s  —  %s (%d)",
                     APP_TITLE, dir, n);
    else
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s", APP_TITLE);
    SetWindowTextW(g_app.hwnd, title);
}

/* The item the UI currently points at.  The viewer follows cur; the grid
 * follows sel, which moves with the arrow keys while cur keeps the last
 * opened file until the grid selection is opened again. */
static int CurrentIdx(void)
{
    if (g_app.single && g_app.cur >= 0) return g_app.cur;
    if (g_app.sel >= 0) return g_app.sel;
    return g_app.cur;
}

void AppUpdateInfo(void)
{
    Library *lib = &g_app.lib;
    int n = g_app.viewCount;
    g_app.info[0] = 0;

    if (g_app.folderMode) {
        _snwprintf_s(g_app.info, _countof(g_app.info), _TRUNCATE,
                     L"%d 个子文件夹 · %d 个文件%s", g_app.folderCount, n,
                     g_app.scanning ? L" · 扫描中…" : L"");
        return;
    }

    if (n == 0) {
        _snwprintf_s(g_app.info, _countof(g_app.info), _TRUNCATE, L"%s",
                     g_app.scanning ? L"扫描中…" : L"0 项");
        return;
    }
    EnterCriticalSection(&lib->lock);
    int idx = CurrentIdx();
    if (idx < 0 || idx >= lib->count) {
        _snwprintf_s(g_app.info, _countof(g_app.info), _TRUNCATE,
                     L"%d 项%s", n, g_app.scanning ? L" · 扫描中…" : L"");
        LeaveCriticalSection(&lib->lock);
        return;
    }
    int pos = FilterPos(idx) + 1;      /* 1-based rank in the filtered set */
    MediaItem *it = &lib->items[idx];
    WCHAR size[32];
    FormatSize(it->size, size, _countof(size));

    if (it->kind == MK_VIDEO) {
        _snwprintf_s(g_app.info, _countof(g_app.info), _TRUNCATE,
                     L"%d / %d · 视频 · %s%s", pos, n, size,
                     g_app.scanning ? L" · 扫描中…" : L"");
    } else if (g_app.single && g_app.img.bits && g_app.img.item == idx) {
        _snwprintf_s(g_app.info, _countof(g_app.info), _TRUNCATE,
                     L"%d / %d · %d×%d · %s · %d%%", pos, n,
                     g_app.img.w, g_app.img.h, size, (int)(g_app.zoom * 100.0 + 0.5));
    } else {
        _snwprintf_s(g_app.info, _countof(g_app.info), _TRUNCATE,
                     L"%d / %d · %s%s", pos, n, size,
                     g_app.scanning ? L" · 扫描中…" : L"");
    }
    LeaveCriticalSection(&lib->lock);
}

BOOL ItemPathOf(int index, WCHAR *out, size_t cap)
{
    Library *lib = &g_app.lib;
    if (!out || cap == 0) return FALSE;
    out[0] = 0;
    EnterCriticalSection(&lib->lock);
    if (index < 0 || index >= lib->count) {
        LeaveCriticalSection(&lib->lock);
        return FALSE;
    }
    wcsncpy_s(out, cap, lib->items[index].path, _TRUNCATE);
    LeaveCriticalSection(&lib->lock);
    return out[0] != 0;
}

/* ------------------------------------------------------------- slideshow ---
 * A timer that walks the filtered list.  It leaves full screen and the single
 * view exactly the way a user would: through the same AppGoto path, so
 * preloading, the seek bar and the video player all stay in sync.  Hitting a
 * video ends the slideshow there - a clip has its own duration and the timer
 * must not cut it off. */
static BOOL s_slidePaused;
static void ToggleFullscreen(void);

static BOOL ItemIsVideo(int index)
{
    Library *lib = &g_app.lib;
    BOOL v = FALSE;
    EnterCriticalSection(&lib->lock);
    if (index >= 0 && index < lib->count)
        v = lib->items[index].kind == MK_VIDEO;
    LeaveCriticalSection(&lib->lock);
    return v;
}

static BOOL CurrentIsVideo(void)
{
    return g_app.single && ItemIsVideo(g_app.cur);
}

static void StopSlideshow(BOOL announce)
{
    s_slidePaused = FALSE;
    if (!g_app.slideshow) return;
    g_app.slideshow = FALSE;
    if (g_app.hwnd) KillTimer(g_app.hwnd, IDT_SLIDESHOW);
    if (announce) SetStatus(L"幻灯片已停止");
    ChromeUpdateButtons();
    AppInvalidate();
}

static void PauseSlideshow(void)
{
    if (!g_app.slideshow) return;
    g_app.slideshow = FALSE;
    s_slidePaused = TRUE;
    if (g_app.hwnd) KillTimer(g_app.hwnd, IDT_SLIDESHOW);
    SetStatus(L"幻灯片已暂停 · 空格继续 · Esc 退出");
    ChromeUpdateButtons();
    AppInvalidate();
}

static void StartSlideshow(void)
{
    if (!g_app.single) {
        AppSetSingle(TRUE);
        if (!g_app.single) return;
    }
    if (g_app.viewCount == 0) return;
    if (CurrentIsVideo()) {
        SetStatus(L"幻灯片从图片开始（当前是视频）");
        return;
    }
    if (!g_app.fullscreen) ToggleFullscreen();
    g_app.slideshow = TRUE;
    s_slidePaused = FALSE;
    SetTimer(g_app.hwnd, IDT_SLIDESHOW, (UINT)g_app.slideSec * 1000, NULL);
    SetStatus(L"幻灯片播放中 · 每 %d 秒切换 · 空格暂停 · Esc 退出", g_app.slideSec);
    ChromeUpdateButtons();
    AppInvalidate();
}

static void ToggleSlideshow(void)
{
    if (g_app.slideshow) StopSlideshow(TRUE);
    else StartSlideshow();
}

static void SlideAdvance(void)
{
    if (!g_app.slideshow) return;
    if (!g_app.single) { StopSlideshow(FALSE); return; }

    int next = FilterStep(g_app.cur, 1);
    if (next < 0) {
        StopSlideshow(FALSE);
        SetStatus(L"幻灯片结束 · 已到最后一张");
        return;
    }
    BOOL video = ItemIsVideo(next);
    if (video) StopSlideshow(FALSE);
    AppGoto(next, TRUE);
    if (video) SetStatus(L"幻灯片结束 · 下一项是视频");
}

static void SlideIntervalStep(int delta)
{
    int v = g_app.slideSec + delta;
    if (v < 1) v = 1;
    if (v > 60) v = 60;
    if (v == g_app.slideSec) return;
    g_app.slideSec = v;
    if (g_app.slideshow)
        SetTimer(g_app.hwnd, IDT_SLIDESHOW, (UINT)g_app.slideSec * 1000, NULL);
    SetStatus(L"幻灯片间隔 %d 秒", g_app.slideSec);
    AppInvalidate();
}

/* ---------------------------------------------------------------- actions */

static void SearchReset(void)
{
    g_app.search[0] = 0;
    g_app.searchActive = FALSE;
}

void AppOpenFolder(const WCHAR *folder)
{
    WCHAR path[MAX_PATH_LONG];
    wcsncpy_s(path, MAX_PATH_LONG, folder, _TRUNCATE);
    PathNormalize(path);
    if (!path[0] || !PathIsDir(path)) {
        SetStatus(L"目录不存在：%s", path);
        return;
    }

    /* a different root makes every remembered place meaningless; a rescan of
     * the same one (F5) keeps them, so going back out still lands where the
     * user was.  Read the old root *before* ScanStart overwrites it. */
    BOOL newRoot = _wcsicmp(g_app.lib.root, path) != 0;

    if (!ScanStart(path)) {
        /* A previous walker is wedged, and starting a second one would free
         * the array it is still appending to.  Keep showing what we have. */
        SetStatus(L"上一个目录仍在扫描，无法切换到 %s", path);
        return;
    }
    if (newRoot) DirMarksClear();

    StopSlideshow(FALSE);
    SearchReset();
    VideoStop();
    ImageFree();
    ImagePreloadClear();
    ThumbsClear();

    g_app.single = FALSE;
    g_app.cur = -1;
    g_app.sel = -1;
    g_app.hover = -1;
    g_app.scrollY = 0;
    g_app.panX = g_app.panY = 0;
    g_app.zoom = 1.0;
    g_app.fit = TRUE;
    g_app.imgFailed = 0;

    /* the folder browser starts at the new root.  Whether it shows cards is
     * decided when the walk finishes, because only then do we know whether
     * there is anything below to show. */
    wcsncpy_s(g_app.curDir, MAX_PATH_LONG, path, _TRUNCATE);
    g_app.folderAuto = 1;
    g_app.folderMode = 0;
    g_app.selFolder = g_app.hoverFolder = -1;
    FoldersFree();
    g_app.folderFiles = 0;
    g_app.fileNPreview = 0;
    g_app.folderAt = 0;

    PaintStatsReset();
    FilterRebuild();     /* items were just dropped: empty the view, mid-scan shows
                            "scanning" instead of stale indices into freed memory */
    g_app.scanning = TRUE;
    SetStatus(L"正在扫描 %s …", path);

    ViewLayout();
    UpdateTitle();
    AppUpdateInfo();
    ChromeUpdateButtons();
    AppInvalidate();
}

void AppOpenPath(const WCHAR *raw)
{
    WCHAR path[MAX_PATH_LONG];
    wcsncpy_s(path, MAX_PATH_LONG, raw, _TRUNCATE);
    PathNormalize(path);
    if (!path[0]) return;

    if (PathIsDir(path)) {
        g_app.pendingFile[0] = 0;
        AppOpenFolder(path);
        return;
    }

    WCHAR parent[MAX_PATH_LONG];
    wcsncpy_s(parent, MAX_PATH_LONG, path, _TRUNCATE);
    PathParent(parent);
    wcsncpy_s(g_app.pendingFile, MAX_PATH_LONG, path, _TRUNCATE);
    AppOpenFolder(parent);
    /* one file was asked for: show that folder's pictures, not its albums */
    g_app.folderAuto = 0;
    g_app.folderMode = 0;
}

static void TryPendingJump(void)
{
    if (!g_app.pendingFile[0] || g_app.lib.count == 0) return;
    for (int i = 0; i < g_app.lib.count; i++) {
        if (CompareStringOrdinal(g_app.lib.items[i].path, -1, g_app.pendingFile, -1, TRUE) == CSTR_EQUAL) {
            g_app.pendingFile[0] = 0;
            g_app.sel = i;
            g_app.cur = i;
            AppSetSingle(TRUE);
            return;
        }
    }
    if (!g_app.lib.scanning) g_app.pendingFile[0] = 0;
}

void AppGoto(int index, BOOL toast)
{
    Library *lib = &g_app.lib;
    EnterCriticalSection(&lib->lock);
    if (lib->count == 0) { LeaveCriticalSection(&lib->lock); return; }
    if (index < 0) index = 0;
    if (index >= lib->count) index = lib->count - 1;

    g_app.cur = index;
    g_app.sel = index;
    g_app.imgFailed = 0;
    g_app.panX = g_app.panY = 0;
    g_app.fit = TRUE;

    if (g_app.single) {
        MediaItem *it = &lib->items[index];
        if (it->kind == MK_VIDEO) {
            if (g_app.slideshow) StopSlideshow(FALSE);   /* a clip ends the tour */
            ImageFree();
            ImagePreloadClear();
            if (!VideoPlay(index)) {
                SetStatus(L"无法播放 · 按 E 用系统播放器打开");
            } else {
                /* the status strip carries the state while the seek bar owns
                 * the right half, so lead with the state, not the file name */
                SetStatus(L"播放中（空格暂停）· %s", it->name);
            }
        } else {
            VideoStop();
            if (!ImageLoad(index))
                SetStatus(L"无法解码这个文件：%s", it->name);
            else
                SetStatus(L"%s", it->name);
        }
    } else {
        EnsureVisible(index);
    }

    /* a manual jump restarts the countdown instead of cutting the new slide
     * short by whatever was left on the old one */
    if (g_app.slideshow && g_app.single)
        SetTimer(g_app.hwnd, IDT_SLIDESHOW, (UINT)g_app.slideSec * 1000, NULL);

    if (toast) UpdateTitle();
    AppUpdateInfo();
    ChromeUpdateButtons();
    AppInvalidate();
    LeaveCriticalSection(&lib->lock);
}

void AppNext(int delta)
{
    Library *lib = &g_app.lib;
    EnterCriticalSection(&lib->lock);
    if (lib->count == 0 || g_app.viewCount == 0) { LeaveCriticalSection(&lib->lock); return; }
    LeaveCriticalSection(&lib->lock);

    if (g_app.single) {
        int next = FilterStep(g_app.cur, delta);
        if (next < 0) next = delta > 0 ? g_app.viewMap[0]
                                       : g_app.viewMap[g_app.viewCount - 1];
        AppGoto(next, TRUE);
        return;
    }

    /* grid: move through the filtered set; delta is already expressed in
     * cells (1 for left/right, cols for up/down), so just add it */
    int vi = FilterPos(g_app.sel);
    if (vi < 0) vi = 0;
    else vi += delta;
    if (vi < 0) vi = 0;
    if (vi >= g_app.viewCount) vi = g_app.viewCount - 1;
    g_app.sel = g_app.viewMap[vi];
    EnsureVisible(g_app.sel);
    AppUpdateInfo();
    AppInvalidate();
}

/* the hint line the grid shows when nothing else has happened yet */
static void GridStatus(void)
{
    /* The walker stops at SCAN_MAX_DEPTH, so a folder at the cap has its own
     * sub-folders unindexed.  Saying so is the difference between "this folder
     * really is empty" and "the scan deliberately stops here" - O then opens
     * the picker in this very folder, and whatever you pick becomes the root,
     * with two more levels attached. */
    WCHAR cap[48] = L"";
    if (PathDepthUnder(g_app.lib.root, g_app.curDir) >= SCAN_MAX_DEPTH)
        _snwprintf_s(cap, _countof(cap), _TRUNCATE, L" · 只扫到 %d 层", SCAN_MAX_DEPTH);

    if (g_app.search[0] || g_app.searchActive) {
        const WCHAR *what = g_app.search[0] ? g_app.search : L"输入文件名…";
        if (g_app.folderMode)
            SetStatus(L"搜索「%s」· %d 个子文件夹 · %d 个文件 · Esc 清除%s",
                      what, g_app.folderCount, g_app.folderFiles, cap);
        else
            SetStatus(L"搜索「%s」· %d 项匹配 · Ctrl+F 编辑 · Esc 清除%s",
                      what, g_app.viewCount, cap);
        return;
    }

    if (g_app.folderMode) {
        SetStatus(L"%d 项 · 单击进入子文件夹，右键返回，Esc 上一级%s",
                  FolderCells(), cap);
        return;
    }
    if (g_app.viewCount)
        SetStatus(L"%d 项 · 点缩略图看大图，← → 切换，F11 全屏%s",
                  g_app.viewCount, cap);
    else if (g_app.filter)
        SetStatus(L"没有符合筛选条件的文件%s", cap);
    else if (g_app.curDir[0] && _wcsicmp(g_app.curDir, g_app.lib.root) != 0)
        SetStatus(L"这个子文件夹里没有图片或视频 · 右键返回%s", cap);
    else
        SetStatus(L"这个文件夹里没有图片或视频%s", cap);
}

void AppSetSingle(BOOL on)
{
    if (on == g_app.single) return;
    if (on && g_app.viewCount == 0) return;

    if (on) {
        /* a card grid has no "current picture"; the file grid it flips to may
         * have a remembered selection, but opening *that* instead of the first
         * tile would be a surprise, so start at the top when coming from cards */
        BOOL fromCards = g_app.folderMode;
        if (fromCards) AppSetFolderMode(0);   /* cards are not pictures */
        g_app.single = TRUE;
        g_app.singleAt = GetTickCount64();
        g_app.hover = -1;
        g_app.navHot = -1;
        int idx = fromCards ? -1 : (g_app.sel >= 0 ? g_app.sel : g_app.cur);
        /* the selection may point outside the current directory (or be the
         * leftover of a filter), so fall back to the first visible item */
        if (idx < 0 || FilterPos(idx) < 0)
            idx = g_app.viewCount ? g_app.viewMap[0] : -1;
        if (idx < 0) { g_app.single = FALSE; return; }
        g_app.cur = idx;
        AppGoto(idx, TRUE);
    } else {
        StopSlideshow(FALSE);
        ImagePreloadClear();
        g_app.single = FALSE;
        g_app.navHot = -1;
        VideoStop();
        g_app.zoom = 1.0;
        g_app.panX = g_app.panY = 0;
        g_app.fit = TRUE;
        ImageFree();
        if (g_app.cur >= 0) g_app.sel = g_app.cur;
        EnsureVisible(g_app.sel);
        /* "播放结束 · 空格重播" makes no sense on the grid, so put the hint
           line back the way a freshly scanned folder shows it */
        GridStatus();
        AppUpdateInfo();
        ChromeUpdateButtons();
        AppInvalidate();
    }
}

/* ---------------------------------------------------------- search / sort */

/* The typed name filter changes both grids at once: folder cards have to count
 * and preview only the matches, the file grid only lists them, and whatever
 * was selected or open may no longer exist.  Called on every keystroke. */
static void AppSearchChanged(void)
{
    FoldersRebuild();
    FilterRebuild();

    if (g_app.folderAuto) g_app.folderMode = g_app.folderCount > 0;
    if (g_app.selFolder >= FolderCells()) g_app.selFolder = -1;
    if (g_app.hoverFolder >= FolderCells()) g_app.hoverFolder = -1;
    if (g_app.folderMode && g_app.selFolder < 0 && FolderCells() > 0)
        g_app.selFolder = g_app.folderFiles > 0 ? 1 : 0;

    if (g_app.single) {
        if (FilterPos(g_app.cur) < 0) {
            if (g_app.viewCount) AppGoto(g_app.viewMap[0], TRUE);
            else AppSetSingle(FALSE);
        }
    } else if (g_app.sel >= 0 && FilterPos(g_app.sel) < 0) {
        g_app.sel = g_app.viewCount ? g_app.viewMap[0] : -1;
    }

    ViewLayout();
    UpdateTitle();
    AppUpdateInfo();
    ChromeUpdateButtons();
    if (!g_app.single) GridStatus();
    AppInvalidate();
}

static void AppSortSet(int mode)
{
    if (mode < SORT_NAME || mode > SORT_SIZE) return;
    g_app.sortMode = mode;
    FilterRebuild();
    if (!g_app.single) {
        if (g_app.sel >= 0 && FilterPos(g_app.sel) < 0)
            g_app.sel = g_app.viewCount ? g_app.viewMap[0] : -1;
        EnsureVisible(g_app.sel);
        GridStatus();
    }
    ViewLayout();
    UpdateTitle();
    AppUpdateInfo();
    ChromeUpdateButtons();
    AppInvalidate();
}

/* ------------------------------------------------------- folder browsing */

/* Land the keyboard selection on the first album, not on the "everything"
 * card: the arrow keys and Enter then have an obvious place to start.  A card
 * grid with no selection at all looks broken the moment a key is pressed. */
static void FolderSelectFirst(void)
{
    if (g_app.folderMode && g_app.selFolder < 0 && FolderCells() > 0)
        g_app.selFolder = g_app.folderFiles > 0 ? 1 : 0;
}

/* Shared tail of every "the grid now reflects another directory" move: re-derive
 * the folder cards and the file list from the array we already have, then put
 * every piece of view state back to a sane start.  No disk I/O, no rescan.
 * Callers save the grid they are leaving first (DirMarkSave); the tail then
 * applies whatever mark the grid it lands on left behind last time. */
static void AfterDirChange(void)
{
    g_app.scrollY = 0;
    g_app.selFolder = g_app.hoverFolder = -1;
    g_app.sel = -1;
    g_app.cur = -1;
    g_app.hover = -1;
    g_app.imgFailed = 0;
    g_app.panX = g_app.panY = 0;
    g_app.fit = TRUE;

    FoldersRebuild();
    FilterRebuild();
    g_app.folderMode = g_app.folderAuto ? (g_app.folderCount > 0) : g_app.folderMode;
    FolderSelectFirst();
    if (!g_app.folderMode && g_app.viewCount) g_app.sel = g_app.viewMap[0];

    ViewLayout();
    DirMarkRestore();      /* coming back to a grid we scrolled before */
    ViewLayout();          /* clamp the restored offset to this content height */
    UpdateTitle();
    AppUpdateInfo();
    ChromeUpdateButtons();
    GridStatus();
    AppInvalidate();
}

void AppEnterFolder(int cell)
{
    if (CellIsFilesRow(cell)) {          /* "this folder's own files" card */
        AppSetFolderMode(0);
        return;
    }
    int lead = g_app.folderFiles > 0 ? 1 : 0;
    FolderEntry *e = FolderAt(cell - lead);
    if (!e) return;

    DirMarkSave();                       /* where the card grid was scrolled to */
    SearchReset();                       /* the filter belonged to the old level */
    wcsncpy_s(g_app.curDir, MAX_PATH_LONG, e->path, _TRUNCATE);
    g_app.folderAt = GetTickCount64();   /* swallow a double-click's second hit */
    g_app.folderAuto = 1;
    g_app.folderMode = 0;
    AfterDirChange();
}

/* One level up, but never above the folder that was scanned: climbing out of
 * the tree is the ↑ button's job, and doing it here would silently re-scan. */
BOOL AppLeaveFolder(void)
{
    if (!g_app.curDir[0]) return FALSE;
    if (_wcsicmp(g_app.curDir, g_app.lib.root) == 0) return FALSE;

    WCHAR up[MAX_PATH_LONG];
    wcsncpy_s(up, MAX_PATH_LONG, g_app.curDir, _TRUNCATE);
    PathParent(up);
    if (!up[0]) return FALSE;
    if (!PathIsUnder(g_app.lib.root, up, NULL)) return FALSE;

    DirMarkSave();
    SearchReset();
    wcsncpy_s(g_app.curDir, MAX_PATH_LONG, up, _TRUNCATE);
    g_app.folderAt = GetTickCount64();
    g_app.folderAuto = 1;
    g_app.folderMode = 0;
    AfterDirChange();
    return TRUE;
}

void AppSetFolderMode(int on)
{
    on = on && g_app.folderCount > 0;
    if (on == g_app.folderMode) return;

    DirMarkSave();               /* the grid being replaced has its own place */
    g_app.folderMode = on;
    g_app.folderAuto = 0;        /* an explicit choice outlives the auto rule */
    g_app.folderAt = GetTickCount64();   /* swallow the 2nd click of a dblclick */
    g_app.scrollY = 0;
    g_app.hoverFolder = -1;
    g_app.hover = -1;
    g_app.selFolder = -1;
    if (on && FolderCells() > 0)
        g_app.selFolder = g_app.folderFiles > 0 ? 1 : 0;   /* first album */
    if (!on && g_app.sel < 0 && g_app.viewCount > 0) g_app.sel = g_app.viewMap[0];

    ViewLayout();
    DirMarkRestore();            /* flipping back lands on the other grid's row */
    ViewLayout();
    UpdateTitle();
    AppUpdateInfo();
    ChromeUpdateButtons();
    GridStatus();
    AppInvalidate();
}

void AppRotate(int degrees)
{
    if (!g_app.single || !g_app.img.bits) return;
    RotateBgra(&g_app.img.bits, &g_app.img.w, &g_app.img.h, degrees);
    g_app.fit = TRUE;
    g_app.panX = g_app.panY = 0;
    SetStatus(L"旋转 %d°", degrees);
    AppUpdateInfo();
    AppInvalidate();
}

void AppRefresh(void)
{
    WCHAR root[MAX_PATH_LONG], keep[MAX_PATH_LONG];
    wcsncpy_s(root, MAX_PATH_LONG, g_app.lib.root, _TRUNCATE);
    wcsncpy_s(keep, MAX_PATH_LONG, g_app.curDir, _TRUNCATE);
    if (root[0]) AppOpenFolder(root);
    /* stay in the sub-folder the user was in: the rescan covers the whole tree,
     * so only the index has to be rebuilt (the final post does that) */
    if (keep[0] && _wcsicmp(keep, root) != 0 && PathIsDir(keep))
        wcsncpy_s(g_app.curDir, MAX_PATH_LONG, keep, _TRUNCATE);
}

/* ------------------------------------------------------------------ paint */

static void EnsureBuffer(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (g_app.memDC && g_app.memW == w && g_app.memH == h) return;

    /* Order matters: a bitmap must never be deleted while it is still selected
     * into a DC.  Doing so used to leave the back-buffer DC in a broken state
     * on the second resize, after which every single blit took ~1.8 ms instead
     * of ~16 us (measured: 6.8 s of blits for 30 cells per frame). */
    if (g_app.memDC) { DeleteDC(g_app.memDC); g_app.memDC = NULL; }
    if (g_app.memBmp) { DeleteObject(g_app.memBmp); g_app.memBmp = NULL; }

    HDC screen = GetDC(g_app.hwnd);

    /* A DIB section, not CreateCompatibleBitmap.  On some drivers (this one
     * included) the compatible bitmap lands in video memory, so every GDI
     * write into the back buffer pays a bus round trip - measured at 1.8 ms
     * per thumbnail instead of 16 us.  A DIB section is guaranteed to be
     * ordinary system memory, and the single BitBlt to the window at the end
     * is still hardware accelerated. */
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    g_app.memDC  = CreateCompatibleDC(screen);
    g_app.memBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!g_app.memBmp)                     /* fall back if the section fails */
        g_app.memBmp = CreateCompatibleBitmap(screen, w, h);
    if (g_app.memDC && g_app.memBmp) {
        SelectObject(g_app.memDC, g_app.memBmp);
        SetBkMode(g_app.memDC, TRANSPARENT);
    }
    ReleaseDC(g_app.hwnd, screen);
    g_app.memW = w;
    g_app.memH = h;
}

static int       s_paintN;
static ULONGLONG s_paintSum, s_paintWorst;

void PaintStatsReset(void) { s_paintN = 0; s_paintSum = 0; s_paintWorst = 0; }
int  PaintStatsCount(void) { return s_paintN; }
ULONGLONG PaintStatsAvg(void) { return s_paintN ? s_paintSum / (ULONGLONG)s_paintN : 0; }
ULONGLONG PaintStatsWorst(void) { return s_paintWorst; }

static void OnPaint(void)
{
    ULONGLONG t0 = GetTickCount64();
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g_app.hwnd, &ps);
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);

    EnsureBuffer(rc.right, rc.bottom);
    if (g_app.memDC) {
        ViewLayout();
        ChromePaint(g_app.memDC);
        ViewPaint(g_app.memDC);
        DrawSeekBar(g_app.memDC);   /* video progress bar, above the status strip */
        /* blit only what was invalidated: a hover change is one cell, not the
         * whole client area */
        RECT dirty = ps.rcPaint;
        if (IsRectEmpty(&dirty)) dirty = rc;
        if (dirty.left   < 0)         dirty.left   = 0;
        if (dirty.top    < 0)         dirty.top    = 0;
        if (dirty.right  > rc.right)  dirty.right  = rc.right;
        if (dirty.bottom > rc.bottom) dirty.bottom = rc.bottom;
        BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
               g_app.memDC, dirty.left, dirty.top, SRCCOPY);
        if (s_bench && !s_firstFrame) {
            s_firstFrame = TRUE;
            BenchLog("first_frame=%llu ms", GetTickCount64() - s_t0);
        }
    } else {
        FillRectC(dc, &rc, C_BG);
    }
    EndPaint(g_app.hwnd, &ps);

    ULONGLONG dt = GetTickCount64() - t0;
    s_paintN++;
    s_paintSum += dt;
    if (dt > s_paintWorst) s_paintWorst = dt;
}

/* ------------------------------------------------------------------ layout */

static void OnSize(void)
{
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);

    int tb = ChromeToolbarHeight();
    int sb = S(26);
    g_app.rcToolbar = rc;
    g_app.rcToolbar.bottom = tb + 1;

    g_app.rcStatus = rc;
    g_app.rcStatus.top = rc.bottom - sb;

    g_app.rcGrid = rc;
    g_app.rcGrid.top = g_app.rcToolbar.bottom;
    g_app.rcGrid.bottom = g_app.rcStatus.top;

    int scale = g_app.thumbScale ? g_app.thumbScale : 100;
    g_app.cellW = S(168 * scale / 100);
    g_app.cellH = S(150 * scale / 100);

    ChromeLayout();
    ViewLayout();

    if (g_app.hVideo) {
        VideoResize();
        ShowWindow(g_app.hVideo, (g_app.single && VideoIsActive()) ? SW_SHOWNA : SW_HIDE);
    }
    AppInvalidate();
}

/* ------------------------------------------------------------------- full */

static void ToggleFullscreen(void)
{
    if (!g_app.fullscreen) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(g_app.hwnd, &wp);
        g_app.savedPlace = wp.rcNormalPosition;
        g_app.savedStyle = GetWindowLongPtrW(g_app.hwnd, GWL_STYLE);

        MONITORINFO mi = { sizeof(mi) };
        HMONITOR mon = MonitorFromWindow(g_app.hwnd, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(mon, &mi)) {
            SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(g_app.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
        }
        g_app.fullscreen = TRUE;
    } else {
        SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, g_app.savedStyle);
        SetWindowPos(g_app.hwnd, NULL, g_app.savedPlace.left, g_app.savedPlace.top,
                     g_app.savedPlace.right - g_app.savedPlace.left,
                     g_app.savedPlace.bottom - g_app.savedPlace.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
        g_app.fullscreen = FALSE;
    }
    AppInvalidate();
}

/* ------------------------------------------------------------------ window */

static void DoCommand(BtnId id)
{
    switch (id) {
    case BT_OPEN: {
        WCHAR dir[MAX_PATH_LONG];
        if (PickFolder(g_app.hwnd, dir, MAX_PATH_LONG)) AppOpenFolder(dir);
        break;
    }
    case BT_UP: {
        WCHAR dir[MAX_PATH_LONG];
        wcsncpy_s(dir, MAX_PATH_LONG, g_app.lib.root, _TRUNCATE);
        if (!dir[0]) break;
        PathParent(dir);
        if (dir[0]) AppOpenFolder(dir);
        break;
    }
    case BT_PREV: AppNext(-1); break;
    case BT_NEXT: AppNext(1); break;
    case BT_ZOOM_OUT: ZoomAt(1.0 / 1.25, (POINT){ g_app.rcGrid.left + (g_app.rcGrid.right - g_app.rcGrid.left) / 2,
                                                   g_app.rcGrid.top + (g_app.rcGrid.bottom - g_app.rcGrid.top) / 2 }); break;
    case BT_ZOOM_IN: ZoomAt(1.25, (POINT){ g_app.rcGrid.left + (g_app.rcGrid.right - g_app.rcGrid.left) / 2,
                                           g_app.rcGrid.top + (g_app.rcGrid.bottom - g_app.rcGrid.top) / 2 }); break;
    case BT_FIT: ZoomFit(); break;
    case BT_ONE:
        if (g_app.img.bits) {
            g_app.fit = FALSE;
            g_app.zoom = 1.0;
            g_app.panX = g_app.panY = 0;
            AppUpdateInfo();
            AppInvalidate();
        }
        break;
    case BT_GRID:
        if (g_app.folderMode) AppSetFolderMode(0);
        AppSetSingle(FALSE);
        break;
    case BT_FOLDERS: AppSetFolderMode(!g_app.folderMode); break;
    case BT_SINGLE: AppSetSingle(TRUE); break;
    case BT_ROTATE: AppRotate(90); break;
    case BT_PLAY: VideoToggle(); break;
    case BT_EXPLORE: {
        WCHAR path[MAX_PATH_LONG];
        if (ItemPathOf(CurrentIdx(), path, MAX_PATH_LONG))
            ShowInExplorer(path);
        else if (g_app.lib.root[0])
            OpenFolderInExplorer(g_app.lib.root);
        break;
    }
    case BT_SORT: {
        /* cycle 名称 -> 时间 -> 大小 -> 名称 */
        int m = g_app.sortMode + 1;
        if (m > SORT_SIZE) m = SORT_NAME;
        AppSortSet(m);
        break;
    }
    case BT_SLIDE: ToggleSlideshow(); break;
    case BT_FILTER: {
        /* cycle 全部 -> 图片 -> 视频 -> 全部 */
        int f = g_app.filter + 1;
        if (f > 2) f = 0;
        FilterSet(f);
        /* keep the selection inside the new view, then repaint everything */
        if (g_app.single && FilterPos(g_app.cur) < 0) {
            if (g_app.viewCount > 0) AppGoto(g_app.viewMap[0], TRUE);
            else AppSetSingle(FALSE);
        } else {
            if (g_app.sel >= 0 && FilterPos(g_app.sel) < 0)
                g_app.sel = g_app.viewCount ? g_app.viewMap[0] : -1;
            ViewLayout();
            UpdateTitle();
            AppUpdateInfo();
            ChromeUpdateButtons();
            GridStatus();
            AppInvalidate();
        }
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------- clipboard ---
 * Ctrl+C on a picture copies the picture itself (CF_DIB, 24bpp so every app
 * accepts it); on the grid, on a video, or with Ctrl+Shift it copies the path
 * the way it always did. */
static void CopyPathToClipboard(void)
{
    WCHAR path[MAX_PATH_LONG];
    if (!ItemPathOf(CurrentIdx(), path, MAX_PATH_LONG)) return;
    if (!OpenClipboard(g_app.hwnd)) return;
    EmptyClipboard();
    size_t n = (wcslen(path) + 1) * sizeof(WCHAR);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (mem) {
        void *p = GlobalLock(mem);
        if (p) { memcpy(p, path, n); GlobalUnlock(mem); }
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    SetStatus(L"已复制路径");
}

static void CopyImageToClipboard(void)
{
    if (!g_app.img.bits || g_app.img.w <= 0 || g_app.img.h <= 0) return;
    int w = g_app.img.w, h = g_app.img.h;
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
    size_t total = sizeof(BITMAPINFOHEADER) + stride * (size_t)h;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem) { SetStatus(L"内存不足，无法复制这张图片"); return; }

    BYTE *base = (BYTE *)GlobalLock(mem);
    if (!base) { GlobalFree(mem); return; }
    memset(base, 0, total);
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)base;
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = (LONG)w;
    bi->biHeight = (LONG)h;          /* positive: CF_DIB is bottom-up */
    bi->biPlanes = 1;
    bi->biBitCount = 24;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(stride * (size_t)h);

    BYTE *dst = base + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        const BYTE *src = g_app.img.bits + (size_t)(h - 1 - y) * w * 4;  /* BGRA, top-down */
        BYTE *row = dst + (size_t)y * stride;
        for (int x = 0; x < w; x++, src += 4, row += 3) {
            row[0] = src[0];
            row[1] = src[1];
            row[2] = src[2];
        }
    }
    GlobalUnlock(mem);

    if (!OpenClipboard(g_app.hwnd)) {
        GlobalFree(mem);
        SetStatus(L"复制图片失败（剪贴板被占用）");
        return;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_DIB, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        SetStatus(L"复制图片失败");
        return;
    }
    CloseClipboard();
    SetStatus(L"已复制图片到剪贴板（%d × %d）", w, h);
}

static void OnKey(WPARAM vk)
{
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
    BOOL shift = GetKeyState(VK_SHIFT) < 0;
    Library *lib = &g_app.lib;

    /* While the search field owns the keyboard, only navigation and editing
     * keys get through; everything else (O, R, F, ...) is text. */
    if (g_app.searchActive) {
        BOOL nav = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                   vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END;
        if (vk == VK_ESCAPE) { SearchReset(); AppSearchChanged(); return; }
        if (vk == VK_RETURN) {
            g_app.searchActive = FALSE;
            SetStatus(L"搜索「%s」· Ctrl+F 编辑 · Esc 清除", g_app.search);
            AppInvalidate();
            return;
        }
        if (vk == VK_BACK) {
            size_t n = wcslen(g_app.search);
            if (n) { g_app.search[n - 1] = 0; AppSearchChanged(); }
            return;
        }
        if (vk == VK_DELETE) {
            g_app.search[0] = 0;
            AppSearchChanged();
            return;
        }
        if (!nav) return;
    }

    switch (vk) {
    case VK_LEFT:
        if (g_app.single && shift && VideoIsActive()) { VideoSeekBy(-10.0); break; }
        if (g_app.folderMode) FolderStep(-1); else AppNext(-1);
        break;
    case VK_RIGHT:
        if (g_app.single && shift && VideoIsActive()) { VideoSeekBy(10.0); break; }
        if (g_app.folderMode) FolderStep(1); else AppNext(1);
        break;
    case VK_UP:
        if (!g_app.single) {
            if (g_app.folderMode) FolderStep(-g_app.cols); else AppNext(-g_app.cols);
        }
        break;
    case VK_DOWN:
        if (!g_app.single) {
            if (g_app.folderMode) FolderStep(g_app.cols); else AppNext(g_app.cols);
        }
        break;
    case VK_PRIOR:
        if (g_app.folderMode) FolderStep(-g_app.cols * 3); else AppNext(-g_app.cols * 3);
        break;
    case VK_NEXT:
        if (g_app.folderMode) FolderStep(g_app.cols * 3); else AppNext(g_app.cols * 3);
        break;
    case VK_HOME:
        if (!g_app.single && g_app.folderMode) FolderSelect(0);
        else if (g_app.single && g_app.viewCount) AppGoto(g_app.viewMap[0], TRUE);
        else if (g_app.viewCount) { g_app.sel = g_app.viewMap[0]; EnsureVisible(g_app.sel); AppUpdateInfo(); AppInvalidate(); }
        break;
    case VK_END:
        if (!g_app.single && g_app.folderMode) FolderSelect(FolderCells() - 1);
        else if (g_app.single && g_app.viewCount) AppGoto(g_app.viewMap[g_app.viewCount - 1], TRUE);
        else if (g_app.viewCount) { g_app.sel = g_app.viewMap[g_app.viewCount - 1]; EnsureVisible(g_app.sel); AppUpdateInfo(); AppInvalidate(); }
        break;
    case VK_RETURN:
    case VK_SPACE: {
        if (g_app.folderMode) {
            AppEnterFolder(g_app.selFolder >= 0 ? g_app.selFolder : 0);
            break;
        }
        if (vk == VK_SPACE && g_app.slideshow) { PauseSlideshow(); break; }
        if (vk == VK_SPACE && s_slidePaused && g_app.single) { StartSlideshow(); break; }
        BOOL isVideo = FALSE;
        EnterCriticalSection(&lib->lock);
        if (g_app.single && lib->count && g_app.cur >= 0 && g_app.cur < lib->count)
            isVideo = lib->items[g_app.cur].kind == MK_VIDEO;
        LeaveCriticalSection(&lib->lock);
        if (isVideo && vk == VK_SPACE) VideoToggle();
        else AppSetSingle(!g_app.single);
        break;
    }
    case VK_ESCAPE:
        if (g_app.search[0]) { SearchReset(); AppSearchChanged(); break; }
        if (g_app.slideshow || s_slidePaused) {
            StopSlideshow(FALSE);
            if (g_app.fullscreen) ToggleFullscreen();
            else AppSetSingle(FALSE);
            break;
        }
        if (g_app.fullscreen) ToggleFullscreen();
        else if (g_app.single) AppSetSingle(FALSE);
        else AppLeaveFolder();
        break;
    case VK_BACK:
        if (!g_app.single) AppLeaveFolder();
        break;
    case VK_F11:
        if (g_app.fullscreen && g_app.slideshow) StopSlideshow(FALSE);
        ToggleFullscreen();
        break;
    case VK_F5: AppRefresh(); break;
    case 'O': DoCommand(BT_OPEN); break;
    case 'R': AppRotate(90); break;
    case 'F':
        if (ctrl) {
            g_app.searchActive = TRUE;
            SetStatus(L"输入文件名以筛选 · Enter 完成 · Esc 清除");
            AppInvalidate();
        } else if (g_app.single) {
            ZoomFit();
        }
        break;
    case 'G': AppSetSingle(FALSE); break;
    case 'S': ToggleSlideshow(); break;
    case 'I':
        g_app.showInfo = !g_app.showInfo;
        SetStatus(g_app.showInfo ? L"信息浮层：开（I 关闭）" : L"信息浮层：关");
        AppInvalidate();
        break;
    case 'M':
        if (g_app.single && VideoIsActive()) VideoToggleMute();
        break;
    case 'E': {
        WCHAR path[MAX_PATH_LONG];
        if (ItemPathOf(CurrentIdx(), path, MAX_PATH_LONG)) OpenWithShell(path);
        break;
    }
    case VK_OEM_PLUS:
    case VK_ADD:
        if (g_app.single && VideoIsActive()) VideoAdjustVolume(10);
        else DoCommand(BT_ZOOM_IN);
        break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        if (g_app.single && VideoIsActive()) VideoAdjustVolume(-10);
        else DoCommand(BT_ZOOM_OUT);
        break;
    case VK_OEM_4: SlideIntervalStep(-1); break;   /* [ */
    case VK_OEM_6: SlideIntervalStep(1); break;    /* ] */
    case VK_OEM_2:                                  /* / */
        if (!ctrl) {
            SearchReset();
            g_app.searchActive = TRUE;
            AppSearchChanged();
        }
        break;
    case '1':
        if (g_app.single) DoCommand(BT_ONE);
        break;
    default:
        if (ctrl && vk == 'C') {
            if (!shift && g_app.single && g_app.img.bits &&
                g_app.img.item == g_app.cur && !CurrentIsVideo())
                CopyImageToClipboard();
            else
                CopyPathToClipboard();
        }
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_app.hwnd = hwnd;
        g_app.hVideo = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPSIBLINGS,
                                       0, 0, 10, 10, hwnd, NULL, g_app.inst, NULL);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, IDT_VIDEO, 500, NULL);
        return 0;
    }

    case WM_SIZE:
        OnSize();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        BtnId id = ChromeHit(pt);
        if (id != BT_NONE) { DoCommand(id); return 0; }
        SetFocus(hwnd);
        ViewOnDown(pt);
        return 0;
    }

    case WM_LBUTTONUP: {
        ViewOnUp();
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (ChromeHit(pt) != BT_NONE) return 0;
        ViewOnDoubleClick(pt);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = (int)ChromeHit(pt);
        if (hot != g_app.hoverBtn) { g_app.hoverBtn = hot; AppInvalidate(); }
        ViewOnMove(pt);
        /* WM_SETCURSOR runs before the hover state it would read is updated,
           so keep the hand cursor in sync here while in the grid */
        if (!g_app.single)
            SetCursor(LoadCursorW(NULL, GridHot() >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        ViewOnWheel(GET_WHEEL_DELTA_WPARAM(wp), pt);
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (ChromeHit(pt) != BT_NONE) return 0;
        /* right-click unwinds the whole stack: full screen, the picture, and
           the folder that was entered all give way in one step each time, never
           a stranded state.  ViewOnRightClick says whether it consumed it. */
        if (g_app.fullscreen) ToggleFullscreen();
        ViewOnRightClick();
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_app.navHot >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            if (!g_app.single) {
                SetCursor(LoadCursorW(NULL, GridHot() >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            if (g_app.single && !VideoIsActive() && g_app.img.bits) {
                SetCursor(LoadCursorW(NULL, g_app.dragging ? IDC_SIZEALL : IDC_ARROW));
                return TRUE;
            }
        }
        break;

    case WM_KEYDOWN:
        OnKey(wp);
        return 0;

    case WM_CHAR: {
        /* Only the search field wants text.  Returning 0 (instead of passing
         * it to DefWindowProc) also keeps the system from beeping at letters
         * that are consumed as shortcuts. */
        if (!g_app.searchActive) return 0;
        WCHAR ch = (WCHAR)wp;
        if (ch == L'/') return 0;      /* the search shortcut, not a filename char */
        if (ch >= L' ' && ch != 127) {
            size_t n = wcslen(g_app.search);
            if (n + 1 < SEARCH_MAX) {
                g_app.search[n] = ch;
                g_app.search[n + 1] = 0;
                AppSearchChanged();
            }
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        if (n > 0) {
            WCHAR path[MAX_PATH_LONG];
            DragQueryFileW(drop, 0, path, MAX_PATH_LONG);
            AppOpenPath(path);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_APP_SCAN: {
        BOOL final = (wp != 0);
        /* A superseded walker can still be winding down: its posts must not
         * repaint or finalize the folder that replaced it. */
        if ((LONG)lp != (LONG)InterlockedCompareExchange(&g_app.lib.gen, 0, 0))
            return 0;
        InterlockedExchange(&g_app.lib.notifyPending, 0);
        static int s_notifyN;
        s_notifyN++;
        if (final) {
            ScanSortDone();
            FilterRebuild();      /* viewMap now tracks the finished list */
            FoldersRebuild();     /* and the folder cards, for the current dir */
            if (g_app.folderAuto) g_app.folderMode = g_app.folderCount > 0;
            if (g_app.selFolder >= FolderCells()) g_app.selFolder = -1;
            if (g_app.hoverFolder >= FolderCells()) g_app.hoverFolder = -1;
            if (g_app.folderMode && g_app.selFolder < 0 && FolderCells() > 0)
                g_app.selFolder = g_app.folderFiles > 0 ? 1 : 0;
            if (!g_app.folderMode && g_app.sel < 0 && g_app.viewCount)
                g_app.sel = g_app.viewMap[0];
            BenchLog("scan_done=%llu ms paints=%d avg_paint=%llu ms worst_paint=%llu ms",
                     GetTickCount64() - s_t0, PaintStatsCount(),
                     PaintStatsAvg(), PaintStatsWorst());
            g_app.lib.scanning = 0;
            g_app.scanning = FALSE;
            GridStatus();
            BenchLog("scan_done=%llu ms items=%d dirs=%d",
                     GetTickCount64() - s_t0, g_app.lib.count, g_app.lib.dirs);
        }
        ViewLayout();
        if (final) {
            UpdateTitle();
            AppUpdateInfo();
            ChromeUpdateButtons();
            AppInvalidate();
            /* only jump once the scan is complete: mid-scan the viewMap is
             * empty, so AppSetSingle would bail and clear the pending file
             * without ever opening it */
            TryPendingJump();
            if (s_scrollBench) {
                BenchLog("scroll folders=%d cells=%d", g_app.folderCount, FolderCells());
                PaintStatsReset();
                s_sbDec0 = g_thumbDecodeN;
                s_sbReq0 = g_thumbRequests;
                s_sbT0 = GetTickCount64();
                SetTimer(hwnd, IDT_SCROLLBENCH, s_sbSlow ? 250 : 8, NULL);
            }
        } else {
            /* mid-scan: the toolbar never changes, so do not repaint it */
            AppUpdateInfo();
            if (g_app.hwnd) {
                InvalidateRect(g_app.hwnd, &g_app.rcGrid, FALSE);
                InvalidateRect(g_app.hwnd, &g_app.rcStatus, FALSE);
            }
        }
        return 0;
    }

    case WM_APP_VIDEO:
        VideoOnEvent(wp, lp);
        return 0;

    case WM_APP_VTHUMB:
        VideoThumbDeliver(wp, lp);
        if (!g_app.single) InvalidateRect(hwnd, &g_app.rcGrid, FALSE);
        return 0;

    case WM_APP_ITHUMB:
        ThumbDeliver(wp, lp);
        if (!g_app.single) InvalidateRect(hwnd, &g_app.rcGrid, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == IDT_VIDEO) {
            VideoTick();
        } else if (wp == IDT_THUMB_SETTLE) {
            /* the wheel has been still long enough: repaint the grid so the
             * thumbnails (and deferred video frames) for the new cells start
             * loading.  A one-shot: the next scroll arms it again. */
            KillTimer(hwnd, IDT_THUMB_SETTLE);
            InvalidateRect(hwnd, &g_app.rcGrid, FALSE);
        } else if (wp == IDT_SCROLLBENCH) {
            ScrollBenchTick();
        } else if (wp == IDT_SLIDESHOW) {
            SlideAdvance();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(640);
        mm->ptMinTrackSize.y = S(420);
        return 0;
    }

    case WM_CLOSE: {
        if (s_bench) {
            int used = 0;
            for (int i = 0; i < THUMB_MAX; i++) if (g_app.thumbs[i].item >= 0) used++;
            BenchLog("paints total=%d avg=%llu ms worst=%llu ms",
                     PaintStatsCount(), PaintStatsAvg(), PaintStatsWorst());
            BenchLog("thumb total_decode=%llu ms decodes=%d avg_decode=%llu us cache_hits=%d queue_since=%d bytes=%zu slotsUsed=%d items=%d",
                     g_thumbDecodeUs / 1000, g_thumbDecodeN,
                     g_thumbDecodeN ? g_thumbDecodeUs / (ULONGLONG)g_thumbDecodeN : 0,
                     g_thumbCacheHits, g_thumbRequests - s_sbReq0, g_app.thumbBytes, used, g_app.lib.count);
        }
        SettingsSave();
        KillTimer(hwnd, IDT_VIDEO);
        KillTimer(hwnd, IDT_THUMB_SETTLE);
        KillTimer(hwnd, IDT_SLIDESHOW);
        /* The decode workers talk to WIC and the media stack: they have to be
         * gone before COM/MF shut down, or the process faults on exit. */
        ImagePreloadShutdown();
        VideoThumbShutdown();
        VideoShutdown();
        ThumbWorkerShutdown();   /* picture decodes must stop before the cache goes */
        ScanStop();
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        Library *lib = &g_app.lib;
        ImagePreloadShutdown(); /* before ImageFree: the worker holds pixels */
        VideoThumbShutdown();   /* before the thumbnails go away */
        ThumbWorkerShutdown();  /* a decode in flight holds a thumb slot */
        /* If the walker would not stop, it may still be appending to this
         * array: leaking it at process exit is the only safe option. */
        if (!lib->thread) {
            for (int i = 0; i < lib->count; i++) free(lib->items[i].path);
            free(lib->items);
            free(lib->retire);
            lib->items = NULL;
            lib->retire = NULL;
            lib->count = lib->cap = 0;
        }
        free(g_app.viewMap);
        g_app.viewMap = NULL;
        g_app.viewCount = 0;
        FoldersFree();
        ThumbsClear();
        ImageFree();
        if (g_app.memBmp) DeleteObject(g_app.memBmp);
        if (g_app.memDC) DeleteDC(g_app.memDC);
        if (g_app.fontUI) DeleteObject(g_app.fontUI);
        if (g_app.fontSmall) DeleteObject(g_app.fontSmall);
        if (g_app.fontBold) DeleteObject(g_app.fontBold);
        GdiCacheFree();
        PostQuitMessage(0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------- main */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    (void)prev;
    (void)cmdLine;

    CrashInstall();
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    s_t0 = GetTickCount64();
    if (wcsstr(GetCommandLineW(), L"--bench")) s_bench = TRUE;

    memset(&g_app, 0, sizeof(g_app));
    g_app.inst = inst;
    g_app.cur = -1;
    g_app.sel = -1;
    g_app.hover = -1;
    g_app.hoverBtn = -1;
    g_app.navHot = -1;
    g_app.img.item = -1;
    g_app.videoItem = -1;
    g_app.zoom = 1.0;
    g_app.fit = TRUE;
    g_app.sortMode = SORT_NAME;
    g_app.thumbScale = 100;
    g_app.slideSec = 5;
    g_app.volume = 100;
    for (int i = 0; i < THUMB_MAX; i++) g_app.thumbs[i].item = -1;
    InitializeCriticalSection(&g_app.lib.lock);

    HDC screen = GetDC(NULL);
    g_app.dpi = (UINT)GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    if (!g_app.dpi) g_app.dpi = 96;

    UtilInit(&g_app.fontUI, &g_app.fontSmall, &g_app.fontBold);
    SettingsLoad();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, L"APPICON");
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = APP_CLASS;
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    int w = g_app.startW ? g_app.startW : S(1180);
    int h = g_app.startH ? g_app.startH : S(760);
    int x = g_app.startW ? g_app.startX : CW_USEDEFAULT;
    int y = g_app.startW ? g_app.startY : CW_USEDEFAULT;

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, APP_CLASS, APP_TITLE,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show ? show : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    /* Startup target, in priority order:
     *   1. a path on the command line (folder or file);
     *   2. the folder we saved last time (SettingsLoad already dropped it into
     *      g_app.lib.root if it still exists);
     *   3. nothing - the window opens empty, waiting for a drag or the O key.
     * The exe's own directory is deliberately NOT a fallback: a viewer should
     * never surprise the user by scanning wherever it happens to be unzipped. */
    WCHAR target[MAX_PATH_LONG] = { 0 };
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int a = 1;
        if (argc > 1 && wcscmp(argv[1], L"--bench") == 0) { s_bench = TRUE; a = 2; }
        if (argc > 1 && wcscmp(argv[1], L"--scrollbench") == 0) {
            s_bench = TRUE; s_scrollBench = TRUE; a = 2;
        }
        if (argc > 1 && wcscmp(argv[1], L"--scrollslow") == 0) {
            s_bench = TRUE; s_scrollBench = TRUE; s_sbSlow = TRUE; a = 2;
        }
        if (argc > a) wcsncpy_s(target, MAX_PATH_LONG, argv[a], _TRUNCATE);
        LocalFree(argv);
    }
    if (!target[0] && g_app.lib.root[0]) wcsncpy_s(target, MAX_PATH_LONG, g_app.lib.root, _TRUNCATE);
    if (target[0]) AppOpenPath(target);   /* empty means: stay on the blank start state */

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
