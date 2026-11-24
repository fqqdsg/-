/* FastPeek - decoding (WIC) and the two caches.
 *
 * Everything is 32bpp BGRA, top-down, stride = w*4.  Thumbnails ask the codec
 * for the closest size it can decode *directly* (JPEG DCT scaling), which is
 * several times faster than decoding full frames and shrinking them.
 *
 * The big picture is decoded right here on the ui thread (the user just asked
 * for it, one file), but grid thumbnails are decoded on a worker pool: see
 * "thumbnail worker" below.
 */
#include "app.h"
#include <wincodec.h>

#define MAX_DECODE_SIDE 4096

/* One factory per thread: the ui thread decodes the single view, the worker
 * threads decode grid thumbnails, and a WIC factory belongs to the apartment
 * that created it.  __declspec(thread) makes that automatic, so every caller
 * of DecodeToBgra works on its own factory without passing one around. */
static __declspec(thread) IWICImagingFactory *s_wic;

static HRESULT Wic(void)
{
    if (s_wic) return S_OK;
    return CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                            &IID_IWICImagingFactory, (void **)&s_wic);
}

/* ---------------------------------------------------------------- rotation */

void RotateBgra(BYTE **bits, int *pw, int *ph, int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg == 0) return;
    int w = *pw, h = *ph;
    if (w <= 0 || h <= 0) return;
    int nw = (deg == 180) ? w : h;
    int nh = (deg == 180) ? h : w;
    BYTE *src = *bits;
    BYTE *dst = (BYTE *)malloc((size_t)nw * nh * 4);
    if (!dst) return;

    if (deg == 180) {
        for (int y = 0; y < h; y++) {
            const BYTE *s = src + (size_t)y * w * 4;
            BYTE *d = dst + (size_t)(h - 1 - y) * w * 4;
            for (int x = 0; x < w; x++)
                memcpy(d + (size_t)(w - 1 - x) * 4, s + (size_t)x * 4, 4);
        }
    } else if (deg == 90) {
        for (int y = 0; y < h; y++) {
            const BYTE *s = src + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
                memcpy(dst + ((size_t)x * nw + (h - 1 - y)) * 4, s + (size_t)x * 4, 4);
        }
    } else {
        for (int y = 0; y < h; y++) {
            const BYTE *s = src + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
                memcpy(dst + ((size_t)(w - 1 - x) * nw + y) * 4, s + (size_t)x * 4, 4);
        }
    }
    free(src);
    *bits = dst;
    *pw = nw;
    *ph = nh;
}

/* --------------------------------------------------------------- alpha mix */

static void ComposeAlpha(BYTE *bits, int w, int h)
{
    BOOL hasAlpha = FALSE;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        if (bits[i * 4 + 3] != 255) { hasAlpha = TRUE; break; }
    }
    if (!hasAlpha) return;

    const int q = 16;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE *p = bits + ((size_t)y * w + x) * 4;
            int a = p[3];
            if (a == 255) continue;
            int base = (((x / q) + (y / q)) & 1) ? 52 : 42;
            p[0] = (BYTE)((p[0] * a + base * (255 - a)) / 255);
            p[1] = (BYTE)((p[1] * a + base * (255 - a)) / 255);
            p[2] = (BYTE)((p[2] * a + base * (255 - a)) / 255);
            p[3] = 255;
        }
    }
}

/* ------------------------------------------------------------ exif orient */

static int ReadOrientation(IWICBitmapFrameDecode *frame)
{
    IWICMetadataQueryReader *qr = NULL;
    int orient = 0;
    if (FAILED(frame->lpVtbl->GetMetadataQueryReader(frame, &qr)) || !qr) return 0;

    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(qr->lpVtbl->GetMetadataByName(qr, L"/app1/ifd/{ushort=274}", &v))) {
        if (v.vt == VT_UI2) orient = v.uiVal;
        else if (v.vt == VT_UI4) orient = (int)v.ulVal;
        else if (v.vt == VT_I2) orient = v.iVal;
    }
    PropVariantClear(&v);
    qr->lpVtbl->Release(qr);
    return orient;
}

static int OrientationDegrees(int orient)
{
    switch (orient) {
    case 3: return 180;
    case 6: return 90;
    case 8: return 270;
    default: return 0;
    }
}

/* ---------------------------------------------------------------- decoding */

/* Fit into maxW x maxH (0 = unbounded).  Taking both bounds rather than one
 * "long edge" matters: the grid cell is not square, and decoding to the exact
 * drawn size turns the per-frame HALFTONE rescale into a plain 1:1 copy. */
