/* FastPeek - video playback through MFPlay (EVR into a child window). */
#include "app.h"
#include <mfapi.h>
#include <mfplay.h>
#include <mfidl.h>
#include <mfreadwrite.h>

/* Loaded dynamically: a machine without the media stack must still start. */
typedef HRESULT(WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT(WINAPI *PFN_MFShutdown)(void);

/* Bumped every time the player is torn down; the callback stamps each event
 * with the generation it saw so the UI can drop stale events from a previous
 * player after a next/prev switch.  Declared before PlayerCallback, which uses
 * it. */
static volatile LONG s_gen;
typedef HRESULT(WINAPI *PFN_CreatePlayer)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS,
                                          IMFPMediaPlayerCallback *, HWND, IMFPMediaPlayer **);

class PlayerCallback : public IMFPMediaPlayerCallback
{
public:
    LONG refs = 1;
    /* The generation that was current when *this* player was created.  It is
     * stamped per player, not read from s_gen when the event fires: an event
     * queued by an old player can reach the callback only after the new player
     * has already bumped s_gen, and reading s_gen then would hand the stale
     * event the *new* generation, defeating the guard entirely. */
    LONG gen = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&refs);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER *hdr) override
    {
        if (!hdr || !g_app.hwnd) return;
        /* The player object is torn down and rebuilt on every next/prev, and
         * MFPlay raises events (PLAYBACK_ENDED, ERROR, ...) asynchronously.  An
         * event that was queued for the *old* player can land after the *new*
         * player is already playing; if it were acted on, the stale event would
         * flip g_app.playing / s_ended for the wrong clip.  Each player owns a
         * callback stamped with the generation it was created under, so the UI
         * drops superseded events. */
        LONG evGen = this->gen;
        if (hdr->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED)
            PostMessageW(g_app.hwnd, WM_APP_VIDEO, 1, (LPARAM)evGen);
        else if (hdr->eEventType == MFP_EVENT_TYPE_ERROR)
            PostMessageW(g_app.hwnd, WM_APP_VIDEO, 2, (LPARAM)evGen);
        else if (hdr->eEventType == MFP_EVENT_TYPE_POSITION_SET) {
            /* carry the seek result in the low bits so the UI can tell a
             * failed seek apart from a successful one */
            LPARAM lp = (LPARAM)evGen;
            if (FAILED(hdr->hrEvent)) lp |= (LPARAM)1 << 16;
            PostMessageW(g_app.hwnd, WM_APP_VIDEO, 3, lp);
        }
    }
};

static IMFPMediaPlayer *s_player;
static PlayerCallback  *s_callback;
static BOOL             s_mfReady;
static BOOL             s_mfTried;
static BOOL             s_ended;   /* the last frame has been shown */
static LONGLONG         s_lastPos = -1;   /* last playhead seen by VideoTick */

/* MFPlay has no mute flag, so mute is volume 0 with the level remembered. */
static float EffectiveVolume(void)
{
    return g_app.muted ? 0.0f : (float)g_app.volume / 100.0f;
}

/* set to the path being decoded right now, so a crash inside a codec on an
 * MF worker thread can be attributed to the offending file */
static WCHAR            s_decoding[MAX_PATH_LONG];

static HMODULE          s_hMfplat, s_hMfplay;
static PFN_MFShutdown   s_mfShutdown;
static PFN_CreatePlayer s_createPlayer;

BOOL VideoInit(void)
{
    if (s_mfReady) return TRUE;
    if (s_mfTried) return FALSE;
    s_mfTried = TRUE;

    /* System32 only: a viewer that runs off a USB stick must never pick up a
     * planted mfplat.dll / mfplay.dll from its own folder. */
    s_hMfplat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s_hMfplat) return FALSE;
    PFN_MFStartup startup = (PFN_MFStartup)(void *)GetProcAddress(s_hMfplat, "MFStartup");
    s_mfShutdown = (PFN_MFShutdown)(void *)GetProcAddress(s_hMfplat, "MFShutdown");
    if (!startup || FAILED(startup(MF_VERSION, MFSTARTUP_FULL))) return FALSE;

    s_hMfplay = LoadLibraryExW(L"mfplay.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s_hMfplay) return FALSE;
    s_createPlayer = (PFN_CreatePlayer)(void *)GetProcAddress(s_hMfplay, "MFPCreateMediaPlayer");
    if (!s_createPlayer)
        s_createPlayer = (PFN_CreatePlayer)(void *)GetProcAddress(s_hMfplay, MAKEINTRESOURCEA(2));
    if (!s_createPlayer) return FALSE;

    s_mfReady = TRUE;
    return TRUE;
}

BOOL VideoIsActive(void)
{
    return s_player != NULL;
}

