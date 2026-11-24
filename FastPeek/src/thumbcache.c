/* FastPeek - on-disk thumbnail cache.
 *
 * Decoding a 24 MP photo costs ~40 ms; reading a pre-decoded 256 px thumbnail
 * back from disk costs well under a millisecond.  Explorer feels instant
 * precisely because it never decodes the same file twice.  This is the same
 * idea: the first time a picture is thumbnailed the pixels are written to
 * %LocalAppData%\FastPeek\thumbs\ and every later visit - including a fresh
 * process run - reads them back.
 *
 * Design:
 *   - The cache lives in the LOCAL PROFILE (%LocalAppData%\FastPeek\thumbs),
 *     never on removable media.  Running from a USB stick therefore never
 *     writes to it - the source folder and the stick both stay read-only.
 *   - The key is the full path (FNV-1a 64), not the bytes, so the cache stays
 *     valid across renames and the source tree is never written to.  The source
 *     folder remains strictly read-only.
 *   - Each file stores the source mtime + size in its header; if either differs
 *     the entry is stale and is re-decoded.
 *   - Thumbnails are cached at one generous fixed size (256) regardless of the
 *     current cell size, then shrunk on read when needed.  The shrink is a
 *     box-filter over a small image, a few tens of microseconds.
 *   - The cache is capped by file count; past the limit the oldest (by last
 *     write time) entries are pruned.  No in-memory index is kept - the disk
 *     is the source of truth - so the cache survives restarts for free.
 */
#include "app.h"

#define TC_MAGIC   0x46505443u   /* "FTPC" */
#define TC_VERSION 1
#define TC_SIDE    256           /* cached thumbnail is square-ish, capped here */
#define TC_MAX_FILES 20000

/* FNV-1a 64-bit over the *full* path (case-insensitive: the walker already
 * yields normalized paths, and Windows paths are case-insensitive anyway). */
static ULONGLONG HashChars(ULONGLONG h, const WCHAR *p)
{
    for (; *p; p++) {
        WCHAR c = *p;
        if (c >= L'A' && c <= L'Z') c = (WCHAR)(c + 32);
        h ^= (ULONGLONG)c;
        h *= 1099511628211ull;
    }
    return h;
}

/* A USB stick routinely comes back under a different drive letter after it is
 * re-plugged.  Hashing the literal path would then miss every cached thumbnail
 * and re-decode the whole folder - exactly what this cache exists to avoid.
 * Hash the volume serial number plus the path below the root instead; UNC
 * shares and other non-drive paths fall back to the literal path.
 *
 * The serial is cached per drive and re-checked every few seconds, so swapping
 * two sticks while the drive letter stays the same cannot serve the wrong
 * entry either. */
static ULONGLONG PathHash(const WCHAR *path)
{
    ULONGLONG h = 14695981039346656037ull;

    if (path[0] && path[1] == L':' && path[2] == L'\\') {
        static WCHAR     sRoot[4];
        static DWORD     sSerial;
        static BOOL      sValid;
        static ULONGLONG sChecked;

        ULONGLONG now = GetTickCount64();
        WCHAR root[4] = { path[0], L':', L'\\', 0 };
        if (!sValid || _wcsicmp(root, sRoot) != 0 || now - sChecked > 5000) {
            DWORD serial = 0;
            sValid = GetVolumeInformationW(root, NULL, 0, &serial, NULL, NULL, NULL, 0) != 0;
            if (sValid) {
                wmemcpy(sRoot, root, 4);
                sSerial = serial;
            }
            sChecked = now;
        }
        if (sValid) {
            h ^= (ULONGLONG)sSerial;
            h *= 1099511628211ull;
            return HashChars(h, path + 3);   /* path relative to the volume root */
        }
    }
    return HashChars(h, path);
}

/* ------------------------------------------------------------ cache paths */

static BOOL ThumbCacheDir(WCHAR *out, size_t cap)
{
    WCHAR base[MAX_PATH_LONG];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH_LONG);
    if (!n || n >= MAX_PATH_LONG) return FALSE;
    WCHAR sub[MAX_PATH_LONG];
    PathJoin(sub, _countof(sub), base, L"FastPeek");
    PathJoin(out, cap, sub, L"thumbs");
    return TRUE;
}

