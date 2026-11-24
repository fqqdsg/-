/* FastPeek - a native, zero-install image/video browser for USB sticks.
 *
 * Design notes
 * ------------
 *  - One exe, static CRT, no runtime install, no browser, no server, no temp
 *    files: everything is decoded in RAM and thrown away on exit.  The stick
 *    is only ever *read*, never written to.
 *  - Startup is two phase: the window appears immediately with the cached
 *    folder, a worker thread walks the tree and posts progress, the grid paints
 *    whatever is ready.  Nothing blocks the UI thread.
 *  - Thumbnails decode lazily, only for cells that are actually on screen, at a
 *    bounded rate so scrolling never stutters.  They live in a small LRU cache.
 */
#ifndef FASTPEEK_APP_H
#define FASTPEEK_APP_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#define APP_CLASS   L"FastPeekWindow"
#define APP_TITLE   L"FastPeek"
#define WM_APP_SCAN  (WM_APP + 1)   /* worker -> ui: new items available */
#define WM_APP_VIDEO (WM_APP + 3)   /* media engine callback            */
#define WM_APP_VTHUMB (WM_APP + 4)  /* worker -> ui: video frame is ready */
#define WM_APP_ITHUMB (WM_APP + 5)  /* worker -> ui: picture thumb is ready */

#define MAX_PATH_LONG 1024

/* ---------------------------------------------------------------- palette */
#define C_BG        RGB(22, 23, 26)
#define C_PANEL     RGB(36, 38, 43)
#define C_PANEL_HI  RGB(54, 57, 64)
#define C_PANEL_ON  RGB(70, 74, 84)
#define C_LINE      RGB(56, 59, 66)
#define C_TEXT      RGB(230, 232, 238)
#define C_TEXT_DIM  RGB(138, 144, 156)
#define C_ACCENT    RGB(78, 156, 255)
#define C_ACCENT_D  RGB(58, 122, 210)
#define C_VIDEO     RGB(10, 10, 12)

/* -------------------------------------------------------------- media item */
typedef enum { MK_IMAGE = 0, MK_VIDEO = 1 } MediaKind;

typedef struct {
    WCHAR     *path;      /* owned, full path                       */
    WCHAR     *name;      /* points inside path, after last '\\'   */
    LONGLONG   size;
    FILETIME   mtime;
    MediaKind  kind;
} MediaItem;

/* ----------------------------------------------------------------- library */
typedef struct {
    MediaItem       *items;
    MediaItem       *retire;    /* pre-sort array waiting for the UI to free it */
    int              count;
    int              cap;
    WCHAR            root[MAX_PATH_LONG];
    CRITICAL_SECTION lock;
    HANDLE           thread;
    volatile LONG    scanning;      /* 1 while worker is walking       */
    volatile LONG    cancel;
    volatile LONG    notifyPending; /* coalesce WM_APP_SCAN posts      */
    volatile LONG    gen;            /* bumped per scan, tags its posts */
    int              dirs;
} Library;

/* ----------------------------------------------------------------- folders
 * The walk is recursive, so one Library already holds the whole tree.  The
 * folder view groups the *immediate* sub-directories of g_app.curDir and draws
 * each one as a card covered by one picture from inside it.  Entering a card
 * only re-filters the existing list - no rescan, no extra disk I/O - and
 * right-click walks back up.  See folders.c. */
/* One cover per card.  A 1-4 picture contact sheet looked busier, but each
 * extra tile is another decode for a card 118 px wide: four times the random
 * reads on the stick and four times the thumbnails pinned in the LRU cache,
 * for detail that is cropped away anyway. */
#define FOLDER_PREVIEW 1
#define MAX_FOLDERS    8192

typedef struct {
    WCHAR *path;               /* owned, full path of the directory      */
    WCHAR *name;               /* points inside path, after last '\\'    */
    int    count;              /* media files anywhere below it          */
    int    preview[FOLDER_PREVIEW];   /* the cover picture               */
    int    nPreview;
} FolderEntry;