void VideoStop(void)
{
    InterlockedIncrement(&s_gen);   /* any in-flight event from the old player is now stale */
    if (s_player) {
        /* Stop() completes asynchronously and raises MFP_EVENT_TYPE_STOP, which
         * can land *after* a new player has already been created on a rapid
         * next/prev switch - the callback then talks to the wrong player.  The
         * docs' shutdown order is Shutdown() then Release(); Shutdown stops
         * playback and drains the internal queue, and the release count makes
         * the player destroy itself.  Drop the explicit Stop() to avoid the
         * stale async stop event entirely. */
        s_player->Shutdown();
        s_player->Release();
        s_player = NULL;
    }
    if (s_callback) {
        /* The player holds its own reference and drops it while being
         * destroyed; ours is the last one and frees the object. */
        s_callback->Release();
        s_callback = NULL;
    }
    g_app.videoItem = -1;
    g_app.playing = FALSE;
    s_ended = FALSE;
    g_app.seekPending = FALSE;
    s_lastPos = -1;
    if (g_app.hVideo) ShowWindow(g_app.hVideo, SW_HIDE);
}

/* Defined with the worker thread below, but the MF shutdown needs to know
 * whether it is still alive. */
static HANDLE s_vtThread;

void VideoShutdown(void)
{
    VideoStop();
    /* The thumbnail worker decodes through the same media stack.  Calling
     * MFShutdown while it is still inside a codec pulls the runtime out from
     * under it and the process dies on exit; the worker is the last MF user,
     * so leave MF up in that case and let process teardown handle it. */
    if (s_vtThread) return;
    if (s_mfReady && s_mfShutdown) s_mfShutdown();
    s_mfReady = FALSE;
}

/* -------------------------------------------------- input pass-through ---
 * The renderer parks its own window inside the host, so a right-click on the
 * picture would otherwise die there and never reach the stage.  Hook the host
 * and every window the renderer creates underneath it, forward only the
 * right-button release, and let the main window treat it exactly like a click
 * on the surrounding empty area.  Nothing else is intercepted.
 *
 * The original WNDPROC is kept in a window *property* (SetPropW) instead of a
 * side table indexed by HWND.  The EVR tears its child windows down and builds
 * new ones on every resize, and Windows reuses the freed HWND immediately; a
 * side table then hands the new window the *dead* WNDPROC of the old one and
 * CallWindowProcW jumps into freed memory -> access violation -> crash.  A
 * property travels with the window, so a reused HWND starts with a clean
 * property and can never inherit a stale pointer. */
static const WCHAR s_prop[] = L"FP.OldProc";

static WNDPROC HostOld(HWND h)
{
    return (WNDPROC)GetPropW(h, s_prop);
}

static LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = HostOld(h);

    if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
        /* the press is swallowed so the renderer never reacts to it, only the
           release travels on and the stage decides what a right-click means */
        if (msg == WM_RBUTTONUP) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            MapWindowPoints(h, g_app.hwnd, &pt, 1);
            PostMessageW(g_app.hwnd, WM_RBUTTONUP, wp, MAKELPARAM(pt.x, pt.y));
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);  /* restore before the handle dies */
        RemovePropW(h, s_prop);
        return old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }
    return old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

static void HostWrap(HWND h)
{
    if (!h || !IsWindow(h) || HostOld(h)) return;
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)HostProc);
    if (!old) return;
    SetPropW(h, s_prop, (HANDLE)old);
}

static BOOL CALLBACK HostHookChild(HWND h, LPARAM lp)
{
    (void)lp;
    HostWrap(h);
    return TRUE;
}

/* the renderer builds its windows lazily, so this runs again on every resize
 * and on the ui tick: it is a handful of comparisons once everything is hooked */
static void VideoHookInput(void)
{
    if (!g_app.hVideo) return;
    HostWrap(g_app.hVideo);
    EnumChildWindows(g_app.hVideo, HostHookChild, 0);
}