BYTE *DecodeToBgra(const WCHAR *path, UINT maxW, UINT maxH, int *outW, int *outH)
{
    *outW = *outH = 0;
    if (FAILED(Wic())) return NULL;

    IWICBitmapDecoder *dec = NULL;
    if (FAILED(s_wic->lpVtbl->CreateDecoderFromFilename(s_wic, path, NULL, GENERIC_READ,
                                                        WICDecodeMetadataCacheOnDemand, &dec)))
        return NULL;

    IWICBitmapFrameDecode *frame = NULL;
    if (FAILED(dec->lpVtbl->GetFrame(dec, 0, &frame))) {
        dec->lpVtbl->Release(dec);
        return NULL;
    }

    UINT ow = 0, oh = 0;
    frame->lpVtbl->GetSize(frame, &ow, &oh);
    if (!ow || !oh) {
        frame->lpVtbl->Release(frame);
        dec->lpVtbl->Release(dec);
        return NULL;
    }

    UINT tw = ow, th = oh;
    {
        double kw = maxW ? (double)maxW / (double)ow : 1e9;
        double kh = maxH ? (double)maxH / (double)oh : 1e9;
        double k  = kw < kh ? kw : kh;
        if (k > 1.0) k = 1.0;               /* never upscale */
        if (k < 1.0) {
            tw = (UINT)((double)ow * k);
            th = (UINT)((double)oh * k);
            if (!tw) tw = 1;
            if (!th) th = 1;
        }
    }

    BYTE *bits = NULL;

    /* fast path: let the codec decode straight to (near) the wanted size */
    IWICBitmapSourceTransform *tr = NULL;
    if ((tw != ow || th != oh) &&
        SUCCEEDED(frame->lpVtbl->QueryInterface(frame, &IID_IWICBitmapSourceTransform, (void **)&tr)) && tr) {
        UINT cw = tw, ch = th;
        /* GetClosestSize reports the nearest size the codec can produce.  For
         * PNG/BMP/GIF that is the *original* size - there is no scaling - so
         * accepting it blindly turned every thumbnail into a full-size bitmap
         * (3.2 MB each instead of 190 KB) and made the cache thrash. */
        if (SUCCEEDED(tr->lpVtbl->GetClosestSize(tr, &cw, &ch)) && cw && ch &&
            (cw < ow || ch < oh)) {
            /* The codec (JPEG DCT scaling in particular) may round the closest
             * size up to a block boundary, so CopyPixels can write a few pixels
             * more than cw*ch.  Over-allocate a guard band instead of trusting
             * the reported size exactly - a tight buffer here is the classic
             * heap-corruption source that only shows up when many thumbnails of
             * mixed sizes are decoded in one burst. */
            UINT guard = cw + 16, guardH = ch + 16;
            size_t bytes = (size_t)guard * guardH * 4;
            BYTE *buf = (BYTE *)malloc(bytes);
            if (buf) {
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                HRESULT hr = tr->lpVtbl->CopyPixels(tr, NULL, cw, ch, &fmt,
                                                    WICBitmapTransformRotate0,
                                                    cw * 4, (UINT)bytes, buf);
                if (SUCCEEDED(hr)) {
                    bits = buf;
                    tw = cw;
                    th = ch;
                } else {
                    free(buf);
                }
            }
        }
        tr->lpVtbl->Release(tr);
    }

    if (!bits) {
        IWICBitmapSource *src = (IWICBitmapSource *)frame;
        src->lpVtbl->AddRef(src);

        IWICBitmapSource *conv = NULL;
        if (SUCCEEDED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, src, &conv)) && conv) {
            src->lpVtbl->Release(src);
            src = conv;
        }

        IWICBitmapSource *final = src;
        if (tw != ow || th != oh) {
            /* FANT is the prettiest and by far the slowest filter.  It earns
             * its cost on a gentle shrink (viewer, near 1:1) but not when a
             * 24 MP frame is squeezed to a 180 px thumbnail. */
            double scale = (double)tw / (double)(ow ? ow : 1);
            WICBitmapInterpolationMode im =
                (scale >= 0.45) ? WICBitmapInterpolationModeFant
                                : WICBitmapInterpolationModeCubic;
            IWICBitmapScaler *sc = NULL;
            if (SUCCEEDED(s_wic->lpVtbl->CreateBitmapScaler(s_wic, &sc)) &&
                SUCCEEDED(sc->lpVtbl->Initialize(sc, src, tw, th, im))) {
                final = (IWICBitmapSource *)sc;
            } else if (sc) {
                sc->lpVtbl->Release(sc);
                sc = NULL;
            }
        }

        /* The pixel destination is tw x th ONLY when final is the scaler.  If the
         * scaler could not be created, final is still the full-size source, and
         * copying it with a tw*4 stride and a tw*th*4 buffer would write far past
         * the allocation (ow > tw) and corrupt the heap.  In that rare case fall
         * back to copying the source at its own size and shrink afterwards. */
        UINT sw = tw, sh = th, stride = tw * 4;
        if (final == src && (tw != ow || th != oh)) { sw = ow; sh = oh; stride = ow * 4; }

        size_t bytes = (size_t)sw * sh * 4;
        BYTE *buf = (BYTE *)malloc(bytes);
        if (buf) {
            if (SUCCEEDED(final->lpVtbl->CopyPixels(final, NULL, stride, (UINT)bytes, buf))) {
                if (sw != tw || sh != th) {
                    /* the scaler was unavailable: shrink the full-size copy now */
                    BYTE *shrunk = DownscaleBgra(buf, (int)sw, (int)sh, (int)tw, (int)th);
                    free(buf);
                    bits = shrunk;
                } else {
                    bits = buf;
                }
            } else {
                free(buf);
            }
        }
        if (final != src) final->lpVtbl->Release(final);
        src->lpVtbl->Release(src);
    }

    int orient = ReadOrientation(frame);
    frame->lpVtbl->Release(frame);
    dec->lpVtbl->Release(dec);

    if (!bits) return NULL;

    int deg = OrientationDegrees(orient);
    if (deg) RotateBgra(&bits, (int *)&tw, (int *)&th, deg);
    ComposeAlpha(bits, (int)tw, (int)th);

    *outW = (int)tw;
    *outH = (int)th;
    return bits;
}

/* ------------------------------------------------------------- big picture */

void ImageFree(void)
{
    if (g_app.img.bits) free(g_app.img.bits);
    g_app.img.bits = NULL;
    g_app.img.item = -1;
    g_app.img.w = g_app.img.h = 0;
}

/* --------------------------------------------------------- neighbour preload
 * Decoding the next picture while the current one is on screen turns <- / ->
 * from "watch WIC work" into a memcpy.  One worker, two slots (~48 MB each, so
 * a folder of 24 MP photos costs at most ~96 MB), and the same decode path as
 * a normal load so what lands in g_app.img is bit-identical.
 *
 * The slot remembers the path it decoded, not just the index: a rescan can
 * insert files and shift every index under it, and serving the wrong picture
 * would be worse than decoding it again. */
#define PRELOAD_SLOTS 2
#define PRELOAD_IMAGE_MAX (48u * 1024u * 1024u)