/* ------------------------------------------------------ remembered places
 * Every grid the user leaves leaves a mark behind: the scroll offset and the
 * selection it was showing, keyed by directory *and* mode (the card grid and
 * the file grid of one folder are two different grids).  Backing out with a
 * right click then lands on the row the wheel was parked on, instead of
 * snapping to the top of a fifty-album list. */
#define VIEW_MARK_MAX 48

typedef struct {
    WCHAR *dir;                /* owned, full path of the directory */
    int    folderMode;         /* which of its two grids this is    */
    int    scrollY;
    int    selFolder;
    int    sel;
} ViewMark;

/* ------------------------------------------------------------- thumbnails */
#define SCAN_NOTIFY_MS  90       /* do not repaint faster than this while scanning */

/* How far below the opened folder the walker descends.  Two levels of
 * sub-folders is all an album tree needs to be browsable, and it is what keeps
 * a whole drive from being read end to end: point the app at D:\ or C:\ and it
 * walks two levels and stops, instead of crawling Program Files, node_modules
 * and a phone backup for minutes with the drive pinned.  Deeper folders are
 * not indexed - to browse one, open it itself (it becomes the root) and two
 * more levels come with it.  Raise this number if you really want a deep tree
 * indexed in one go; the walker is bounded by SCAN_MAX_DEPTH and ITEM_LIMIT. */
#define SCAN_MAX_DEPTH  2
/* A wheel fling crosses several screens; decoding a thumbnail for every cell
 * that flies past is pure wasted random I/O on a USB stick.  Grid cells wait
 * this long after the last scroll before they are even requested, and a
 * one-shot timer repaints the grid so the deferred work actually starts. */
#define SCROLL_SETTLE_MS 120
#define IDT_VIDEO        1
#define IDT_THUMB_SETTLE 2
#define IDT_SCROLLBENCH  3   /* TEMP: --scrollbench timer */
#define IDT_SLIDESHOW    4

/* Grid ordering.  The Library itself stays natural-name-sorted (folder cards
 * and the cover pick depend on it); only the viewMap is reordered. */
#define SORT_NAME 0   /* natural: IMG_2 before IMG_10 */
#define SORT_DATE 1   /* newest first */
#define SORT_SIZE 2   /* largest first */

#define SEARCH_MAX 96
#define THUMB_MAX   160      /* ~200 KB each at 150% DPI: 160 -> ~32 MB */
#define THUMB_SIZE  256

/* The scan and the thumbnails read the same device.  On a stick that is one
 * queue: every directory the walker reads pushes the picture the user is
 * waiting for further back, which is why opening an album while the tree was
 * still being walked felt so slow.  So the walker stands off the disk for a
 * short while after every thumbnail read (see ThumbTouchBusy) and only resumes
 * once the screen has been quiet for a moment.  Cap: a long album must not be
 * able to freeze the scan for good, so the walk always creeps forward. */
#define THUMB_YIELD_MS   900     /* quiet time before the walk resumes     */
#define SCAN_YIELD_MAX_MS 2500   /* ...but never hold it back longer than this */
/* Pictures are decoded on a small worker pool, never while painting: one 24 MP
 * JPEG off a stick is 20-60 ms, and no per-frame budget can undo a stall that
 * has already happened.  Two threads halve the fill-in time for a screenful of
 * misses without turning the stick into a random-read storm. */
#define THUMB_WORKERS 2

typedef struct {
    int     item;      /* -1 = empty slot */
    int     w, h;
    BYTE   *bits;      /* w*h*4, BGRA top-down, heap */
    UINT64  used;
} Thumb;

typedef struct {
    int    item;
    int    w, h;
    BYTE  *bits;
} ImageBitmap;

/* ------------------------------------------------------------------ layout */
typedef enum {
    BT_NONE = 0,
    BT_OPEN, BT_UP, BT_PREV, BT_NEXT,
    BT_ZOOM_OUT, BT_ZOOM_IN, BT_FIT, BT_ONE,
    BT_GRID, BT_SINGLE, BT_ROTATE, BT_PLAY, BT_EXPLORE, BT_FILTER, BT_FOLDERS,
    BT_SORT, BT_SLIDE
} BtnId;