void VideoResize(void)
{
    if (!g_app.hVideo) return;
    RECT rc = g_app.rcGrid;
    /* leave the arrow gutters free so the overlay stays clickable */
    int pad = ViewNavGutter();
    if (rc.right - rc.left - pad * 2 < S(200)) pad = 0;
    rc.left += pad;
    rc.right -= pad;
    SetWindowPos(g_app.hVideo, HWND_TOP, rc.left, rc.top, rc.right - rc.left,
                 rc.bottom - rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    VideoHookInput();
}

BOOL VideoPlay(int index)
{
    Library *lib = &g_app.lib;
    if (index < 0 || index >= lib->count) return FALSE;
    if (lib->items[index].kind != MK_VIDEO) return FALSE;

    VideoStop();
    if (!VideoInit() || !g_app.hVideo) return FALSE;

    RECT rc = g_app.rcGrid;
    if (rc.right - rc.left < 16 || rc.bottom - rc.top < 16) return FALSE;
    VideoResize();

    /* One callback per player, stamped with the generation it was created
     * under.  Keeping our own reference guarantees it outlives the player
     * teardown; the player adds its own on top of that. */
    PlayerCallback *cb = new PlayerCallback();
    if (!cb) return FALSE;
    cb->gen = InterlockedCompareExchange(&s_gen, 0, 0);

    HRESULT hr = s_createPlayer(NULL, FALSE, MFP_OPTION_NONE, cb,
                                g_app.hVideo, &s_player);
    if (FAILED(hr) || !s_player) {
        g_app.videoErr = hr;
        s_player = NULL;
        cb->Release();
        return FALSE;
    }
    s_callback = cb;

    IMFPMediaItem *item = NULL;
    hr = s_player->CreateMediaItemFromURL(lib->items[index].path, TRUE, 0, &item);
    if (FAILED(hr) || !item) {
        g_app.videoErr = hr;
        VideoStop();
        return FALSE;
    }
    hr = s_player->SetMediaItem(item);
    item->Release();
    if (FAILED(hr)) {
        g_app.videoErr = hr;
        VideoStop();
        return FALSE;
    }

    hr = s_player->Play();
    if (FAILED(hr)) {
        g_app.videoErr = hr;
        VideoStop();
        return FALSE;
    }
    s_player->SetVolume(EffectiveVolume());
    g_app.videoItem = index;
    g_app.playing = TRUE;
    g_app.videoErr = S_OK;
    s_ended = FALSE;
    return TRUE;
}

void VideoToggle(void)
{
    if (!s_player) return;
    if (g_app.playing) {
        s_player->Pause();
        g_app.playing = FALSE;
        /* the status strip is the only place the state is visible while a clip
         * plays (the info line steps aside for the seek bar) */
        SetStatus(L"已暂停 · 空格继续");
        return;
    }
    if (s_ended) {
        /* the last frame is showing: restart instead of resuming at the tail */
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = 0;
        HRESULT hrSeek = s_player->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
        PropVariantClear(&pv);
        if (FAILED(hrSeek)) {
            if (g_app.videoItem >= 0) VideoPlay(g_app.videoItem);
            return;
        }
        s_ended = FALSE;
    }
    s_player->Play();
    g_app.playing = TRUE;
    SetStatus(L"正在播放 · 空格暂停");
}

/* called from the ui timer: keeps the info line in sync */
static const WCHAR *StateText(void)
{
    if (g_app.playing) return L"播放中（空格暂停）";
    if (s_ended) return L"播放结束（空格重播）";
    return L"已暂停（空格继续）";
}

/* current playhead + total duration, both in 100 ns units.  Returns FALSE when
 * the player has no useful duration (live / unknown). */
BOOL VideoClock(LONGLONG *pos100ns, LONGLONG *dur100ns)
{
    if (pos100ns) *pos100ns = 0;
    if (dur100ns) *dur100ns = 0;
    if (!s_player) return FALSE;

    LONGLONG dur = 0;
    IMFPMediaItem *item = NULL;
    if (SUCCEEDED(s_player->GetMediaItem(&item)) && item) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        /* NOTE: IMFPMediaItem::GetDuration takes MFP_POSITIONTYPE_100NS as the
         * unit GUID, NOT MF_PD_DURATION (that is a presentation-descriptor
         * attribute GUID and makes GetDuration fail with E_INVALIDARG, leaving
         * the duration at 0 - which is why the seek bar never moved). */
        HRESULT hrDur = item->GetDuration(MFP_POSITIONTYPE_100NS, &pv);
        if (SUCCEEDED(hrDur)) {
            if (pv.vt == VT_UI8) dur = (LONGLONG)pv.uhVal.QuadPart;
            else if (pv.vt == VT_I8) dur = (LONGLONG)pv.hVal.QuadPart;
        }
        PropVariantClear(&pv);
        item->Release();
    }
    if (dur100ns) *dur100ns = dur;

    LONGLONG pos = 0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(s_player->GetPosition(MFP_POSITIONTYPE_100NS, &pv))) {
        if (pv.vt == VT_I8) pos = pv.hVal.QuadPart;
        else if (pv.vt == VT_UI8) pos = (LONGLONG)pv.uhVal.QuadPart;
    }
    PropVariantClear(&pv);
    if (pos100ns) *pos100ns = pos;

    return dur > 0;
}