typedef struct {
    int      index;
    int      gen;
    unsigned job;
    BOOL     busy;
    WCHAR   *path;
    FILETIME mtime;
    LONGLONG size;
    BYTE    *bits;
    int      w, h;
    ULONGLONG stamp;
} PreSlot;

typedef struct {
    int      index;
    int      gen;
    WCHAR   *path;
    FILETIME mtime;
    LONGLONG size;
} PreReq;

static PreSlot s_pre[PRELOAD_SLOTS];
static CRITICAL_SECTION s_preLock;
static BOOL   s_preLockReady;
static HANDLE s_preWake;          /* worker: one per queued request */
static HANDLE s_preDone;          /* worker -> ui: a slot changed state */
static HANDLE s_preThread;
static volatile LONG s_preQuit;
static PreReq s_preQ[PRELOAD_SLOTS];
static int    s_preQN;
static unsigned s_preJob;

static void PreDropSlot(int i)
{
    free(s_pre[i].path);
    free(s_pre[i].bits);
    memset(&s_pre[i], 0, sizeof(s_pre[i]));
    s_pre[i].index = -1;
}

static DWORD WINAPI PreloadThread(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    for (;;) {
        WaitForSingleObject(s_preWake, INFINITE);

        for (;;) {
            PreReq req;
            EnterCriticalSection(&s_preLock);
            if (s_preQuit) {
                while (s_preQN > 0) free(s_preQ[s_preQN - 1].path), s_preQN--;
                LeaveCriticalSection(&s_preLock);
                CoUninitialize();
                return 0;
            }
            if (s_preQN == 0) { LeaveCriticalSection(&s_preLock); break; }
            req = s_preQ[0];
            memmove(&s_preQ[0], &s_preQ[1],
                    (size_t)(s_preQN - 1) * sizeof(PreReq));
            s_preQN--;

            if (req.gen != ThumbGeneration()) {
                LeaveCriticalSection(&s_preLock);
                free(req.path);
                continue;
            }

            int slot = -1, freeSlot = -1;
            ULONGLONG oldest = ~0ULL;
            for (int i = 0; i < PRELOAD_SLOTS; i++) {
                if (s_pre[i].gen != req.gen || s_pre[i].index < 0) {
                    if (freeSlot < 0) freeSlot = i;
                    continue;
                }
                if (s_pre[i].index == req.index &&
                    _wcsicmp(s_pre[i].path, req.path) == 0) {
                    /* already queued for decode, or already decoded: no work */
                    freeSlot = -1;
                    slot = -2;
                    break;
                }
                if (s_pre[i].stamp < oldest) { oldest = s_pre[i].stamp; slot = i; }
            }
            if (slot == -2) {
                LeaveCriticalSection(&s_preLock);
                free(req.path);
                continue;
            }
            if (freeSlot >= 0) slot = freeSlot;
            if (slot < 0) slot = 0;

            /* The slot keeps its own copy of the path; the request's copy stays
             * worker-local so ImagePreloadClear can free a slot that is still
             * being decoded without yanking the string out from under WIC. */
            size_t plen = wcslen(req.path);
            WCHAR *slotPath = (WCHAR *)malloc((plen + 1) * sizeof(WCHAR));
            if (!slotPath) {
                LeaveCriticalSection(&s_preLock);
                free(req.path);
                continue;
            }
            memcpy(slotPath, req.path, (plen + 1) * sizeof(WCHAR));

            PreDropSlot(slot);
            s_pre[slot].index = req.index;
            s_pre[slot].gen = req.gen;
            s_pre[slot].job = ++s_preJob;
            s_pre[slot].busy = TRUE;
            s_pre[slot].path = slotPath;
            s_pre[slot].mtime = req.mtime;
            s_pre[slot].size = req.size;
            unsigned job = s_pre[slot].job;
            LeaveCriticalSection(&s_preLock);

            int w = 0, h = 0;
            BYTE *bits = NULL;
            __try {
                bits = DecodeToBgra(req.path, MAX_DECODE_SIDE, MAX_DECODE_SIDE, &w, &h);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                bits = NULL;
            }
            free(req.path);

            EnterCriticalSection(&s_preLock);
            if (s_pre[slot].job == job && !s_preQuit) {
                if (bits && (size_t)w * (size_t)h * 4 <= PRELOAD_IMAGE_MAX &&
                    req.gen == ThumbGeneration()) {
                    s_pre[slot].bits = bits;
                    s_pre[slot].w = w;
                    s_pre[slot].h = h;
                    s_pre[slot].stamp = GetTickCount64();
                    bits = NULL;
                }
                if (!s_pre[slot].bits) {          /* failed: nothing to serve */
                    PreDropSlot(slot);
                    s_pre[slot].gen = req.gen;
                }
                s_pre[slot].busy = FALSE;
            }
            LeaveCriticalSection(&s_preLock);
            free(bits);
            SetEvent(s_preDone);
        }
    }
}

static BOOL PreloadStart(void)
{
    if (s_preThread) return TRUE;
    if (!s_preLockReady) {
        InitializeCriticalSection(&s_preLock);
        s_preWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        s_preDone = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!s_preWake || !s_preDone) return FALSE;
        for (int i = 0; i < PRELOAD_SLOTS; i++) s_pre[i].index = -1;
        s_preLockReady = TRUE;
    }
    s_preThread = CreateThread(NULL, 0, PreloadThread, NULL, 0, NULL);
    return s_preThread != NULL;
}