typedef struct { BtnId id; RECT rc; BOOL enabled; } Button;

/* --------------------------------------------------------------------- app */
#define MAX_BUTTONS 16

typedef struct {
    HINSTANCE  inst;
    HWND       hwnd;
    HWND       hVideo;        /* child window handed to the media engine */
    UINT       dpi;

    Library    lib;
    int        cur;           /* single view index, -1 = none */
    int        sel;           /* grid selected index           */
    int        hover;         /* grid hovered index, -1 none   */
    BOOL       single;        /* view mode                     */

    int        filter;        /* 0 all, 1 images, 2 videos     */
    int        sortMode;      /* SORT_NAME / SORT_DATE / SORT_SIZE */
    int        viewCount;     /* items visible under the filter */
    int       *viewMap;       /* viewCount real item indices   */

    /* Name filter, typed straight into the window (Ctrl+F or '/').  It is a
     * substring test on the file name, case-insensitive, applied on top of the
     * media-kind filter; the folder cards count and preview only what matches. */
    WCHAR      search[SEARCH_MAX];
    BOOL       searchActive;  /* the keyboard is typing into search[] */
    int        thumbScale;    /* grid density, percent of the default cell */

    /* folder browser: curDir is the directory the grid reflects.  It starts as
     * the scan root and only ever moves *down* through folder cards, so the
     * walker never has to run again just to change levels. */
    WCHAR       curDir[MAX_PATH_LONG];
    int         folderMode;   /* grid renders folder cards instead of files */
    int         folderAuto;   /* the next finished scan picks folderMode    */
    int         selFolder;    /* selected cell in the folder grid, -1 none  */
    int         hoverFolder;
    FolderEntry *folders;
    int         folderCount;
    int         folderCap;
    int         folderFiles;              /* media files anywhere below curDir */
    int         filePreview[FOLDER_PREVIEW];  /* its own cover, for that card */
    int         fileNPreview;
    UINT64      folderAt;     /* entry time, swallows the 2nd click of a dblclick */
    ViewMark    marks[VIEW_MARK_MAX];     /* grids left behind, oldest first */
    int         markCount;

    int        scrollY;
    int        cellW, cellH, cols, rows;
    int        contentH;
    BOOL       sbDrag;
    int        sbOff;
    ULONGLONG  lastScroll;   /* GetTickCount64 of the last scroll, throttles video thumbs */

    double     zoom;
    BOOL       fit;
    int        panX, panY;
    BOOL       dragging;
    POINT      dragOrigin;
    int        dragPanX, dragPanY;

    int        rotation;      /* 0/90/180/270 for images */

    Thumb      thumbs[THUMB_MAX];
    size_t     thumbBytes;
    UINT64     clock;      /* LRU stamp, bumped by every cache touch */
    UINT64     frame;      /* one per repaint, drives the decode budget  */
    ImageBitmap img;
    int        imgFailed;

    HDC        memDC;
    HBITMAP    memBmp;
    int        memW, memH;

    HFONT      fontUI, fontSmall, fontBold;
    HBRUSH     brBg, brPanel;

    Button     buttons[MAX_BUTTONS];
    int        nButtons;
    int        hoverBtn;
    int        navHot;        /* single-view arrow under the cursor: 0 prev, 1 next, -1 none */
    UINT64     singleAt;      /* GetTickCount64 when single view was entered (dblclick guard) */

    RECT       rcToolbar, rcGrid, rcStatus, rcScroll;

    WCHAR      status[300];
    WCHAR      info[160];
    BOOL       scanning;

    int        startX, startY, startW, startH;
    BOOL       fullscreen;
    RECT       savedPlace;
    LONG_PTR   savedStyle;
    WCHAR      pendingFile[MAX_PATH_LONG];

    BOOL       playing;
    int        videoItem;     /* item index loaded in the player, -1 none */
    HRESULT    videoErr;
    int        volume;        /* 0..100, remembered between runs */
    BOOL       muted;
    BOOL       seekDrag;      /* progress-bar scrub is in progress */
    double     seekFrac;      /* current scrub fraction (mirrors the playhead) */
    BOOL       seekPending;   /* a SetPosition is in flight: show target, not GetPosition */

    BOOL       showInfo;      /* I toggles the file-info card in single view */
    BOOL       slideshow;     /* advancing on a timer */
    int        slideSec;      /* seconds between slides */
} App;