/* seek to a fraction of the duration (0.0 .. 1.0) */
void VideoSeek(double frac)
{
    if (!s_player) return;
    LONGLONG pos, dur;
    if (!VideoClock(&pos, &dur) || dur <= 0) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    LONGLONG target = (LONGLONG)((double)dur * frac);
    if (target > dur) target = dur;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = target;
    HRESULT hr = s_player->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
    PropVariantClear(&pv);

    if (SUCCEEDED(hr)) {
        s_ended = FALSE;
        /* SetPosition is asynchronous: the position does not move until the
         * MFP_EVENT_TYPE_POSITION_SET event arrives, so GetPosition() still
         * reports the *old* position for a few frames.  If the seek bar kept
         * drawing from GetPosition() it would snap straight back and look like
         * the drag did nothing.  Remember the target and hold the bar there
         * until the event confirms the jump. */
        g_app.seekFrac = frac;
        g_app.seekPending = TRUE;
        /* SetPosition resumes playback on its own when the player was already
         * playing ("If playback was started before SetPosition is called,
         * playback resumes at the new position").  An extra Play() here races
         * the in-flight seek and can snap the position back, which is exactly
         * the "dragging the bar does nothing" symptom.  Leave it to MFPlay. */
        InvalidateRect(g_app.hwnd, &g_app.rcGrid, FALSE);
        InvalidateRect(g_app.hwnd, &g_app.rcStatus, FALSE);   /* seek bar lives here */
    }
}

void VideoAdjustVolume(int delta)
{
    int v = g_app.volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_app.volume = v;
    if (v > 0) g_app.muted = FALSE;
    if (s_player) s_player->SetVolume(EffectiveVolume());
    SetStatus(g_app.muted ? L"音量 %d%%（静音）" : L"音量 %d%%", v);
}

void VideoToggleMute(void)
{
    if (!s_player) return;
    g_app.muted = !g_app.muted;
    s_player->SetVolume(EffectiveVolume());
    SetStatus(g_app.muted ? L"已静音（M 恢复 · +/− 调音量）"
                          : L"取消静音 · 音量 %d%%", g_app.volume);
}

/* relative jump in seconds, clamped to the clip; works while paused too */
void VideoSeekBy(double seconds)
{
    if (!s_player) return;
    LONGLONG pos = 0, dur = 0;
    if (!VideoClock(&pos, &dur) || dur <= 0) return;

    double target = (double)pos + seconds * 10000000.0;
    if (target < 0.0) target = 0.0;
    if (target > (double)dur) target = (double)dur;
    VideoSeek(target / (double)dur);

    WCHAR t[32];
    FormatDuration((LONGLONG)target, t, _countof(t));
    SetStatus(seconds < 0 ? L"后退到 %s" : L"前进到 %s", t);
}

void VideoTick(void)
{
    if (!s_player) return;
    VideoHookInput();

    LONGLONG pos, dur;
    VideoClock(&pos, &dur);

    WCHAR next[192];
    int n = FilterPos(g_app.cur) + 1;
    if (dur > 0) {
        WCHAR t[64];
        FormatDuration(dur, t, _countof(t));
        _snwprintf_s(next, _countof(next), _TRUNCATE, L"%d / %d · 视频 · %s · %s",
                     n, g_app.viewCount, t,
                     StateText());
    } else {
        _snwprintf_s(next, _countof(next), _TRUNCATE, L"%d / %d · 视频 · %s",
                     n, g_app.viewCount,
                     StateText());
    }
    BOOL textChanged = wcscmp(next, g_app.info) != 0;
    if (textChanged)
        wcsncpy_s(g_app.info, _countof(g_app.info), next, _TRUNCATE);

    /* The seek bar draws its own clock, so it has to be repainted whenever the
     * playhead moves - even while the info text (which only carries duration
     * and state) stays identical.  Without this the bar froze for the whole
     * clip because the text comparison never changed. */
    BOOL posMoved = dur > 0 && pos != s_lastPos;
    s_lastPos = pos;
    if (textChanged || posMoved)
        InvalidateRect(g_app.hwnd, &g_app.rcStatus, FALSE);
}

