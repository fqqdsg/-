/* FastPeek - background folder walker.
 *
 * The UI thread never touches the disk during a scan.  A worker collects the
 * media list under a critical section and only wakes the UI when it has made
 * progress; the UI sorts the list once the walk is done so that indices stay
 * stable while items stream in.
 */
#include "app.h"

#define EXT_MAX 32
#define ITEM_LIMIT 400000

static const WCHAR *kImageExt[] = {
    L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"gif", L"tif", L"tiff",
    L"webp", L"heic", L"heif", L"avif", L"dng", L"cr2", L"nef", L"arw",
    L"orf", L"rw2", L"pef", L"srw", L"tga", L"jxl", NULL
};

static const WCHAR *kVideoExt[] = {
    L"mp4", L"m4v", L"mov", L"wmv", L"avi", L"mkv", L"webm", L"mpg", L"mpeg",
    L"m2ts", L"mts", L"ts", L"3gp", L"3g2", L"flv", L"ogv", L"asf", L"vob", NULL
};

static WCHAR Lower(WCHAR c)
{
    return (c >= L'A' && c <= L'Z') ? (WCHAR)(c + 32) : c;
}

static BOOL ExtInList(const WCHAR *ext, const WCHAR **list)
{
    for (int i = 0; list[i]; i++)
        if (NaturalCompare(ext, list[i]) == 0) return TRUE;
    return FALSE;
}