static void PreloadRequest(int index)
{
    Library *lib = &g_app.lib;
    if (index < 0 || !s_preLockReady) return;
    if (index == g_app.img.item) return;      /* already on screen */

    EnterCriticalSection(&lib->lock);
    if (index >= lib->count || lib->items[index].kind != MK_IMAGE) {
        LeaveCriticalSection(&lib->lock);
        return;
    }
    MediaItem *it = &lib->items[index];
    size_t len = wcslen(it->path);
    WCHAR *dup = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));
    if (!dup) { LeaveCriticalSection(&lib->lock); return; }
    memcpy(dup, it->path, (len + 1) * sizeof(WCHAR));
    FILETIME mtime = it->mtime;
    LONGLONG size = it->size;
    int gen = ThumbGeneration();
    LeaveCriticalSection(&lib->lock);

    EnterCriticalSection(&s_preLock);
    for (int i = 0; i < PRELOAD_SLOTS; i++) {
        if (s_pre[i].gen == gen && s_pre[i].index == index &&
            s_pre[i].path && _wcsicmp(s_pre[i].path, dup) == 0) {
            LeaveCriticalSection(&s_preLock);
            free(dup);
            return;
        }
    }
    for (int i = 0; i < s_preQN; i++) {
        if (s_preQ[i].gen == gen && s_preQ[i].index == index &&
            _wcsicmp(s_preQ[i].path, dup) == 0) {
            LeaveCriticalSection(&s_preLock);
            free(dup);
            return;
        }
    }
    while (s_preQN >= PRELOAD_SLOTS) {
        free(s_preQ[0].path);
        memmove(&s_preQ[0], &s_preQ[1],
                (size_t)(s_preQN - 1) * sizeof(PreReq));
        s_preQN--;
    }
    s_preQ[s_preQN].index = index;
    s_preQ[s_preQN].gen = gen;
    s_preQ[s_preQN].path = dup;
    s_preQ[s_preQN].mtime = mtime;
    s_preQ[s_preQN].size = size;
    s_preQN++;
    LeaveCriticalSection(&s_preLock);
    ReleaseSemaphore(s_preWake, 1, NULL);
}

/* Adopt a finished neighbour.  Only a slot whose file identity still matches
 * the item at that index is served; anything else is dropped and decoded
 * normally.  If the slot is being decoded right now, wait for it - that is
 * still much cheaper than decoding the same bytes a second time. */
static BOOL PreloadTake(int index, int *outW, int *outH, BYTE **out)
{
    *out = NULL;
    if (!s_preLockReady) return FALSE;

    ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        EnterCriticalSection(&s_preLock);
        int i;
        for (i = 0; i < PRELOAD_SLOTS; i++) {
            if (s_pre[i].index == index && s_pre[i].gen == ThumbGeneration())
                break;
        }
        if (i >= PRELOAD_SLOTS) { LeaveCriticalSection(&s_preLock); return FALSE; }

        if (!s_pre[i].busy) {
            BOOL same = FALSE;
            Library *lib = &g_app.lib;
            EnterCriticalSection(&lib->lock);
            if (index >= 0 && index < lib->count) {
                MediaItem *it = &lib->items[index];
                same = it->kind == MK_IMAGE && s_pre[i].path &&
                       _wcsicmp(it->path, s_pre[i].path) == 0 &&
                       it->size == s_pre[i].size &&
                       it->mtime.dwHighDateTime == s_pre[i].mtime.dwHighDateTime &&
                       it->mtime.dwLowDateTime == s_pre[i].mtime.dwLowDateTime;
            }
            LeaveCriticalSection(&lib->lock);

            if (same && s_pre[i].bits) {
                *out = s_pre[i].bits;
                *outW = s_pre[i].w;
                *outH = s_pre[i].h;
                s_pre[i].bits = NULL;      /* the caller now owns the pixels */
                PreDropSlot(i);
                s_pre[i].gen = ThumbGeneration();
                LeaveCriticalSection(&s_preLock);
                return TRUE;
            }
            PreDropSlot(i);
            s_pre[i].gen = ThumbGeneration();
            LeaveCriticalSection(&s_preLock);
            return FALSE;
        }

        ResetEvent(s_preDone);
        LeaveCriticalSection(&s_preLock);
        if (GetTickCount64() >= deadline) return FALSE;
        if (WaitForSingleObject(s_preDone, 1000) != WAIT_OBJECT_0 &&
            GetTickCount64() >= deadline)
            return FALSE;
    }
}

static void PreloadNeighbours(int index)
{
    if (g_app.viewCount <= 0) return;
    int next = FilterStep(index, 1);
    int prev = FilterStep(index, -1);
    PreloadRequest(next);
    PreloadRequest(prev);
}

void ImagePreloadClear(void)
{
    if (!s_preLockReady) return;
    EnterCriticalSection(&s_preLock);
    while (s_preQN > 0) free(s_preQ[s_preQN - 1].path), s_preQN--;
    for (int i = 0; i < PRELOAD_SLOTS; i++) PreDropSlot(i);
    s_preJob++;                 /* a decode still in flight must not land */
    LeaveCriticalSection(&s_preLock);
    if (s_preDone) SetEvent(s_preDone);
}

void ImagePreloadShutdown(void)
{
    if (!s_preLockReady) return;
    InterlockedExchange(&s_preQuit, 1);
    ImagePreloadClear();
    if (s_preWake) SetEvent(s_preWake);
    if (s_preThread) {
        if (WaitForSingleObject(s_preThread, 3000) == WAIT_OBJECT_0) {
            CloseHandle(s_preThread);
            s_preThread = NULL;
        }
    }
}

BOOL ImageLoad(int index)
{
    Library *lib = &g_app.lib;
    if (g_app.img.item == index && g_app.img.bits) {
        if (PreloadStart()) PreloadNeighbours(index);
        return TRUE;
    }
    ImageFree();
    g_app.imgFailed = 0;

    if (index < 0 || index >= lib->count) return FALSE;
    MediaItem *it = &lib->items[index];
    if (it->kind != MK_IMAGE) return FALSE;

    int w = 0, h = 0;
    ThumbTouchBusy();      /* a full-size decode outranks the tree walk */
    BYTE *bits = NULL;
    PreloadTake(index, &w, &h, &bits);
    if (!bits) {
        bits = DecodeToBgra(it->path, MAX_DECODE_SIDE, MAX_DECODE_SIDE, &w, &h);
    }
    if (!bits) { g_app.imgFailed = 1; return FALSE; }

    g_app.img.bits = bits;
    g_app.img.w = w;
    g_app.img.h = h;
    g_app.img.item = index;

    if (PreloadStart()) PreloadNeighbours(index);
    return TRUE;
}