void VideoOnEvent(WPARAM kind, LPARAM data)
{
    /* leaving the player also raises PLAYBACK_ENDED, and the message can land
     * after the grid is already back: only a live player may talk to the
     * status line, otherwise the grid keeps a stale "播放结束" hint.  The
     * generation stamp additionally drops events queued by a *previous* player
     * that arrive after a next/prev switch, so they cannot flip g_app.playing
     * or s_ended for the clip that is actually playing now. */
    if (!s_player || !g_app.single) return;
    if ((LONG)data != InterlockedCompareExchange(&s_gen, 0, 0)) return;

    if (kind == 1) {
        g_app.playing = FALSE;
        s_ended = TRUE;
        SetStatus(L"播放结束 · 按空格重播");
    } else if (kind == 2) {
        /* a decode error stops the clip; without clearing the flag the ui
         * would keep claiming it is playing and space would try to "pause" */
        g_app.playing = FALSE;
        g_app.seekPending = FALSE;
        s_ended = FALSE;
        SetStatus(L"这个视频无法解码（系统缺少解码器）· 按 E 用系统播放器打开");
    } else if (kind == 3) {
        /* SetPosition completed: the playhead now reports the real position,
         * so the seek bar can follow GetPosition() again. */
        g_app.seekPending = FALSE;
        InvalidateRect(g_app.hwnd, &g_app.rcStatus, FALSE);
        if (data & ((LPARAM)1 << 16)) {
            /* the seek itself failed (e.g. the file has no seekable keyframe
             * index) - the playhead is still at the old spot */
            SetStatus(L"无法跳转到该位置 · 请尝试其他时间点");
        } else {
            /* the frame under the player's EVR window can lag a seek, especially
             * while paused; UpdateVideo repaints it to the new position */
            s_player->UpdateVideo();
        }
    }
}

/* ------------------------------------------------------------ grid thumbnails
 * The grid wants a picture, not a film strip: pull one representative frame
 * through IMFSourceReader.  Every entry point is resolved at runtime, so a
 * machine without the media stack still starts and simply shows the plain
 * "视频" card instead. */

typedef HRESULT(WINAPI *PFN_CreateReader)(LPCWSTR, IMFAttributes *, IMFSourceReader **);
typedef HRESULT(WINAPI *PFN_CreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *PFN_CreateMediaType)(IMFMediaType **);

static HMODULE               s_hReader;
static PFN_CreateReader      s_createReader;
static PFN_CreateAttributes  s_createAttrs;
static PFN_CreateMediaType   s_createMediaType;
static BOOL                  s_readerTried;

static BOOL ReaderReady(void)
{
    if (s_createReader) return TRUE;
    if (s_readerTried) return FALSE;
    s_readerTried = TRUE;
    if (!VideoInit()) return FALSE;

    s_hReader = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (s_hReader)
        s_createReader = (PFN_CreateReader)(void *)GetProcAddress(s_hReader,
                                                                 "MFCreateSourceReaderFromURL");
    s_createAttrs    = (PFN_CreateAttributes)(void *)GetProcAddress(s_hMfplat, "MFCreateAttributes");
    s_createMediaType = (PFN_CreateMediaType)(void *)GetProcAddress(s_hMfplat, "MFCreateMediaType");
    return s_createReader && s_createAttrs && s_createMediaType;
}

static void SizeOf(IMFMediaType *mt, int *w, int *h)
{
    UINT64 v = 0;
    if (mt && SUCCEEDED(mt->GetUINT64(MF_MT_FRAME_SIZE, &v))) {
        *w = (int)(v >> 32);
        *h = (int)(v & 0xFFFFFFFF);
    }
}

/* the buffer is RGB32 from the video processor: BGRA in memory, which is
 * exactly the layout our DIBs use */
static void CopyFrame(IMFMediaBuffer *buf, int w, int h, BYTE **out)
{
    if (!buf || w <= 0 || h <= 0) return;
    /* guard against a decoder that reports a corrupted frame size: an 8K frame
     * is 132 MB of RGBA and a *plausible* upper bound for real clips; anything
     * larger is a broken stream and would just overflow the allocation. */
    if (w > 8192 || h > 8192 || (size_t)w * (size_t)h > (size_t)8192 * 8192) return;
    BYTE *dst = (BYTE *)malloc((size_t)w * h * 4);
    if (!dst) return;

    IMF2DBuffer *b2d = NULL;
    if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2d))) && b2d) {
        BYTE *s0 = NULL;
        LONG pitch = 0;
        if (SUCCEEDED(b2d->Lock2D(&s0, &pitch)) && s0) {
            /* pitch can be larger than w*4 (row alignment): copy only w*4 bytes
               per row and never read past the buffer for the last row. */
            int apitch = pitch < 0 ? -pitch : pitch;
            if (apitch >= w * 4) {
                for (int y = 0; y < h; y++) {
                    const BYTE *row = (pitch >= 0) ? s0 + (size_t)y * apitch
                                                   : s0 + (size_t)(h - 1 - y) * (size_t)apitch;
                    memcpy(dst + (size_t)y * w * 4, row, (size_t)w * 4);
                }
                b2d->Unlock2D();
                b2d->Release();
                *out = dst;
                return;
            }
        }
        b2d->Unlock2D();
        b2d->Release();
    }

    BYTE *p = NULL;
    DWORD len = 0;
    if (SUCCEEDED(buf->Lock(&p, NULL, &len)) && p && len >= (DWORD)w * 4) {
        /* len is the whole contiguous buffer; copy at most the frame we asked
           for, never more than the caller-provided buffer can hold. */
        size_t want = (size_t)w * h * 4;
        size_t have = (size_t)len;
        memcpy(dst, p, want < have ? want : have);
        buf->Unlock();
        *out = dst;
        return;
    }
    free(dst);
}