BOOL IsMediaFile(const WCHAR *name, MediaKind *kind)
{
    const WCHAR *dot = wcsrchr(name, L'.');
    if (!dot || dot == name || dot[1] == 0) return FALSE;
    WCHAR ext[EXT_MAX];
    int n = 0;
    for (const WCHAR *p = dot + 1; *p && n < EXT_MAX - 1; p++) ext[n++] = Lower(*p);
    ext[n] = 0;
    if (ExtInList(ext, kImageExt)) { if (kind) *kind = MK_IMAGE; return TRUE; }
    if (ExtInList(ext, kVideoExt)) { if (kind) *kind = MK_VIDEO; return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ notify */

/* Every post carries the generation of the walk that produced it.  A stale
 * walk that is still winding down (it can take a while on a slow device) must
 * not wake the UI for the folder that replaced it. */
static void NotifyUI(BOOL final, LONG gen)
{
    if (final) {
        InterlockedExchange(&g_app.lib.notifyPending, 1);
        PostMessageW(g_app.hwnd, WM_APP_SCAN, 1, (LPARAM)gen);
        return;
    }
    /* Every notification costs the UI a full repaint.  On a fast disk the walk
     * finishes 15k files in 200 ms, which used to fire ~150 posts and leave
     * the window permanently painting.  Throttle to ~11 Hz; the final post is
     * never throttled. */
    static ULONGLONG lastPost;
    ULONGLONG now = GetTickCount64();
    if (lastPost && now - lastPost < SCAN_NOTIFY_MS) return;
    lastPost = now;
    if (InterlockedExchange(&g_app.lib.notifyPending, 1) == 0)
        PostMessageW(g_app.hwnd, WM_APP_SCAN, 0, (LPARAM)gen);
}

/* --------------------------------------------------------------- collecting */

/* how much of the walk was spent standing off the disk for the pictures */
static int       s_yieldN;
static ULONGLONG s_yieldMs;
/* directories left behind by SCAN_MAX_DEPTH (see Walk) */
static int       s_cutDirs;

/* Share the device with the pictures.
 *
 * A stick serves reads in arrival order, so a directory listing issued while
 * the user is waiting for a photo lands in front of that photo - which is why
 * opening an album before the tree had been walked felt like a stuck window.
 * Whenever the decoders are working the walk stands off the disk before each
 * directory; the moment the grid goes quiet the stamp ages out and the walk
 * runs at full speed again.  Never longer than SCAN_YIELD_MAX_MS at a stretch,
 * so a screen that keeps asking for pictures cannot lock the tree out for
 * good: the walk always keeps creeping forward. */
static void YieldToPictures(void)
{
    Library *lib = &g_app.lib;
    if (!ThumbBusy()) return;

    ULONGLONG t0 = GetTickCount64();
    while (ThumbBusy()) {
        if (InterlockedCompareExchange(&lib->cancel, 0, 0)) break;
        if (GetTickCount64() - t0 >= SCAN_YIELD_MAX_MS) break;
        Sleep(15);
    }
    s_yieldN++;
    s_yieldMs += GetTickCount64() - t0;
}

static void AddItem(const WCHAR *dir, const WIN32_FIND_DATAW *fd)
{
    Library *lib = &g_app.lib;
    MediaKind kind;
    if (!IsMediaFile(fd->cFileName, &kind)) return;

    size_t need = wcslen(dir) + wcslen(fd->cFileName) + 2;
    WCHAR *full = (WCHAR *)malloc(need * sizeof(WCHAR));
    if (!full) return;
    PathJoin(full, need, dir, fd->cFileName);

    EnterCriticalSection(&lib->lock);
    if (lib->count >= ITEM_LIMIT) {
        LeaveCriticalSection(&lib->lock);
        free(full);
        return;
    }
    if (lib->count == lib->cap) {
        int ncap = lib->cap ? lib->cap * 2 : 512;
        MediaItem *grown = (MediaItem *)realloc(lib->items, (size_t)ncap * sizeof(MediaItem));
        if (!grown) {
            LeaveCriticalSection(&lib->lock);
            free(full);
            return;
        }
        lib->items = grown;
        lib->cap = ncap;
    }
    MediaItem *it = &lib->items[lib->count++];
    it->path  = full;
    it->name  = PathFileName(full);
    it->size  = ((LONGLONG)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    it->mtime = fd->ftLastWriteTime;
    it->kind  = kind;
    LeaveCriticalSection(&lib->lock);
}

static void Walk(const WCHAR *dir, int depth, LONG gen)
{
    Library *lib = &g_app.lib;
    if (depth > SCAN_MAX_DEPTH) return;    /* belt: the descent is guarded below */

    YieldToPictures();
    if (InterlockedCompareExchange(&lib->cancel, 0, 0)) return;

    WCHAR pattern[MAX_PATH_LONG];
    PathJoin(pattern, _countof(pattern), dir, L"*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd,
                                FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        h = FindFirstFileW(pattern, &fd);          /* old USB drivers */
    if (h == INVALID_HANDLE_VALUE) return;

    WCHAR **subdirs = NULL;
    int    nSub = 0, capSub = 0;
    int    added = 0;

    do {
        if (InterlockedCompareExchange(&lib->cancel, 0, 0)) break;

        DWORD attr = fd.dwFileAttributes;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (fd.cFileName[0] == L'.') continue;
            if (NaturalCompare(fd.cFileName, L"$recycle.bin") == 0) continue;
            if (NaturalCompare(fd.cFileName, L"system volume information") == 0) continue;
            if (nSub == capSub) {
                int nc = capSub ? capSub * 2 : 32;
                WCHAR **grown = (WCHAR **)realloc(subdirs, (size_t)nc * sizeof(WCHAR *));
                if (grown) { subdirs = grown; capSub = nc; }
            }
            if (nSub < capSub) {
                size_t n = wcslen(dir) + wcslen(fd.cFileName) + 2;
                WCHAR *p = (WCHAR *)malloc(n * sizeof(WCHAR));
                if (p) { PathJoin(p, n, dir, fd.cFileName); subdirs[nSub++] = p; }
            }
        } else {
            if (attr & FILE_ATTRIBUTE_HIDDEN) continue;
            AddItem(dir, &fd);
            if (++added >= 96) { added = 0; NotifyUI(FALSE, gen); }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    for (int i = 0; i < nSub; i++) {
        if (InterlockedCompareExchange(&lib->cancel, 0, 0)) { free(subdirs[i]); continue; }
        /* one level past the cap: the folder is not indexed at all, so drop it
         * instead of collecting an empty card for it.  Counted so --bench can
         * show how much of a deep tree a two-level walk leaves behind. */
        if (depth + 1 > SCAN_MAX_DEPTH) { s_cutDirs++; free(subdirs[i]); continue; }
        lib->dirs++;
        Walk(subdirs[i], depth + 1, gen);
        free(subdirs[i]);
    }
    free(subdirs);
}

static int __cdecl CompareItems(void *ctx, const void *pa, const void *pb)
{
    (void)ctx;
    const MediaItem *a = (const MediaItem *)pa, *b = (const MediaItem *)pb;
    return NaturalCompare(a->path, b->path);
}

/* Sorting 100k paths with a natural-order comparison costs ~100 ms.  Doing it
 * on the UI thread meant a visible hitch right when the user starts using the
 * window; on the worker it is invisible.  We sort a copy and swap it in, so a
 * repaint that is already running never sees a half-permuted array. */
static void SortAndPublish(Library *lib)
{
    EnterCriticalSection(&lib->lock);
    int        n   = lib->count;
    MediaItem *old = lib->items;
    MediaItem *sorted = NULL;
    if (n > 1) {
        sorted = (MediaItem *)malloc((size_t)n * sizeof(MediaItem));
        if (sorted) memcpy(sorted, old, (size_t)n * sizeof(MediaItem));
    }
    LeaveCriticalSection(&lib->lock);

    ULONGLONG t0 = GetTickCount64();
    if (sorted) qsort_s(sorted, (size_t)n, sizeof(MediaItem), CompareItems, NULL);
    BenchLog("sort=%llu ms n=%d", GetTickCount64() - t0, n);

    EnterCriticalSection(&lib->lock);
    if (sorted && lib->count == n && lib->items == old && !lib->cancel) {
        lib->items  = sorted;
        lib->retire = old;          /* freed by the UI thread on the final post */
    } else {
        free(sorted);               /* scan superseded: keep what we have */
    }
    LeaveCriticalSection(&lib->lock);
}

static DWORD WINAPI ScanThread(LPVOID param)
{
    /* The stick is the bottleneck, not the CPU, but a normal-priority walker
     * still steals scheduling slots from paints and input on a busy machine.
     * Below normal keeps the window responsive while the tree is walked. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    Library *lib = (Library *)param;
    LONG gen = (LONG)InterlockedCompareExchange(&lib->gen, 0, 0);
    s_cutDirs = 0;
    s_yieldN = 0;
    s_yieldMs = 0;
    WCHAR root[MAX_PATH_LONG];
    wcsncpy_s(root, MAX_PATH_LONG, lib->root, _TRUNCATE);
    Walk(root, 0, gen);
    BenchLog("walk_done items=%d dirs=%d cut=%d yield=%d eps %llu ms", lib->count,
             lib->dirs, s_cutDirs, s_yieldN, s_yieldMs);
    if (!InterlockedCompareExchange(&lib->cancel, 0, 0)) SortAndPublish(lib);
    NotifyUI(TRUE, gen);
    return 0;
}

/* ------------------------------------------------------------------ public */

BOOL ScanStop(void)
{
    Library *lib = &g_app.lib;
    InterlockedExchange(&lib->cancel, 1);
    if (lib->thread) {
        DWORD w = WaitForSingleObject(lib->thread, 5000);
        if (w != WAIT_OBJECT_0) {
            /* The walker is wedged (dead network share, failing disk).  Leave
             * the handle and the cancel flag alone: freeing the array or
             * starting a second walker now would have two threads writing the
             * same list.  The walker's own final post still clears the state
             * once it does return. */
            return FALSE;
        }
        CloseHandle(lib->thread);
        lib->thread = NULL;
    }
    InterlockedExchange(&lib->scanning, 0);
    InterlockedExchange(&lib->cancel, 0);
    return TRUE;
}

BOOL ScanStart(const WCHAR *root)
{
    Library *lib = &g_app.lib;
    if (!ScanStop())
        return FALSE;                   /* keep the old folder, it still works */

    EnterCriticalSection(&lib->lock);
    for (int i = 0; i < lib->count; i++) free(lib->items[i].path);
    free(lib->items);
    free(lib->retire);              /* struct array only: paths are shared */
    lib->items = NULL;
    lib->retire = NULL;
    lib->count = lib->cap = 0;
    lib->dirs = 0;
    LeaveCriticalSection(&lib->lock);

    if (!root || !root[0]) { InterlockedExchange(&lib->scanning, 0); return TRUE; }
    wcsncpy_s(lib->root, MAX_PATH_LONG, root, _TRUNCATE);
    PathNormalize(lib->root);

    InterlockedIncrement(&lib->gen);     /* invalidates posts of the old walk */
    InterlockedExchange(&lib->scanning, 1);
    InterlockedExchange(&lib->notifyPending, 0);
    lib->thread = CreateThread(NULL, 0, ScanThread, lib, 0, NULL);
    if (!lib->thread) {
        InterlockedExchange(&lib->scanning, 0);
        return FALSE;
    }
    return TRUE;
}

void ScanRestart(void)
{
    WCHAR root[MAX_PATH_LONG];
    wcsncpy_s(root, MAX_PATH_LONG, g_app.lib.root, _TRUNCATE);
    ScanStart(root);
}

/* Called by the UI on the final notification.  The sort itself already
 * happened on the worker, so all that is left is dropping the pre-sort array.
 * Safe here and only here: the UI is single threaded, so no repaint that still
 * references it can be in flight while we run. */
void ScanSortDone(void)
{
    Library *lib = &g_app.lib;
    MediaItem *retire;
    EnterCriticalSection(&lib->lock);
    retire = lib->retire;
    lib->retire = NULL;
    LeaveCriticalSection(&lib->lock);
    free(retire);
}

/* ---------------------------------------------------------------- filter
 * viewMap is a plain array of real item indices, rebuilt whenever the filter
 * or the item list changes.  The UI threads that walk it do so on the main
 * thread only, so no lock is needed here. */

BOOL SearchMatchName(const WCHAR *name)
{
    if (!g_app.search[0]) return TRUE;
    return StrStrIW(name, g_app.search) != NULL;
}

typedef struct {
    const MediaItem *items;
    int              mode;
} ViewSortCtx;

static ULONGLONG FileTimeKey(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* viewMap is an array of *indices*, so the comparator resolves them against
 * the item array (borrowed only for the duration of the qsort, which runs
 * under the library lock).  Every mode falls back to the natural path order,
 * so two files with the same timestamp or size stay in a predictable place. */
static int __cdecl CompareViewIdx(void *ctx, const void *pa, const void *pb)
{
    const ViewSortCtx *c = (const ViewSortCtx *)ctx;
    const MediaItem *a = &c->items[*(const int *)pa];
    const MediaItem *b = &c->items[*(const int *)pb];
    int r = 0;

    if (c->mode == SORT_DATE) {
        ULONGLONG ta = FileTimeKey(&a->mtime), tb = FileTimeKey(&b->mtime);
        if (ta != tb) r = ta > tb ? -1 : 1;          /* newest first */
    } else if (c->mode == SORT_SIZE) {
        if (a->size != b->size) r = a->size > b->size ? -1 : 1;
    }
    if (!r) r = NaturalCompare(a->path, b->path);
    return r;
}

void FilterRebuild(void)
{
    Library *lib = &g_app.lib;
    free(g_app.viewMap);
    g_app.viewMap = NULL;
    g_app.viewCount = 0;

    int *map = NULL;
    int  k = 0;

    /* The walker can still be appending, and realloc can move the array, so
     * read it under the same lock the walker uses. */
    EnterCriticalSection(&lib->lock);
    int n = lib->count;
    if (n > 0) map = (int *)malloc((size_t)n * sizeof(int));
    if (map) {
        for (int i = 0; i < n; i++) {
            MediaKind kind = lib->items[i].kind;
            if (g_app.filter == 1 && kind != MK_IMAGE) continue;
            if (g_app.filter == 2 && kind != MK_VIDEO) continue;
            if (!SearchMatchName(lib->items[i].name)) continue;
            /* the folder browser shows one level at a time: everything below
             * curDir, nothing from its siblings */
            if (g_app.curDir[0] &&
                !PathIsUnder(g_app.curDir, lib->items[i].path, NULL)) continue;
            map[k++] = i;
        }
        if (map && k > 1) {
            ViewSortCtx ctx = { lib->items,
                                (g_app.sortMode == SORT_DATE || g_app.sortMode == SORT_SIZE)
                                ? g_app.sortMode : SORT_NAME };
            qsort_s(map, (size_t)k, sizeof(int), CompareViewIdx, &ctx);
        }
    }
    LeaveCriticalSection(&lib->lock);

    g_app.viewMap = map;
    g_app.viewCount = k;
}

void FilterSet(int filter)
{
    if (filter < 0) filter = 0;
    if (filter > 2) filter = 2;
    if (filter == g_app.filter) return;
    g_app.filter = filter;
    FilterRebuild();
    /* cards are per-filter too: a folder with no videos should not sit in the
     * grid when only videos are being shown, and the badges must match */
    FoldersRebuild();
    if (g_app.folderAuto) g_app.folderMode = g_app.folderCount > 0;
    if (g_app.selFolder >= FolderCells()) g_app.selFolder = -1;
    if (g_app.hoverFolder >= FolderCells()) g_app.hoverFolder = -1;
}

int FilterPos(int real)
{
    for (int i = 0; i < g_app.viewCount; i++)
        if (g_app.viewMap[i] == real) return i;
    return -1;
}

int FilterStep(int real, int delta)
{
    if (g_app.viewCount == 0) return -1;
    int pos = FilterPos(real);
    if (pos < 0) {
        /* the current item is filtered out: fall back to a neighbour */
        for (int i = 0; i < g_app.viewCount; i++) {
            if (delta > 0 && g_app.viewMap[i] > real) return g_app.viewMap[i];
            if (delta < 0 && g_app.viewMap[g_app.viewCount - 1 - i] < real)
                return g_app.viewMap[g_app.viewCount - 1 - i];
        }
        return -1;
    }
    int np = pos + delta;
    if (np < 0 || np >= g_app.viewCount) return -1;
    return g_app.viewMap[np];
}