/* --------------------------------------------------------------- thumbnails */

static int ThumbFind(int index)
{
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_app.thumbs[i].item == index) return i;
    return -1;
}

static void ThumbDrop(int i)
{
    Thumb *t = &g_app.thumbs[i];
    if (t->bits) {
        g_app.thumbBytes -= (size_t)t->w * (size_t)t->h * 4;
        free(t->bits);
    }
    t->bits = NULL;
    t->item = -1;
    t->w = t->h = 0;
}

static int ThumbVictim(int protect)
{
    int    victim = -1;
    UINT64 oldest = ~0ULL;
    for (int i = 0; i < THUMB_MAX; i++) {
        if (i == protect || g_app.thumbs[i].item < 0) continue;
        if (g_app.thumbs[i].used < oldest) { oldest = g_app.thumbs[i].used; victim = i; }
    }
    return victim;
}

static int ThumbSlot(void)
{
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_app.thumbs[i].item < 0) return i;
    int victim = ThumbVictim(-1);
    if (victim < 0) victim = 0;
    ThumbDrop(victim);
    return victim;
}

/* Eviction is driven by slot count only.  The old byte budget kept its own
 * running total, and once that total drifted the cache evicted every cell it
 * had just decoded: 339 decodes for one screen of thumbnails, 319 of them
 * immediately discarded.  A fixed number of slots cannot drift. */
static int ThumbStore(int index, BYTE *bits, int w, int h)
{
    int s = ThumbSlot();
    g_app.thumbs[s].item = index;
    g_app.thumbs[s].w = w;
    g_app.thumbs[s].h = h;
    g_app.thumbs[s].bits = bits;
    g_app.thumbs[s].used = ++g_app.clock;
    g_app.thumbBytes += (size_t)w * (size_t)h * 4;
    return s;
}

void ThumbMarkBad(int index)
{
    int s = ThumbSlot();
    g_app.thumbs[s].item = index;
    g_app.thumbs[s].w = g_app.thumbs[s].h = 0;
    g_app.thumbs[s].bits = NULL;
    g_app.thumbs[s].used = ++g_app.clock;
}

/* Called on the ui thread with a picture produced elsewhere (video frame). */
void ThumbAccept(int index, BYTE *bits, int w, int h)
{
    if (!bits || w <= 0 || h <= 0) { free(bits); return; }
    if (ThumbFind(index) >= 0) { free(bits); return; }   /* raced: already cached */
    ThumbStore(index, bits, w, h);
}

/* Box filter.  Codecs that can only offer 1/2, 1/4, 1/8 (JPEG) still land
 * above the cell size; one cheap rescale here keeps every later frame a plain
 * 1:1 copy. */
BYTE *DownscaleBgra(const BYTE *src, int sw, int sh, int tw, int th)
{
    if (tw <= 0 || th <= 0 || sw <= 0 || sh <= 0) return NULL;
    BYTE *dst = (BYTE *)malloc((size_t)tw * th * 4);
    if (!dst) return NULL;
    for (int y = 0; y < th; y++) {
        int sy0 = y * sh / th, sy1 = (y + 1) * sh / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = x * sw / tw, sx1 = (x + 1) * sw / tw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            UINT r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const BYTE *row = src + ((size_t)sy * sw + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++, row += 4) {
                    b += row[0]; g += row[1]; r += row[2]; n++;
                }
            }
            BYTE *d = dst + ((size_t)y * tw + x) * 4;
            d[0] = (BYTE)(b / n); d[1] = (BYTE)(g / n); d[2] = (BYTE)(r / n); d[3] = 255;
        }
    }
    return dst;
}

int       g_thumbDecodeN;
ULONGLONG g_thumbDecodeUs;
int       g_thumbCacheHits;
int       g_thumbRequests;

/* ------------------------------------------------------- yielding the disk
 * The walker and the decoders share one device.  A stick serves them in
 * arrival order, so while the tree is being walked every picture the user is
 * waiting for lands behind another directory listing - which is exactly the
 * "打开子文件夹要等半天才出图" complaint.
 *
 * Only an episode that starts on the ui thread can set the stamp, because only
 * the ui knows what the user is looking at.  A cover on the top-level album
 * grid is not worth slowing the walk down for - the grid is waiting for that
 * walk to arrive; a picture inside a folder is, because the walk is not what
 * fills that screen.  Workers only prolong a stamp that already exists, so a
 * long burst stays peaceful until it ends. */
static ULONGLONG s_thumbBusyAt;

static BOOL PicturesAreUrgent(void)
{
    if (g_app.single) return TRUE;              /* one picture, full screen */
    /* Only *inside* a folder.  On the top-level album grid the walk is what
     * puts the cards on screen, so standing it off the disk there would delay
     * the very list the user is scrolling - and a root whose pictures are all
     * loose files would then wait on the walk it is throttling. */
    return g_app.curDir[0] &&
           _wcsicmp(g_app.curDir, g_app.lib.root) != 0;
}

static void StampBusy(void)
{
    InterlockedExchange64((volatile LONG64 *)&s_thumbBusyAt,
                          (LONG64)GetTickCount64());
}

static ULONGLONG BusyAt(void)
{
    return (ULONGLONG)InterlockedCompareExchange64(
               (volatile LONG64 *)&s_thumbBusyAt, 0, 0);
}

void ThumbTouchBusy(void)
{
    if (PicturesAreUrgent()) StampBusy();
}

void ThumbProlongBusy(void)
{
    ULONGLONG at = BusyAt();
    if (at && GetTickCount64() - at < THUMB_YIELD_MS) StampBusy();
}