static BOOL ReadOneFrame(IMFSourceReader *reader, int w, int h, BYTE **out)
{
    for (int i = 0; i < 60 && !*out; i++) {
        DWORD flags = 0;
        IMFSample *sample = NULL;
        HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags,
                                        NULL, &sample);
        if (FAILED(hr)) return FALSE;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) sample->Release();
            return FALSE;
        }
        if (!sample) continue;   /* stream tick, no picture yet */

        IMFMediaBuffer *buf = NULL;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
            CopyFrame(buf, w, h, out);
            buf->Release();
        }
        sample->Release();
    }
    return *out != NULL;
}

BYTE *VideoFirstFrame(const WCHAR *path, int wantW, int wantH, int *outW, int *outH)
{
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!path || !path[0] || !ReaderReady()) return NULL;

    wcsncpy_s(s_decoding, _countof(s_decoding), path, _TRUNCATE);

    IMFAttributes *attrs = NULL;
    if (FAILED(s_createAttrs(&attrs, 2)) || !attrs) return NULL;
    /* lets the reader insert the video processor MFT: that is what makes
     * "give me RGB32 at this size" possible for ordinary H.264 files */
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader *reader = NULL;
    HRESULT hr = s_createReader(path, attrs, &reader);
    attrs->Release();
    if (FAILED(hr) || !reader) return NULL;

    /* fit the frame inside the cell box, never upscale a tiny clip */
    int nw = 0, nh = 0;
    IMFMediaType *native = NULL;
    if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)) && native) {
        SizeOf(native, &nw, &nh);
        /* VP8/VP9/AV1 are decoded by msvp9dec_store.dll / mfplat AV1 on an
         * internal Media Foundation work-queue thread (RTWorkQ), not on our
         * ThumbThread.  A damaged or odd-sized clip makes that decoder throw
         * 0xC0000005 on that thread, where no __try/__except around the
         * ReadSample call here can reach it - the whole process dies.  Those
         * codecs are almost never H.264-in-webm, so just skip the preview
         * frame for them: the cell still shows a "视频" card, and the file
         * still *plays* fine through MFPlay's own renderer. */
        GUID sub = GUID_NULL;
        if (SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &sub)) &&
            (sub == MFVideoFormat_VP80 || sub == MFVideoFormat_VP90 ||
             sub == MFVideoFormat_AV1)) {
            native->Release();
            reader->Release();
            return NULL;
        }
        native->Release();
    }
    /* A corrupt container can report a nonsensical frame size (e.g. 0x7FFFFFFF),
     * which then becomes a gigantic RGB32 allocation and, in the worst case, a
     * crash deep inside the decoder.  Clamp to something a real clip can have
     * (8K) before any buffer is sized from these numbers. */
    if (nw <= 0 || nw > 8192 || nh <= 0 || nh > 8192) { nw = nh = 0; }
    int tw = wantW > 0 ? wantW : nw, th = wantH > 0 ? wantH : nh;
    if (nw > 0 && nh > 0 && tw > 0 && th > 0) {
        double k = (double)tw / nw;
        if ((double)th / nh < k) k = (double)th / nh;
        if (k > 1.0) k = 1.0;
        tw = (int)(nw * k) & ~1;
        th = (int)(nh * k) & ~1;
        if (tw < 2) tw = 2;
        if (th < 2) th = 2;
    }
    if (tw <= 0 || th <= 0) { reader->Release(); return NULL; }

    IMFMediaType *mt = NULL;
    if (SUCCEEDED(s_createMediaType(&mt)) && mt) {
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        mt->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)(UINT32)tw << 32) | (UINT32)th);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
        mt->Release();
        if (FAILED(hr)) {
            /* last resort: let the source pick the size, we shrink afterwards */
            IMFMediaType *any = NULL;
            if (SUCCEEDED(s_createMediaType(&any)) && any) {
                any->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                any->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, any))) {
                    any->Release();
                    reader->Release();
                    return NULL;
                }
                any->Release();
            } else {
                reader->Release();
                return NULL;
            }
        }
    } else {
        reader->Release();
        return NULL;
    }

    int w = tw, h = th;
    IMFMediaType *cur = NULL;
    if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
        SizeOf(cur, &w, &h);
        cur->Release();
    }
    if (w <= 0 || h <= 0) { reader->Release(); return NULL; }

    /* a tenth of the way in beats frame zero: no fade-in, no black intro card */
    LONGLONG dur = 0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) dur = (LONGLONG)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
    }
    LONGLONG seek = dur / 10;
    if (seek > 30000000LL) seek = 30000000LL;   /* 3 s in 100 ns units */

    BYTE *bits = NULL;
    if (seek > 0) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = seek;
        if (SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, pos)) && ReadOneFrame(reader, w, h, &bits))
            { /* got it */ }
        PropVariantClear(&pos);
    }
    if (!bits) {
        PROPVARIANT pos;              /* seek refused (or nothing there): rewind */
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = 0;
        reader->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
        ReadOneFrame(reader, w, h, &bits);
    }

    reader->Release();

    /* the video processor does not always honour the requested size (some
     * sources hand back the native frame), and a 1280x720 buffer would cost
     * 3.7 MB per cell in the cache: shrink it here, once. */
    if (bits && (w > wantW || h > wantH) && wantW > 0 && wantH > 0) {
        double k = (double)wantW / w;
        if ((double)wantH / h < k) k = (double)wantH / h;
        int fw = (int)(w * k), fh = (int)(h * k);
        if (fw < 1) fw = 1;
        if (fh < 1) fh = 1;
        BYTE *shrunk = DownscaleBgra(bits, w, h, fw, fh);
        if (shrunk) {
            free(bits);
            bits = shrunk;
            w = fw;
            h = fh;
        }
    }

    if (bits) {
        if (outW) *outW = w;
        if (outH) *outH = h;
    }
    return bits;
}