#ifdef __cplusplus
extern "C" {
#endif

extern App g_app;

/* ------------------------------------------------------------------- util */
int    S(int v);
int    SF(double v);
void   UtilInit(HFONT *ui, HFONT *fSmall, HFONT *fBold);
void   SetStatus(const WCHAR *fmt, ...);
WCHAR *PathFileName(WCHAR *path);
BOOL   PathIsDir(const WCHAR *path);
void   PathJoin(WCHAR *dst, size_t cap, const WCHAR *dir, const WCHAR *name);
void   PathParent(WCHAR *path);
void   PathNormalize(WCHAR *path);
int    NaturalCompare(const WCHAR *a, const WCHAR *b);
void   FormatSize(LONGLONG bytes, WCHAR *out, size_t cap);
void   FormatDuration(LONGLONG ticks100ns, WCHAR *out, size_t cap);
void   FormatFileTime(const FILETIME *ft, WCHAR *out, size_t cap);
void   GdiCacheFree(void);
void   FillRectC(HDC dc, RECT *rc, COLORREF c);
void   FrameRectC(HDC dc, RECT *rc, COLORREF c);
void   RoundRectC(HDC dc, RECT *rc, int radius, COLORREF fill, COLORREF line);
void   DrawTextC(HDC dc, const WCHAR *text, RECT *rc, COLORREF color, HFONT font, UINT flags);
int    TextWidth(HDC dc, const WCHAR *text, HFONT font);

/* ------------------------------------------------------------- settings io */
void   SettingsLoad(void);
void   SettingsSave(void);
BOOL   PickFolder(HWND owner, WCHAR *out, size_t cap);
void   OpenFolderInExplorer(const WCHAR *path);
void   ShowInExplorer(const WCHAR *path);
void   OpenWithShell(const WCHAR *path);

/* ------------------------------------------------------------------ scan */
BOOL   ScanStart(const WCHAR *root);   /* FALSE: old walker would not stop */
BOOL   ScanStop(void);                 /* TRUE: the worker is gone          */
void   ScanRestart(void);
void   ScanSortDone(void);
BOOL   IsMediaFile(const WCHAR *name, MediaKind *kind);

/* ---------------------------------------------------------------- filter
 * The scan collects every media file, but the grid can show a subset.  A
 * rebuilt viewMap holds the real indices of the items that pass the filter,
 * so g_app.cur / g_app.sel stay real indices while the grid, paging and the
 * item count all work on the filtered set. */
void   FilterRebuild(void);       /* rebuild viewMap from the current filter */
void   FilterSet(int filter);     /* switch filter (0 all / 1 image / 2 video) */
int    FilterPos(int real);       /* position of a real index in viewMap, -1 */
int    FilterStep(int real, int delta);  /* next/prev visible real index, -1 */
/* TRUE when the file name passes the typed search (always TRUE when empty).
 * FoldersRebuild uses it so card counts and covers follow the search too. */
BOOL   SearchMatchName(const WCHAR *name);

/* ---------------------------------------------------------------- folders */
void   FoldersFree(void);
void   FoldersRebuild(void);      /* immediate sub-dirs of g_app.curDir */
FolderEntry *FolderAt(int index); /* NULL when out of range             */
/* Remembered grid positions: save the grid being left, then apply the mark for
 * the directory/mode just arrived at (a no-op when it has never been seen). */
void   DirMarkSave(void);
void   DirMarkRestore(void);
void   DirMarksClear(void);
/* TRUE when path is dir itself or lives below it; rel gets the remainder. */
BOOL   PathIsUnder(const WCHAR *dir, const WCHAR *path, const WCHAR **rel);
/* Sub-folder levels from dir down to path (0 = dir itself, -1 = not under). */
int    PathDepthUnder(const WCHAR *dir, const WCHAR *path);
int    FolderCells(void);         /* cards the folder grid will draw    */
BOOL   CellIsFilesRow(int cell);  /* the "everything below here" card   */
void   AppEnterFolder(int cell);  /* open the card at that grid position */
BOOL   AppLeaveFolder(void);      /* one level up inside the scan root  */
void   AppSetFolderMode(int on);
void   FolderStep(int delta);     /* keyboard selection in the folder grid */
void   FolderSelect(int cell);    /* absolute selection, clamped */
void   EnsureFolderVisible(int cell);
int    HitTestFolder(POINT pt);   /* folder card under the cursor, -1 none */
int    GridHot(void);             /* hovered cell of whichever grid is up */

/* ------------------------------------------------------------------ image */
BOOL   ImageLoad(int index);            /* full size into g_app.img */
void   ImageFree(void);
BOOL   ThumbGet(int index, int *slot);  /* -1 when not cached */
/* A grid cell that misses the cache never decodes inline: it asks the worker
 * pool (ThumbRequest) and draws a placeholder, then the delivered frame
 * repaints the cell.  Same split as VideoThumbRequest / WM_APP_VTHUMB. */
void   ThumbRequest(int index);
void   ThumbDeliver(WPARAM idx, LPARAM job);
void   ThumbWorkerShutdown(void);
/* Any thread that is about to read a picture off the library device stamps
 * this.  The scan worker watches it and stays off the disk until the stamp is
 * older than THUMB_YIELD_MS, so the pictures the user is looking at win the
 * queue.  Only the ui starts an episode (it knows whether the user is looking
 * at pictures or at the album list); a worker only prolongs one. */
void   ThumbTouchBusy(void);     /* ui thread: a picture is being waited for */
void   ThumbProlongBusy(void);   /* worker: keep an episode alive             */
BOOL   ThumbBusy(void);          /* scan worker: should I step aside?         */
BYTE  *DecodeToBgra(const WCHAR *path, UINT maxW, UINT maxH, int *outW, int *outH);
void   RotateBgra(BYTE **bits, int *pw, int *ph, int deg);
/* The neighbour on each side of the current picture is decoded on a worker so
 * left/right is close to instant.  Budgeted: an image whose pixels would exceed
 * the per-image cap is simply not preloaded, and the slot pool has a total cap. */
void   ImagePreloadClear(void);        /* drop cached neighbours + pending work */
void   ImagePreloadShutdown(void);     /* stop the worker before COM uninit    */
void   ImageDraw(HDC dc, int dstX, int dstY, int dstW, int dstH);
void   DrawBitmapFit(HDC dc, RECT *rc, BYTE *bits, int w, int h, COLORREF bg);
void   DrawThumb(HDC dc, RECT *rc, const BYTE *bits, int w, int h);

/* ------------------------------------------------------------------ video */
BOOL   VideoInit(void);
void   VideoShutdown(void);
BOOL   VideoPlay(int index);
void   VideoStop(void);
void   VideoToggle(void);
void   VideoResize(void);
void   VideoTick(void);
void   VideoOnEvent(WPARAM kind, LPARAM data);
BOOL   VideoIsActive(void);
BOOL   VideoClock(LONGLONG *pos100ns, LONGLONG *dur100ns);  /* both optional */
void   VideoSeek(double frac);   /* 0.0 .. 1.0 of the duration */
void   VideoSeekBy(double seconds);   /* relative jump, clamped to the clip */
void   VideoAdjustVolume(int delta);  /* percent steps, 0..100 */
void   VideoToggleMute(void);
/* one representative frame as BGRA in heap memory, NULL when the file has no
 * decodable video stream (caller owns the buffer) */
BYTE  *VideoFirstFrame(const WCHAR *path, int wantW, int wantH, int *outW, int *outH);
const WCHAR *VideoDecodingPath(void);   /* the file being decoded right now */

/* Video frames cost ~80 ms each, far more than the per-frame decode budget, so
 * they are pulled on a worker thread and handed back through WM_APP_VTHUMB. */
void   VideoThumbRequest(int index);
void   VideoThumbDeliver(WPARAM idx, LPARAM job);
void   VideoThumbShutdown(void);
void   ThumbAccept(int index, BYTE *bits, int w, int h);
BYTE  *DownscaleBgra(const BYTE *src, int sw, int sh, int tw, int th);
void   ThumbMarkBad(int index);
int    ThumbGeneration(void);   /* bumped whenever the cache is dropped */

/* ------------------------------------------------------------ thumb cache
 * On-disk thumbnail cache (see thumbcache.c).  Load returns a BGRA buffer the
 * caller owns, or NULL when there is no valid entry.  Store persists one. */
BYTE  *ThumbCacheLoad(const WCHAR *path, const FILETIME *mtime, LONGLONG size,
                      int *outW, int *outH);
void   ThumbCacheStore(const WCHAR *path, const BYTE *bits, int w, int h,
                       const FILETIME *mtime, LONGLONG size);

/* ------------------------------------------------------------------- view */
void   ViewLayout(void);
void   ViewPaint(HDC dc);
void   ViewOnSize(void);
void   ViewOnWheel(int delta, POINT pt);
void   ViewOnDown(POINT pt);
void   ViewOnMove(POINT pt);
void   ViewOnUp(void);
void   ViewOnDoubleClick(POINT pt);
/* "up one level" gesture: TRUE when a right-click was consumed leaving the
 * single view.  Full screen is unwound by the caller first, so right-clicking a
 * full screen picture lands on the plain grid in one step. */
BOOL   ViewOnRightClick(void);
int    HitTestGrid(POINT pt);
int    ViewNavHit(POINT pt);   /* 0 prev arrow, 1 next arrow, -1 none */
int    ViewNavGutter(void);    /* px reserved on each side for the arrows */
void   EnsureVisible(int index);
void   GridScroll(int dy);
void   ZoomAt(double factor, POINT pt);
void   ZoomFit(void);
void   DrawSeekBar(HDC dc);   /* video progress bar, drawn on the status strip */
/* geometry of the seek bar inside the status strip; FALSE when no video is
 * playing (or the strip is too narrow).  ChromePaint uses it to place the
 * ordinary status text beside the bar. */
BOOL   SeekBarRect(RECT *out);

/* ----------------------------------------------------------------- chrome */
void   ChromeLayout(void);
void   ChromePaint(HDC dc);
BtnId  ChromeHit(POINT pt);
void   ChromeUpdateButtons(void);
int    ChromeToolbarHeight(void);

/* -------------------------------------------------------------------- app */
void   BenchLog(const char *fmt, ...);
extern int       g_thumbDecodeN;
extern ULONGLONG g_thumbDecodeUs;
extern int       g_thumbCacheHits;   /* served from the on-disk cache */
extern int       g_thumbRequests;    /* jobs queued for the pool */
void   PaintStatsReset(void);
int    PaintStatsCount(void);
ULONGLONG PaintStatsAvg(void);
ULONGLONG PaintStatsWorst(void);
void   AppOpenFolder(const WCHAR *folder);
void   AppOpenPath(const WCHAR *path);
void   AppGoto(int index, BOOL toast);
void   AppNext(int delta);
void   AppSetSingle(BOOL on);
void   AppRefresh(void);
void   AppInvalidate(void);
void   AppRotate(int degrees);
void   AppUpdateInfo(void);
/* copies item[index].path into out (cap WCHARs) under the library lock, so a
 * caller can then use the string outside the lock while the walker may be
 * reallocating the item array.  Returns FALSE if index is out of range. */
BOOL   ItemPathOf(int index, WCHAR *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FASTPEEK_APP_H */