BOOL ThumbBusy(void)
{
    ULONGLONG at = BusyAt();
    return at && GetTickCount64() - at < THUMB_YIELD_MS;
}

/* Shrink a buffer that the codec handed back larger than the cell.  JPEG can
 * only scale in powers of two, so even the "closest size" is often twice the
 * tile; one box filter here keeps every later frame a plain 1:1 copy. */
static void FitToCell(BYTE **bits, int *w, int *h, int bw, int bh)
{
    if (!*bits || *w <= bw && *h <= bh) return;
    double k = (double)bw / *w;
    if ((double)bh / *h < k) k = (double)bh / *h;
    int tw = (int)(*w * k), th = (int)(*h * k);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    BYTE *shrunk = DownscaleBgra(*bits, *w, *h, tw, th);
    if (!shrunk) return;
    free(*bits);
    *bits = shrunk;
    *w = tw;
    *h = th;
}

/* ----------------------------------------------------------- thumbnail worker
 * Decoding inside the paint is what made a scrolled-to screen feel frozen: the
 * old code spent "at most 9 ms per frame" but always allowed one decode, so a
 * single 24 MP JPEG (40 ms) or a slow read off the stick stalled that whole
 * frame, twenty-four times in a row.  Time spent decoding is time not spent
 * painting, and no budget can take it back afterwards.
 *
 * So the grid never touches a codec now.  A miss asks the pool, draws "…", and
 * the frame that lands (WM_APP_ITHUMB) repaints the cell.  This mirrors the
 * video thumbnail worker in video.cpp, which is why both grids feel the same.
 */
typedef struct {
    int      index;
    int      wantW, wantH;
    int      outW, outH;
    int      gen;              /* cache generation when requested */
    FILETIME mtime;
    LONGLONG size;
    BYTE    *bits;             /* worker -> ui, heap */
    WCHAR    path[1];
} ThumbJob;

/* A queue the ui thread fills and the pool drains, instead of one posted
 * message per cell.  Two reasons:
 *   - it is bounded.  Flying through a folder of thousands of albums used to
 *     post a decode for every cell that crossed the screen, so the pool then
 *     spent seconds reading files the user had already scrolled past.  On a
 *     stick that is the difference between "fills in" and "seems stuck".
 *   - the oldest request is the one that is off screen, so a full queue drops
 *     it and keeps the cell the user is actually looking at.
 * Delivery is still one window message per finished cell. */
#define THUMB_PENDING_MAX 64           /* ~two screens: what can be looked at */

static HANDLE s_twThread[THUMB_WORKERS];
static DWORD  s_twTid[THUMB_WORKERS];
static int    s_twThreads;
static HWND   s_twHwnd;
static int    s_twGen;
static BOOL   s_twQuitSent;
static CRITICAL_SECTION s_twLock;
static BOOL   s_twLockReady;
static HANDLE s_twSem;                     /* one release per queued job */
static ThumbJob *s_twQueue[THUMB_PENDING_MAX];
static int    s_twQueueN;
static int    s_twFlight[THUMB_WORKERS];   /* popped, being decoded */
static int    s_twFlightN;

#define THUMB_QUIT_JOB ((ThumbJob *)1)     /* not dereferenced, just a marker */

static BOOL FlightHas(int index)
{
    for (int i = 0; i < s_twFlightN; i++)
        if (s_twFlight[i] == index) return TRUE;
    return FALSE;
}

static void FlightAdd(int index)
{
    if (s_twFlightN < THUMB_WORKERS) s_twFlight[s_twFlightN++] = index;
}

static void FlightDrop(int index)
{
    for (int i = 0; i < s_twFlightN; i++) {
        if (s_twFlight[i] != index) continue;
        s_twFlight[i] = s_twFlight[--s_twFlightN];
        return;
    }
}

/* the three below expect s_twLock to be held */
static int QueueFind(int index)
{
    for (int i = 0; i < s_twQueueN; i++)
        if (s_twQueue[i] != THUMB_QUIT_JOB && s_twQueue[i]->index == index) return i;
    return -1;
}

static void QueueDropAt(int i)
{
    if (s_twQueue[i] != THUMB_QUIT_JOB) free(s_twQueue[i]);
    memmove(&s_twQueue[i], &s_twQueue[i + 1],
            (size_t)(s_twQueueN - i - 1) * sizeof(ThumbJob *));
    s_twQueueN--;
}

static int CellWantW(void)
{
    int v = g_app.cellW > 0 ? g_app.cellW : S(THUMB_SIZE);
    if (v < 112) v = 112;
    if (v > 320) v = 320;
    return v;
}

static int CellWantH(void)
{
    int v = g_app.cellH > 0 ? g_app.cellH : S(THUMB_SIZE);
    if (v < 96)  v = 96;
    if (v > 320) v = 320;
    return v;
}

/* How many decoders may run at once.  A stick serves one read stream well and
 * two badly: interleaving two decodes makes the controller seek between two
 * files, which is slower than reading them one after the other - and the whole
 * point of this pool is to be gentle with the removable media the app is built
 * for.  Fixed disks have the queue depth for a pair, sticks get one. */
static int WorkerCount(void)
{
    const WCHAR *root = g_app.lib.root;
    if (!root[0] || root[1] != L':') return THUMB_WORKERS;
    WCHAR vol[4] = { root[0], L':', L'\\', 0 };
    UINT t = GetDriveTypeW(vol);
    return (t == DRIVE_REMOVABLE || t == DRIVE_REMOTE) ? 1 : THUMB_WORKERS;
}