const WCHAR *VideoDecodingPath(void)
{
    return s_decoding;
}

/* --------------------------------------------------- worker thread plumbing
 * A 1080p frame costs ~80 ms end to end (demux, seek, decode, scale), which
 * would blow the 9 ms per-frame budget and turn every scroll into a stutter.
 * The ui thread therefore only *asks*; the frame arrives later as a message. */

typedef struct VJob {
    int   index;
    int   wantW, wantH;
    int   outW, outH;
    int   gen;
    FILETIME mtime;      /* source identity, so the frame can be cached too */
    LONGLONG size;
    BYTE *bits;
    WCHAR path[1];
} VJob;

#define VT_JOB  (WM_APP + 5)
#define VT_QUIT (WM_APP + 6)

static HANDLE         s_vtReady;
static DWORD          s_vtTid;
static HWND           s_vtHwnd;
static BOOL           s_vtQuitSent;   /* a shutdown already asked; do not wait twice */
static int            s_vtPending[64];
static int            s_vtPendingN;
static int            s_vtGen = -1;

static void PendingDrop(int index)
{
    for (int i = 0; i < s_vtPendingN; i++) {
        if (s_vtPending[i] == index) {
            s_vtPending[i] = s_vtPending[--s_vtPendingN];
            return;
        }
    }
}

static BOOL PendingHas(int index)
{
    for (int i = 0; i < s_vtPendingN; i++)
        if (s_vtPending[i] == index) return TRUE;
    return FALSE;
}

/* Serve a video cell from the on-disk thumbnail cache when a previous visit
 * already decoded this exact file.  Repeat visits to a video folder then cost
 * a local disk read instead of pulling the clip off the stick again.
 *
 * This runs on the thumbnail worker, not on the ui thread: reading (and
 * storing) the cache from inside a paint is a disk round trip per cell, which
 * is what made a big folder crawl.  Every cache buffer is shrunk to the cell
 * first, because an entry may come from a different window or DPI. */
static void FitFrame(BYTE **bits, int *w, int *h, int wantW, int wantH)
{
    if (!*bits || wantW <= 0 || wantH <= 0) return;
    if (*w <= wantW && *h <= wantH) return;
    double k = (double)wantW / *w;
    if ((double)wantH / *h < k) k = (double)wantH / *h;
    int fw = (int)(*w * k), fh = (int)(*h * k);
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    BYTE *shrunk = DownscaleBgra(*bits, *w, *h, fw, fh);
    if (shrunk) { free(*bits); *bits = shrunk; *w = fw; *h = fh; }
}