/* hash -> "thumbs\\1A2B3C4D.tc" */
static void EntryPath(const WCHAR *path, WCHAR *out, size_t cap)
{
    WCHAR dir[MAX_PATH_LONG];
    if (!ThumbCacheDir(dir, MAX_PATH_LONG)) { out[0] = 0; return; }
    ULONGLONG h = PathHash(path);
    WCHAR name[24];
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"%016llx.tc", h);
    PathJoin(out, cap, dir, name);
}

/* ------------------------------------------------------------------ format
 * header (all little-endian):
 *   u32 magic   u32 version
 *   u32 width   u32 height        (pixel dimensions, BGRA, top-down)
 *   u64 mtime   (FILETIME as u64) u64 size
 * then width*height*4 bytes of BGRA. */

typedef struct {
    DWORD    magic;
    DWORD    version;
    DWORD    w;
    DWORD    h;
    ULONGLONG mtime;
    ULONGLONG size;
} TCHeader;

static void FileTimeToU64(const FILETIME *ft, ULONGLONG *out)
{
    *out = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* write the pixels; returns TRUE on success */
static BOOL ThumbWrite(const WCHAR *path, const BYTE *bits, int w, int h,
                       const FILETIME *mtime, LONGLONG size)
{
    WCHAR ep[MAX_PATH_LONG];
    EntryPath(path, ep, MAX_PATH_LONG);
    if (!ep[0]) return FALSE;

    WCHAR dir[MAX_PATH_LONG];
    ThumbCacheDir(dir, MAX_PATH_LONG);
    CreateDirectoryW(dir, NULL);

    HANDLE f = CreateFileW(ep, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;

    TCHeader hd;
    hd.magic = TC_MAGIC;
    hd.version = TC_VERSION;
    hd.w = (DWORD)w;
    hd.h = (DWORD)h;
    FileTimeToU64(mtime, &hd.mtime);
    hd.size = (ULONGLONG)size;

    DWORD done = 0;
    BOOL ok = WriteFile(f, &hd, sizeof(hd), &done, NULL) && done == sizeof(hd);
    if (ok) {
        DWORD n = (DWORD)((size_t)w * h * 4);
        ok = WriteFile(f, bits, n, &done, NULL) && done == n;
    }
    CloseHandle(f);
    return ok;
}

/* Read back a thumbnail only if it matches the source's mtime and size.
 * On success the caller owns the returned buffer (w*h*4, BGRA). */
static BYTE *ThumbRead(const WCHAR *path, const FILETIME *mtime, LONGLONG size,
                       int *outW, int *outH)
{
    *outW = *outH = 0;
    WCHAR ep[MAX_PATH_LONG];
    EntryPath(path, ep, MAX_PATH_LONG);
    if (!ep[0]) return NULL;

    HANDLE f = CreateFileW(ep, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;

    TCHeader hd;
    DWORD got = 0;
    BOOL ok = ReadFile(f, &hd, sizeof(hd), &got, NULL) && got == sizeof(hd);
    if (ok) {
        ULONGLONG mt;
        FileTimeToU64(mtime, &mt);
        ok = hd.magic == TC_MAGIC && hd.version == TC_VERSION &&
             hd.mtime == mt && hd.size == (ULONGLONG)size &&
             hd.w > 0 && hd.h > 0 && hd.w <= 4096 && hd.h <= 4096;
    }

    BYTE *bits = NULL;
    if (ok) {
        size_t nbytes = (size_t)hd.w * hd.h * 4;
        bits = (BYTE *)malloc(nbytes);
        ok = bits && ReadFile(f, bits, (DWORD)nbytes, &got, NULL) && got == nbytes;
        if (!ok) { free(bits); bits = NULL; }
    }
    CloseHandle(f);

    if (bits) { *outW = (int)hd.w; *outH = (int)hd.h; }
    return bits;
}

/* --------------------------------------------- lazy, throttled pruning ---
 * Counting every file in the thumbs dir on each write would be slow, so prune
 * only occasionally (every 512 successful writes) and only when the dir has
 * clearly overflowed.  Entries are ordered by last-write time; the oldest are
 * removed until we are back under the cap.
 *
 * The entry is a bare file name, not a path: at the 20 000-file cap a
 * MAX_PATH_LONG buffer per entry is 40 MB to allocate, qsort and free - a
 * multi-hundred-millisecond fault on whatever thread calls this.  It runs on
 * the thumbnail worker, never on the ui thread. */
typedef struct { FILETIME t; WCHAR name[32]; } CacheEntry;

static int __cdecl CompareMtimeAsc(void *ctx, const void *pa, const void *pb)
{
    (void)ctx;
    const FILETIME *a = &((const CacheEntry *)pa)->t;
    const FILETIME *b = &((const CacheEntry *)pb)->t;
    ULONGLONG ua = ((ULONGLONG)a->dwHighDateTime << 32) | a->dwLowDateTime;
    ULONGLONG ub = ((ULONGLONG)b->dwHighDateTime << 32) | b->dwLowDateTime;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

static void Prune(void)
{
    WCHAR dir[MAX_PATH_LONG];
    if (!ThumbCacheDir(dir, MAX_PATH_LONG)) return;
    WCHAR pat[MAX_PATH_LONG];
    PathJoin(pat, _countof(pat), dir, L"*.tc");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    /* collect (mtime, path) pairs */
    CacheEntry *es = NULL;
    int n = 0, cap = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 256;
            CacheEntry *g = (CacheEntry *)realloc(es, (size_t)nc * sizeof(CacheEntry));
            if (!g) { if (es) free(es); FindClose(h); return; }
            es = g; cap = nc;
        }
        es[n].t = fd.ftLastWriteTime;
        wcsncpy_s(es[n].name, _countof(es[n].name), fd.cFileName, _TRUNCATE);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (n <= TC_MAX_FILES) { free(es); return; }

    /* order oldest-first (qsort, not the O(n^2) bubble this replaced: at the
     * 20k cap the bubble is ~400M comparisons and stalls the UI thread) */
    qsort_s(es, (size_t)n, sizeof(CacheEntry), CompareMtimeAsc, NULL);

    int drop = n - TC_MAX_FILES;
    for (int i = 0; i < drop; i++) {
        WCHAR ep[MAX_PATH_LONG];
        PathJoin(ep, _countof(ep), dir, es[i].name);
        DeleteFileW(ep);
    }
    free(es);
}

/* Throttled wrapper.  This runs on the thumbnail workers, so two of them can
 * arrive at once: the loser skips, because a scan of 20 000 entries is not
 * something to do twice in parallel for the same result. */
static void PruneIfNeeded(void)
{
    static ULONGLONG since;
    static SRWLOCK   gate = SRWLOCK_INIT;

    if (since++ % 512 != 0) return;
    if (!TryAcquireSRWLockExclusive(&gate)) return;
    Prune();
    ReleaseSRWLockExclusive(&gate);
}

/* ------------------------------------------------------------------ public
 * Try to load a cached thumbnail for a media item; returns a BGRA buffer the
 * caller owns (may be larger than wanted - call DownscaleBgra afterwards), or
 * NULL when there is no valid cache entry. */
BYTE *ThumbCacheLoad(const WCHAR *path, const FILETIME *mtime, LONGLONG size,
                     int *outW, int *outH)
{
    BYTE *bits = ThumbRead(path, mtime, size, outW, outH);
    if (bits) {
        /* touch the file so LRU-style pruning keeps recently used entries */
        WCHAR ep[MAX_PATH_LONG];
        EntryPath(path, ep, MAX_PATH_LONG);
        if (ep[0]) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            HANDLE f = CreateFileW(ep, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) { SetFileTime(f, NULL, NULL, &now); CloseHandle(f); }
        }
    }
    return bits;
}

/* Store a decoded thumbnail so the next visit (or next run) skips decoding. */
void ThumbCacheStore(const WCHAR *path, const BYTE *bits, int w, int h,
                     const FILETIME *mtime, LONGLONG size)
{
    if (!bits || w <= 0 || h <= 0) return;
    ThumbWrite(path, bits, w, h, mtime, size);
    PruneIfNeeded();
}