static DWORD WINAPI ThumbThread(LPVOID arg)
{
    HANDLE ready = (HANDLE)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* a decode must never outrank painting or input */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetEvent(ready);

    for (;;) {
        WaitForSingleObject(s_twSem, INFINITE);

        ThumbJob *job = NULL;
        EnterCriticalSection(&s_twLock);
        if (s_twQueueN > 0) {
            job = s_twQueue[0];
            memmove(&s_twQueue[0], &s_twQueue[1],
                    (size_t)(s_twQueueN - 1) * sizeof(ThumbJob *));
            s_twQueueN--;
            if (job != THUMB_QUIT_JOB) FlightAdd(job->index);
        }
        LeaveCriticalSection(&s_twLock);

        if (!job) continue;                     /* trimmed away while queued */
        if (job == THUMB_QUIT_JOB) break;

        /* keep the walk off the device for as long as this burst lasts, not
         * just for as long as the queue was filling */
        ThumbProlongBusy();

        LARGE_INTEGER q0, q1, qf;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        /* A truncated download in the middle of a photo album can make a codec
         * fault; on a worker thread that fault would take the process down.
         * A thumbnail must never be able to kill the viewer. */
        __try {
            /* Cache first, decode only on a miss - and the result is written
             * back here, on this thread.  Both halves of the cache traffic
             * used to run inside the paint (load in ThumbGet, store in
             * ThumbDeliver); that is what made scrolling into a folder of
             * thousands of albums stall, because every freshly filled cell
             * paid a disk write and, every 512 of them, a full directory scan
             * before the grid could repaint. */
            job->bits = ThumbCacheLoad(job->path, &job->mtime, job->size,
                                       &job->outW, &job->outH);
            if (job->bits) {
                InterlockedIncrement((volatile LONG *)&g_thumbCacheHits);
                FitToCell(&job->bits, &job->outW, &job->outH,
                          job->wantW, job->wantH);
            } else {
                job->bits = DecodeToBgra(job->path, (UINT)job->wantW,
                                         (UINT)job->wantH,
                                         &job->outW, &job->outH);
                if (job->bits) {
                    FitToCell(&job->bits, &job->outW, &job->outH,
                              job->wantW, job->wantH);
                    ThumbCacheStore(job->path, job->bits, job->outW, job->outH,
                                    &job->mtime, job->size);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            job->bits = NULL;
            job->outW = job->outH = 0;
        }
        QueryPerformanceCounter(&q1);
        InterlockedAdd64((volatile LONG64 *)&g_thumbDecodeUs,
                         (LONG64)((q1.QuadPart - q0.QuadPart) * 1000000LL /
                                  (LONGLONG)qf.QuadPart));
        InterlockedIncrement((volatile LONG *)&g_thumbDecodeN);

        if (!s_twHwnd || !PostMessageW(s_twHwnd, WM_APP_ITHUMB,
                                       (WPARAM)job->index, (LPARAM)job)) {
            EnterCriticalSection(&s_twLock);
            FlightDrop(job->index);
            LeaveCriticalSection(&s_twLock);
            free(job->bits);
            free(job);
        }
    }

    CoUninitialize();
    return 0;
}

static BOOL PoolStart(void)
{
    if (s_twThreads > 0) return TRUE;
    s_twHwnd = g_app.hwnd;

    if (!s_twLockReady) {
        InitializeCriticalSection(&s_twLock);
        s_twSem = CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, NULL);
        if (!s_twSem) return FALSE;
        s_twLockReady = TRUE;
    }

    int want = WorkerCount();
    for (int i = 0; i < want; i++) {
        HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ready) break;
        s_twThread[i] = CreateThread(NULL, 0, ThumbThread, ready, 0, &s_twTid[i]);
        if (!s_twThread[i]) { CloseHandle(ready); break; }
        WaitForSingleObject(ready, 2000);
        CloseHandle(ready);
        s_twThreads++;
    }
    return s_twThreads > 0;
}

/* Ask the pool for one cell.  Cheap, idempotent, safe to call every frame. */
void ThumbRequest(int index)
{
    Library *lib = &g_app.lib;
    if (index < 0 || index >= lib->count) return;
    if (lib->items[index].kind != MK_IMAGE) return;
    if (!PoolStart()) { ThumbMarkBad(index); return; }

    size_t len = wcslen(lib->items[index].path);
    ThumbJob *job = (ThumbJob *)malloc(sizeof(ThumbJob) + len * sizeof(WCHAR));
    if (!job) return;
    job->index = index;
    job->wantW = CellWantW();
    job->wantH = CellWantH();
    job->outW = job->outH = 0;
    job->mtime = lib->items[index].mtime;
    job->size  = lib->items[index].size;
    job->bits  = NULL;
    memcpy(job->path, lib->items[index].path, (len + 1) * sizeof(WCHAR));

    EnterCriticalSection(&s_twLock);
    if (s_twGen != ThumbGeneration()) {    /* different folder: forget the queue */
        s_twGen = ThumbGeneration();
        while (s_twQueueN > 0) QueueDropAt(s_twQueueN - 1);
        s_twFlightN = 0;                   /* the rest carry a stale gen */
    }
    if (QueueFind(index) >= 0 || FlightHas(index)) {   /* already asked for */
        LeaveCriticalSection(&s_twLock);
        free(job);
        return;
    }
    while (s_twQueueN >= THUMB_PENDING_MAX) QueueDropAt(0);   /* off screen */
    job->gen = s_twGen;
    s_twQueue[s_twQueueN++] = job;
    g_thumbRequests++;
    LeaveCriticalSection(&s_twLock);

    /* one release per queued job: whichever worker is free takes it.  A job
     * that got trimmed leaves an extra release behind, which simply wakes a
     * worker that finds nothing - cheaper than tracking the count. */
    ReleaseSemaphore(s_twSem, 1, NULL);
    ThumbTouchBusy();     /* a screenful is wanted: let the walk step aside now */
}

void ThumbDeliver(WPARAM idx, LPARAM p)
{
    ThumbJob *job = (ThumbJob *)p;
    if (!job) return;

    EnterCriticalSection(&s_twLock);
    FlightDrop((int)idx);
    LeaveCriticalSection(&s_twLock);

    if (job->gen == ThumbGeneration()) {
        if (job->bits) {
            /* already persisted by the worker: nothing to write from here */
            ThumbAccept(job->index, job->bits, job->outW, job->outH);
        } else {
            ThumbMarkBad(job->index);   /* do not ask again */
        }
    } else if (job->bits) {
        free(job->bits);
    }
    free(job);
}