static DWORD WINAPI ThumbThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* decoding a frame means reading a whole video segment off the stick:
     * never let that outrank painting or input */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* create the queue */
    SetEvent(s_vtReady);

    for (;;) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0 || msg.message == VT_QUIT) break;
        if (msg.message != VT_JOB) continue;

        VJob *job = (VJob *)msg.lParam;
        if (!job) continue;

        ThumbProlongBusy();    /* pulling a segment off the stick: the walk waits */

        LARGE_INTEGER q0, q1, qf;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        /* A broken / malicious / 4K-then-corrupt clip can make a codec or the
         * media engine itself throw (0xC0000005 deep inside mfplat / the GPU
         * driver).  On a worker thread that unhandled fault is promoted all the
         * way to the process-wide filter and takes the whole app down with it.
         * A thumbnail must never be allowed to kill the viewer: trap the fault
         * here and treat the clip as undecodable (a plain "视频" card). */
        __try {
            /* cache first: a repeat visit costs a local read, never the codec -
             * and both the read and the store happen here, so the grid can
             * repaint while the stick is being touched */
            job->bits = ThumbCacheLoad(job->path, &job->mtime, job->size,
                                       &job->outW, &job->outH);
            if (job->bits) {
                FitFrame(&job->bits, &job->outW, &job->outH, job->wantW, job->wantH);
            } else {
                job->bits = VideoFirstFrame(job->path, job->wantW, job->wantH,
                                            &job->outW, &job->outH);
                if (job->bits)
                    ThumbCacheStore(job->path, job->bits, job->outW, job->outH,
                                    &job->mtime, job->size);
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

        if (!s_vtHwnd || !PostMessageW(s_vtHwnd, WM_APP_VTHUMB,
                                       (WPARAM)job->index, (LPARAM)job))
            free(job);
    }

    CoUninitialize();
    return 0;
}

static BOOL ThreadStart(void)
{
    if (s_vtThread) return TRUE;
    s_vtHwnd = g_app.hwnd;
    s_vtQuitSent = FALSE;
    s_vtReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s_vtReady) return FALSE;
    s_vtThread = CreateThread(NULL, 0, ThumbThread, NULL, 0, &s_vtTid);
    if (!s_vtThread) return FALSE;
    WaitForSingleObject(s_vtReady, 2000);
    CloseHandle(s_vtReady);
    s_vtReady = NULL;
    return TRUE;
}

/* Serve a video cell from the on-disk thumbnail cache when a previous visit
 * already decoded this exact file, so a repeat visit costs a local read
 * instead of pulling the clip off the stick again.  See FitFrame above. */
void VideoThumbRequest(int index)
{
    Library *lib = &g_app.lib;
    if (index < 0 || index >= lib->count) return;
    if (lib->items[index].kind != MK_VIDEO) return;

    if (s_vtGen != ThumbGeneration()) {   /* different folder: forget the queue */
        s_vtGen = ThumbGeneration();
        s_vtPendingN = 0;
    }
    if (PendingHas(index) || s_vtPendingN >= (int)_countof(s_vtPending)) return;

    if (!ReaderReady() || !ThreadStart()) return;   /* no media stack: plain card */

    size_t len = wcslen(lib->items[index].path);
    VJob *job = (VJob *)malloc(sizeof(VJob) + len * sizeof(WCHAR));
    if (!job) return;
    job->index = index;
    job->wantW = g_app.cellW > 0 ? g_app.cellW : 160;
    job->wantH = g_app.cellH > 0 ? g_app.cellH : 120;
    job->outW = job->outH = 0;
    job->gen = s_vtGen;
    job->mtime = lib->items[index].mtime;
    job->size = lib->items[index].size;
    job->bits = NULL;
    memcpy(job->path, lib->items[index].path, (len + 1) * sizeof(WCHAR));

    if (!PostThreadMessageW(s_vtTid, VT_JOB, 0, (LPARAM)job)) {
        free(job);
        return;
    }
    s_vtPending[s_vtPendingN++] = index;
    ThumbTouchBusy();      /* same rule as the stills: pictures win while seen */
}

void VideoThumbDeliver(WPARAM idx, LPARAM p)
{
    VJob *job = (VJob *)p;
    if (!job) return;

    PendingDrop((int)idx);
    if (job->gen == ThumbGeneration()) {
        if (job->bits) {
            /* the worker already persisted it */
            ThumbAccept(job->index, job->bits, job->outW, job->outH);
        } else {
            ThumbMarkBad(job->index);   /* do not ask again */
        }
    } else if (job->bits) {
        free(job->bits);
    }
    free(job);
}

void VideoThumbShutdown(void)
{
    if (!s_vtThread || s_vtQuitSent) return;
    s_vtQuitSent = TRUE;

    /* VT_QUIT queues behind a job that is already decoding, so a clip in
     * progress finishes before the thread goes away. */
    PostThreadMessageW(s_vtTid, VT_QUIT, 0, 0);

    if (WaitForSingleObject(s_vtThread, 3000) != WAIT_OBJECT_0) {
        /* Wedged inside a codec.  Keep the handle so VideoShutdown() knows MF
         * must stay loaded, and drop the window so the thread cannot post
         * into a destroyed one. */
        s_vtHwnd = NULL;
        return;
    }
    CloseHandle(s_vtThread);
    s_vtThread = NULL;
    s_vtTid = 0;
    s_vtHwnd = NULL;
    s_vtPendingN = 0;
}