void ThumbWorkerShutdown(void)
{
    if (s_twThreads <= 0 || s_twQuitSent) return;
    s_twQuitSent = TRUE;

    /* Nothing queued is worth waiting for at exit: drop the backlog, then park
     * one sentinel per thread at the tail. */
    EnterCriticalSection(&s_twLock);
    while (s_twQueueN > 0) QueueDropAt(s_twQueueN - 1);
    for (int i = 0; i < s_twThreads; i++)
        s_twQueue[s_twQueueN++] = THUMB_QUIT_JOB;
    LeaveCriticalSection(&s_twLock);
    ReleaseSemaphore(s_twSem, s_twThreads, NULL);

    for (int i = 0; i < s_twThreads; i++) {
        if (WaitForSingleObject(s_twThread[i], 3000) != WAIT_OBJECT_0) {
            s_twHwnd = NULL;     /* wedged in a codec: it cannot post any more */
            continue;
        }
        CloseHandle(s_twThread[i]);
        s_twThread[i] = NULL;
    }
    s_twThreads = 0;
    s_twHwnd = NULL;
    s_twQueueN = 0;
    s_twFlightN = 0;
}

BOOL ThumbGet(int index, int *slot)
{
    *slot = -1;
    Library *lib = &g_app.lib;
    if (index < 0 || index >= lib->count) return FALSE;

    int found = ThumbFind(index);
    if (found >= 0) {
        g_app.thumbs[found].used = ++g_app.clock;
        *slot = found;
        return TRUE;
    }
    MediaItem *it = &lib->items[index];
    if (it->kind != MK_IMAGE) return FALSE;

    /* Still flinging?  Anything already in RAM was served above; a cache miss
     * would mean reading a file the user is scrolling past.  On a USB stick
     * those random reads also compete with the folder scan, so wait for the
     * settle repaint (armed by GridScroll) before even asking. */
    if (GetTickCount64() - g_app.lastScroll < SCROLL_SETTLE_MS) return FALSE;

    /* The on-disk cache used to be read from here, in the middle of a paint,
     * once per missing cell per frame: CreateFile + a ~150 KB read + a
     * set-file-time to keep the entry warm, twenty-four times a frame.  On a
     * folder holding thousands of albums that is a real disk round trip inside
     * every frame, and antivirus makes each one cost several times the read.
     * The worker owns BOTH halves of that traffic now - it checks the cache,
     * decodes on a miss, and writes the result - so painting touches nothing
     * but RAM and the ui thread never waits for a disk. */
    ThumbRequest(index);
    return FALSE;
}

/* ----------------------------------------------------------------- drawing */

/* Grid cells.  Pixels live in ordinary heap memory on purpose: a DIB section
 * may be relocated to video memory by the driver, and reading it back from the
 * CPU cost ~1.1 ms per cell here instead of ~15 us. */
void DrawThumb(HDC dc, RECT *rc, const BYTE *bits, int w, int h)
{
    if (!bits || w <= 0 || h <= 0) return;
    int cw = rc->right - rc->left, ch = rc->bottom - rc->top;
    if (cw <= 0 || ch <= 0) return;

    double k = (double)cw / w;
    if ((double)ch / h < k) k = (double)ch / h;
    int dw = (int)(w * k), dh = (int)(h * k);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = rc->left + (cw - dw) / 2;
    int dy = rc->top + (ch - dh) / 2;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    /* 1:1 is the normal case: thumbnails are decoded at exactly the drawn
     * size, so this is a straight DIB write with no resampling. */
    if (dw >= w - 1 && dw <= w + 1 && dh >= h - 1 && dh <= h + 1) {
        SetStretchBltMode(dc, COLORONCOLOR);   /* sticky per DC: HALFTONE here
                                                * would make this 100x slower */
        SetDIBitsToDevice(dc, dx, dy, (DWORD)w, (DWORD)h, 0, 0, 0, (UINT)h,
                          bits, &bi, DIB_RGB_COLORS);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, w, h, bits, &bi, DIB_RGB_COLORS, SRCCOPY);
        SetStretchBltMode(dc, COLORONCOLOR);
    }

}

void DrawBitmapFit(HDC dc, RECT *rc, BYTE *bits, int w, int h, COLORREF bg)
{
    if (!bits || w <= 0 || h <= 0) return;
    int cw = rc->right - rc->left, ch = rc->bottom - rc->top;
    if (cw <= 0 || ch <= 0) return;

    double k = (double)cw / w;
    if ((double)ch / h < k) k = (double)ch / h;
    int dw = (int)(w * k), dh = (int)(h * k);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = rc->left + (cw - dw) / 2;
    int dy = rc->top + (ch - dh) / 2;

    if (bg != CLR_INVALID && (dw < cw || dh < ch)) {
        RECT all = *rc;
        FillRectC(dc, &all, bg);
    }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    if ((dw == w && dh == h) || (dw >= w - 1 && dw <= w + 1 && dh >= h - 1 && dh <= h + 1)) {
        /* already the right size: HALFTONE would only cost us time */
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, w, h, bits, &bi, DIB_RGB_COLORS, SRCCOPY);
        return;
    }
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, w, h, bits, &bi, DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, COLORONCOLOR);
}

void ImageDraw(HDC dc, int dstX, int dstY, int dstW, int dstH)
{
    if (!g_app.img.bits || dstW <= 0 || dstH <= 0) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_app.img.w;
    bi.bmiHeader.biHeight = -g_app.img.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, dstX, dstY, dstW, dstH, 0, 0, g_app.img.w, g_app.img.h,
                  g_app.img.bits, &bi, DIB_RGB_COLORS, SRCCOPY);
}
