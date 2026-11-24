/* FastPeek - the two views: thumbnail grid and single picture stage. */
#include "app.h"

static int Pad(void)   { return S(14); }
static int Gap(void)   { return S(12); }
static int CapH(void)  { return S(26); }
static int BoxH(void)  { return g_app.cellH + CapH(); }
static int SbW(void)   { return S(10); }

static int HitTestCell(POINT pt);   /* grid position under the cursor, -1 none */

/* ------------------------------------------------------------- geometry */

static RECT CellRect(int index)
{
    int pad = Pad(), gap = Gap(), boxH = BoxH();
    int col = index % g_app.cols, row = index / g_app.cols;
    RECT r;
    r.left   = g_app.rcGrid.left + pad + col * (g_app.cellW + gap);
    r.top    = g_app.rcGrid.top + pad + row * (boxH + gap) - g_app.scrollY;
    r.right  = r.left + g_app.cellW;
    r.bottom = r.top + boxH;
    return r;
}

static void ScrollMetrics(int *maxScroll)
{
    int availH = g_app.rcGrid.bottom - g_app.rcGrid.top;
    int m = g_app.contentH - availH;
    *maxScroll = m > 0 ? m : 0;
}

void ViewLayout(void)
{
    int pad = Pad(), gap = Gap(), boxH = BoxH();
    int availW = (g_app.rcGrid.right - g_app.rcGrid.left) - pad * 2 - SbW();
    int availH = g_app.rcGrid.bottom - g_app.rcGrid.top;
    if (availW < g_app.cellW) availW = g_app.cellW;

    g_app.cols = (availW + gap) / (g_app.cellW + gap);
    if (g_app.cols < 1) g_app.cols = 1;

    int n = g_app.folderMode ? FolderCells() : g_app.viewCount;
    g_app.rows = n ? (n + g_app.cols - 1) / g_app.cols : 0;
    g_app.contentH = n ? (pad * 2 + g_app.rows * (boxH + gap) - gap) : 0;

    int maxScroll;
    ScrollMetrics(&maxScroll);
    if (g_app.scrollY > maxScroll) g_app.scrollY = maxScroll;
    if (g_app.scrollY < 0) g_app.scrollY = 0;

    g_app.rcScroll.left = g_app.rcGrid.right - SbW();
    g_app.rcScroll.right = g_app.rcGrid.right;
    g_app.rcScroll.top = g_app.rcGrid.top;
    g_app.rcScroll.bottom = g_app.rcGrid.bottom;

    (void)availH;
}

void GridScroll(int dy)
{
    int maxScroll;
    ScrollMetrics(&maxScroll);
    g_app.scrollY += dy;
    if (g_app.scrollY > maxScroll) g_app.scrollY = maxScroll;
    if (g_app.scrollY < 0) g_app.scrollY = 0;
    g_app.lastScroll = GetTickCount64();   /* video thumbs wait for the wheel to settle */
    /* While the wheel keeps moving, thumbnails are not decoded (see ThumbGet).
     * Re-arm a one-shot timer on every step: when it finally fires, nothing
     * has moved for SCROLL_SETTLE_MS and the deferred work can start. */
    if (g_app.hwnd) SetTimer(g_app.hwnd, IDT_THUMB_SETTLE, SCROLL_SETTLE_MS + 40, NULL);
    AppInvalidate();
}

static void EnsureCellVisible(int vi);

void EnsureVisible(int index)
{
    if (index < 0 || g_app.cols < 1) return;
    int vi = FilterPos(index);         /* real index -> grid position */
    if (vi < 0) return;
    EnsureCellVisible(vi);
}

/* shared by both grids: scroll so that row of cell vi is fully on screen */
static void EnsureCellVisible(int vi)
{
    if (vi < 0 || g_app.cols < 1) return;
    int pad = Pad(), gap = Gap(), boxH = BoxH();
    int row = vi / g_app.cols;
    int top = pad + row * (boxH + gap);
    int h = g_app.rcGrid.bottom - g_app.rcGrid.top;

    if (top - Gap() < g_app.scrollY) g_app.scrollY = top - Gap();
    else if (top + boxH + Gap() > g_app.scrollY + h) g_app.scrollY = top + boxH + Gap() - h;
    GridScroll(0);
}

void EnsureFolderVisible(int cell)
{
    EnsureCellVisible(cell);
}

int HitTestGrid(POINT pt)
{
    int vi = HitTestCell(pt);
    if (vi < 0 || g_app.folderMode) return -1;
    return g_app.viewMap[vi];              /* hand back the real item index */
}

int HitTestFolder(POINT pt)
{
    if (!g_app.folderMode) return -1;
    return HitTestCell(pt);                /* folder cards use cell numbers */
}

int GridHot(void)
{
    return g_app.folderMode ? g_app.hoverFolder : g_app.hover;
}

/* grid position under the cursor, -1 when the point misses every cell */
static int HitTestCell(POINT pt)
{
    int count = g_app.folderMode ? FolderCells() : g_app.viewCount;
    if (count <= 0) return -1;
    int pad = Pad(), gap = Gap(), boxH = BoxH();
    if (pt.x < g_app.rcGrid.left + pad || pt.x > g_app.rcGrid.right - SbW()) return -1;

    int relX = pt.x - g_app.rcGrid.left - pad;
    int relY = pt.y - g_app.rcGrid.top - pad + g_app.scrollY;
    if (relX < 0 || relY < 0) return -1;

    int col = relX / (g_app.cellW + gap);
    int row = relY / (boxH + gap);
    if (col >= g_app.cols) return -1;
    if (relX % (g_app.cellW + gap) > g_app.cellW) return -1;
    if (relY % (boxH + gap) > boxH) return -1;

    int vi = row * g_app.cols + col;
    if (vi < 0 || vi >= count) return -1;
    return vi;
}

/* ---------------------------------------------------------------- zoom */

static void SingleGeom(RECT *out, double *zoomOut)
{
    int cw = g_app.rcGrid.right - g_app.rcGrid.left;
    int ch = g_app.rcGrid.bottom - g_app.rcGrid.top;
    int iw = g_app.img.w, ih = g_app.img.h;
    if (iw <= 0 || ih <= 0) { SetRectEmpty(out); *zoomOut = 1.0; return; }

    double z = g_app.zoom;
    if (g_app.fit) {
        double zx = (double)(cw - S(24)) / iw;
        double zy = (double)(ch - S(24)) / ih;
        z = zx < zy ? zx : zy;
        if (z > 1.0) z = 1.0;           /* never upscale on "fit" */
        if (z <= 0.0) z = 0.01;
        g_app.zoom = z;
    }
    int dw = (int)(iw * z + 0.5), dh = (int)(ih * z + 0.5);
    int x = g_app.rcGrid.left + (cw - dw) / 2 + g_app.panX;
    int y = g_app.rcGrid.top + (ch - dh) / 2 + g_app.panY;
    out->left = x; out->top = y; out->right = x + dw; out->bottom = y + dh;
    *zoomOut = z;
}

static void ClampPan(void)
{
    int cw = g_app.rcGrid.right - g_app.rcGrid.left;
    int ch = g_app.rcGrid.bottom - g_app.rcGrid.top;
    double z = g_app.zoom;
    int dw = (int)(g_app.img.w * z), dh = (int)(g_app.img.h * z);
    int mx = dw > cw ? (dw - cw) / 2 + S(40) : 0;
    int my = dh > ch ? (dh - ch) / 2 + S(40) : 0;
    if (g_app.panX > mx) g_app.panX = mx;
    if (g_app.panX < -mx) g_app.panX = -mx;
    if (g_app.panY > my) g_app.panY = my;
    if (g_app.panY < -my) g_app.panY = -my;
}

void ZoomFit(void)
{
    g_app.fit = TRUE;
    g_app.panX = g_app.panY = 0;
    AppUpdateInfo();
    AppInvalidate();
}

void ZoomAt(double factor, POINT pt)
{
    if (!g_app.img.bits) return;
    int cw = g_app.rcGrid.right - g_app.rcGrid.left;
    int ch = g_app.rcGrid.bottom - g_app.rcGrid.top;
    double z = g_app.zoom;
    double nz = z * factor;
    if (nz < 0.05) nz = 0.05;
    if (nz > 16.0) nz = 16.0;
    if (nz == z) return;

    double left = g_app.rcGrid.left + (cw - g_app.img.w * z) / 2.0 + g_app.panX;
    double top  = g_app.rcGrid.top + (ch - g_app.img.h * z) / 2.0 + g_app.panY;
    double px = (pt.x - left) / z;
    double py = (pt.y - top) / z;

    g_app.fit = FALSE;
    g_app.zoom = nz;
    g_app.panX = (int)(pt.x - px * nz - g_app.rcGrid.left - (cw - g_app.img.w * nz) / 2.0);
    g_app.panY = (int)(pt.y - py * nz - g_app.rcGrid.top - (ch - g_app.img.h * nz) / 2.0);
    ClampPan();
    AppUpdateInfo();
    AppInvalidate();
}

/* ---------------------------------------------------------------- paint */

static BOOL ThumbCachedBad(int index)
{
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_app.thumbs[i].item == index) return g_app.thumbs[i].bits == NULL;
    return FALSE;
}

static void DrawPlayGlyph(HDC dc, int cx, int cy, int size, COLORREF c)
{
    POINT p[3];
    p[0].x = cx - size / 2; p[0].y = cy - size / 2;
    p[1].x = cx - size / 2; p[1].y = cy + size / 2;
    p[2].x = cx + size / 2; p[2].y = cy;
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ  ob = SelectObject(dc, br);
    HGDIOBJ  op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
}

/* A flat white triangle disappears on a bright frame, so the play badge sits
 * on a soft translucent disc.  The disc is one 128 px DIB built on first use
 * and AlphaBlended at whatever size the cell needs. */
static void DrawPlayBadge(HDC dc, int cx, int cy, int size)
{
    enum { BD = 128 };
    static HBITMAP s_bmp;
    static HDC     s_dc;
    static BOOL    s_tried;

    if (!s_tried) {
        s_tried = TRUE;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = BD;
        bi.bmiHeader.biHeight      = -BD;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;
        s_bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        s_dc  = CreateCompatibleDC(NULL);
        if (s_bmp && s_dc && bits) {
            SelectObject(s_dc, s_bmp);
            DWORD *px = (DWORD *)bits;
            const double r = BD / 2.0 - 2.0;
            for (int y = 0; y < BD; y++) {
                for (int x = 0; x < BD; x++) {
                    double dx = x - BD / 2.0 + 0.5, dy = y - BD / 2.0 + 0.5;
                    double d = sqrt(dx * dx + dy * dy);
                    double a = d > r ? 0.0 : (d > r - 1.5 ? (r - d) / 1.5 : 1.0);
                    if (a <= 0.0) { px[y * BD + x] = 0; continue; }
                    /* premultiplied BGRA, 150/255 opaque */
                    BYTE a8 = (BYTE)(a * 150.0);
                    BYTE c8 = (BYTE)(10 * a8 / 255);
                    px[y * BD + x] = ((DWORD)a8 << 24) | ((DWORD)c8 << 16) |
                                     ((DWORD)c8 << 8) | c8;
                }
            }
        } else {
            s_bmp = NULL;
            if (s_dc) { DeleteDC(s_dc); s_dc = NULL; }
        }
    }

    int d = size > 0 ? size : S(46);
    if (s_dc) {
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, cx - d / 2, cy - d / 2, d, d, s_dc, 0, 0, BD, BD, bf);
    }
    DrawPlayGlyph(dc, cx + d / 22, cy, (int)(d * 0.40), RGB(255, 255, 255));
}

static void DrawScrollBar(HDC dc, int availH)
{
    int maxScroll;
    ScrollMetrics(&maxScroll);
    if (maxScroll <= 0 || g_app.contentH <= 0) return;
    int trackH = g_app.rcGrid.bottom - g_app.rcGrid.top - S(8);
    if (trackH <= 0) return;
    int thumbH = trackH * availH / g_app.contentH;
    if (thumbH < S(30)) thumbH = S(30);
    int travel = trackH - thumbH;
    int ty = g_app.rcGrid.top + S(4) +
             (maxScroll ? (int)((LONGLONG)travel * g_app.scrollY / maxScroll) : 0);
    RECT t = { g_app.rcScroll.left + S(2), ty, g_app.rcScroll.right - S(2), ty + thumbH };
    RoundRectC(dc, &t, S(4), C_PANEL_ON, CLR_INVALID);
}

static void DrawGrid(HDC dc)
{
    Library *lib = &g_app.lib;
    /* the walker reallocs lib->items under lock while it streams entries in;
       the repaint triggered by a mid-scan notify reads the same array, so hold
       the lock for the whole pass.  It is reentrant, so the nested reads in
       ThumbGet / VideoThumbRequest are safe. */
    EnterCriticalSection(&lib->lock);
    int pad = Pad(), gap = Gap(), boxH = BoxH();
    int availH = g_app.rcGrid.bottom - g_app.rcGrid.top;

    RECT body = g_app.rcGrid;
    FillRectC(dc, &body, C_BG);

    if (g_app.viewCount == 0) {
        RECT r = g_app.rcGrid;
        r.top += (availH / 2) - S(46);
        r.bottom = r.top + S(28);
        WCHAR searchMsg[MAX_PATH_LONG];
        const WCHAR *msg = g_app.scanning ? L"正在扫描…" :
                           (g_app.search[0] ? searchMsg :
                            (g_app.filter ? L"没有符合筛选条件的文件" : L"这里没有图片或视频"));
        if (g_app.search[0] && !g_app.scanning)
            _snwprintf_s(searchMsg, _countof(searchMsg), _TRUNCATE,
                         L"没有匹配「%s」的文件", g_app.search);
        DrawTextC(dc, msg, &r, C_TEXT_DIM, g_app.fontUI, DT_CENTER | DT_SINGLELINE);
        r.top += S(30); r.bottom += S(30);
        DrawTextC(dc, g_app.search[0] ? L"Backspace 修改搜索 · Esc 清除" :
                                       L"把文件夹拖到这个窗口，或按 O 选择文件夹",
                  &r, C_TEXT_DIM, g_app.fontSmall, DT_CENTER | DT_SINGLELINE);
        LeaveCriticalSection(&lib->lock);
        return;
    }

    int firstRow = (g_app.scrollY - pad) / (boxH + gap);
    if (firstRow < 0) firstRow = 0;
    int lastRow = (g_app.scrollY + availH) / (boxH + gap) + 1;

    int first = firstRow * g_app.cols;
    int last = (lastRow + 1) * g_app.cols;
    if (last > g_app.viewCount) last = g_app.viewCount;

    /* a fling (wheel + scrollbar drag) keeps the grid moving for a while; only
       start the expensive decodes / video-frame requests once it has been still
       a moment, so we never touch files for cells the user merely flew over.
       On a USB stick each of those is a random read that also slows the scan. */
    BOOL scrollSettled = (GetTickCount64() - g_app.lastScroll) >= SCROLL_SETTLE_MS;
    SetStretchBltMode(dc, COLORONCOLOR);   /* reset sticky HALFTONE from earlier frames */

    for (int vi = first; vi < last; vi++) {
        int i = g_app.viewMap[vi];         /* real item index for this cell */
        RECT r = CellRect(vi);
        if (r.bottom < g_app.rcGrid.top || r.top > g_app.rcGrid.bottom) continue;

        BOOL sel = (i == g_app.sel);
        BOOL hov = (i == g_app.hover);

        RECT box = { r.left, r.top, r.right, r.top + g_app.cellH };
        RECT card = box;
        InflateRect(&card, S(5), S(5));

        RoundRectC(dc, &card, S(8), hov ? C_PANEL_HI : C_PANEL, CLR_INVALID);
        if (sel) {
            RECT ring = card;
            FrameRectC(dc, &ring, C_ACCENT);
            RECT ring2 = ring; InflateRect(&ring2, 1, 1);
            FrameRectC(dc, &ring2, C_ACCENT);
        }

        if (lib->items[i].kind == MK_VIDEO) {
            int slot = -1;
            BOOL got = ThumbGet(i, &slot) && slot >= 0 && g_app.thumbs[slot].bits != NULL;
            FillRectC(dc, &box, C_VIDEO);
            if (got) {
                Thumb *th = &g_app.thumbs[slot];
                DrawThumb(dc, &box, th->bits, th->w, th->h);
            } else if (!ThumbCachedBad(i) && scrollSettled) {
                /* a video frame costs ~80 ms: ask the worker, never decode here.
                   While the wheel is still moving the user is just flying past,
                   so defer the request until the scroll settles - this keeps a
                   fast fling from flooding the worker queue with hundreds of
                   jobs for cells the user already left behind. */
                VideoThumbRequest(i);
            }
            DrawPlayBadge(dc, (box.left + box.right) / 2, (box.top + box.bottom) / 2, S(52));
            RECT tag = box;
            tag.left = box.right - S(42);
            tag.top = box.top + S(6);
            tag.bottom = tag.top + S(18);
            RoundRectC(dc, &tag, S(4), RGB(0, 0, 0), CLR_INVALID);
            DrawTextC(dc, L"视频", &tag, RGB(230, 230, 230), g_app.fontSmall,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            int slot = -1;
            /* ThumbGet serves RAM hits and on-disk cache hits; a genuine miss
             * is handed to the decode pool and answers "not yet".  It never
             * decodes while we are painting, so this frame stays at blit speed
             * and the cell fills in on the repaint its delivery triggers. */
            if (ThumbGet(i, &slot) && slot >= 0 && g_app.thumbs[slot].bits) {
                Thumb *th = &g_app.thumbs[slot];
                DrawThumb(dc, &box, th->bits, th->w, th->h);
            } else if (ThumbCachedBad(i)) {
                RECT m = box;
                DrawTextC(dc, L"无法预览", &m, C_TEXT_DIM, g_app.fontSmall,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                RECT m = box;
                DrawTextC(dc, L"…", &m, C_TEXT_DIM, g_app.fontUI,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        RECT cap = { r.left - S(4), r.top + g_app.cellH + S(6), r.right + S(4), r.top + boxH };
        DrawTextC(dc, lib->items[i].name, &cap, sel ? C_TEXT : C_TEXT_DIM,
                  g_app.fontSmall, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_TOP);
    }

    /* slim scrollbar */
    int maxScroll;
    ScrollMetrics(&maxScroll);
    if (maxScroll > 0 && g_app.contentH > 0) {
        DrawScrollBar(dc, availH);
    }

    LeaveCriticalSection(&lib->lock);
}

/* ------------------------------------------------------------ folder grid */

/* Centre-crop instead of letterboxing: a contact sheet built from letterboxed
 * pictures is all seams and wasted bands.  The card is about the picture, so
 * the source is cropped to whatever the destination rect's aspect wants.
 * The stretch mode is left alone here: DrawFolders picks one for the whole
 * card grid (HALFTONE while still, COLORONCOLOR while moving) because toggling
 * it per blit costs far more than the filter itself. */
static void DrawCover(HDC dc, RECT *rc, const BYTE *bits, int w, int h)
{
    if (!bits || w <= 0 || h <= 0) return;
    int cw = rc->right - rc->left, ch = rc->bottom - rc->top;
    if (cw <= 0 || ch <= 0) return;

    double k = (double)cw / w;
    if ((double)ch / h > k) k = (double)ch / h;      /* fill, then crop */
    int sw = (int)(cw / k), sh = (int)(ch / k);
    if (sw > w) sw = w;
    if (sh > h) sh = h;
    int sx = (w - sw) / 2, sy = (h - sh) / 2;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(dc, rc->left, rc->top, cw, ch, sx, sy, sw, sh,
                  bits, &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* A card's picture: one image from the folder, filling the tile.  This used to
 * be a 1-4 picture contact sheet, but every extra tile cost another full decode
 * of a file on the stick - four random reads per card, four thumbnails held in
 * the LRU cache - for detail that is cropped down to a 118 px square anyway.
 * One cover says the same thing and costs a quarter as much.
 * The decode itself happens on the worker pool (ThumbGet hands it over), so
 * this either blits a ready cover or leaves the blank grey tile. */
static void DrawCoverCell(HDC dc, RECT *box, const int *prev, int n)
{
    if (n <= 0 || prev[0] < 0) return;

    FillRectC(dc, box, C_PANEL_HI);       /* the blank while the cover decodes */
    int slot = -1;
    if (ThumbGet(prev[0], &slot) && slot >= 0 && g_app.thumbs[slot].bits) {
        Thumb *th = &g_app.thumbs[slot];
        DrawCover(dc, box, th->bits, th->w, th->h);
    }
}

static void DrawFolderGlyph(HDC dc, int cx, int cy, int size)
{
    int w = size, h = size * 72 / 100;
    RECT body = { cx - w / 2, cy - h / 2 + S(5), cx + w / 2, cy + h / 2 + S(5) };
    RECT tab  = { body.left, body.top - S(9), body.left + w * 38 / 100, body.top + S(1) };
    FillRectC(dc, &tab, C_PANEL_ON);
    FillRectC(dc, &body, C_PANEL_ON);
}

static void DrawFolders(HDC dc)
{
    Library *lib = &g_app.lib;
    EnterCriticalSection(&lib->lock);

    int pad = Pad(), gap = Gap(), boxH = BoxH();
    int availH = g_app.rcGrid.bottom - g_app.rcGrid.top;

    RECT body = g_app.rcGrid;
    FillRectC(dc, &body, C_BG);

    int cells = FolderCells();
    if (cells == 0) {
        RECT r = g_app.rcGrid;
        r.top += (availH / 2) - S(46);
        r.bottom = r.top + S(28);
        DrawTextC(dc, L"这个文件夹里没有图片或视频", &r, C_TEXT_DIM, g_app.fontUI,
                  DT_CENTER | DT_SINGLELINE);
        LeaveCriticalSection(&lib->lock);
        return;
    }

    int firstRow = (g_app.scrollY - pad) / (boxH + gap);
    if (firstRow < 0) firstRow = 0;
    int lastRow = (g_app.scrollY + availH) / (boxH + gap) + 1;
    int first = firstRow * g_app.cols;
    int last = (lastRow + 1) * g_app.cols;
    if (last > cells) last = cells;

    BOOL scrollSettled = (GetTickCount64() - g_app.lastScroll) >= SCROLL_SETTLE_MS;
    int lead = g_app.folderFiles > 0 ? 1 : 0;   /* the "own files" card, if any */

    /* One stretch mode for the whole card grid.  HALFTONE is the right filter
     * for shrinking a 240 px thumbnail into a 118 px tile, but a screenful of
     * cards is ~100 blits and toggling the mode around each of them costs more
     * than the filtering does - so it is set once here.  While the grid is
     * still moving the cheap mode wins: motion hides the difference and the
     * settle repaint sharpens everything. */
    SetStretchBltMode(dc, scrollSettled ? HALFTONE : COLORONCOLOR);
    if (scrollSettled) SetBrushOrgEx(dc, 0, 0, NULL);

    for (int vi = first; vi < last; vi++) {
        RECT r = CellRect(vi);
        if (r.bottom < g_app.rcGrid.top || r.top > g_app.rcGrid.bottom) continue;

        BOOL files = CellIsFilesRow(vi);
        FolderEntry *e = files ? NULL : FolderAt(vi - lead);
        if (!files && !e) continue;

        BOOL sel = (vi == g_app.selFolder);
        BOOL hov = (vi == g_app.hoverFolder);

        RECT box = { r.left, r.top, r.right, r.top + g_app.cellH };
        RECT card = box;
        InflateRect(&card, S(5), S(5));

        RoundRectC(dc, &card, S(8), hov ? C_PANEL_HI : C_PANEL, CLR_INVALID);
        /* the collage covers the card, so a tinted background alone is
         * invisible: hover needs a frame of its own */
        if (sel) {
            RECT ring = card;
            FrameRectC(dc, &ring, C_ACCENT);
            RECT ring2 = ring; InflateRect(&ring2, 1, 1);
            FrameRectC(dc, &ring2, C_ACCENT);
        } else if (hov) {
            RECT ring = card;
            InflateRect(&ring, -1, -1);
            FrameRectC(dc, &ring, C_PANEL_ON);
        }

        RECT inner = box;
        InflateRect(&inner, -S(2), -S(2));

        int save = SaveDC(dc);
        IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);

        const int *prev = files ? g_app.filePreview : e->preview;
        int nprev = files ? g_app.fileNPreview : e->nPreview;
        DrawCoverCell(dc, &inner, prev, nprev);
        if (nprev == 0)
            DrawFolderGlyph(dc, (inner.left + inner.right) / 2,
                            (inner.top + inner.bottom) / 2, S(54));
        RestoreDC(dc, save);

        /* how many media files live in there, as a small pill on the picture */
        WCHAR badge[32];
        _snwprintf_s(badge, _countof(badge), _TRUNCATE, L"%d",
                     files ? g_app.folderFiles : e->count);
        int bw = TextWidth(dc, badge, g_app.fontSmall) + S(14);
        RECT pill = { box.right - bw - S(6), box.bottom - S(24),
                      box.right - S(6), box.bottom - S(6) };
        RoundRectC(dc, &pill, S(9), RGB(0, 0, 0), CLR_INVALID);
        DrawTextC(dc, badge, &pill, RGB(235, 235, 235), g_app.fontSmall,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        WCHAR label[MAX_PATH_LONG + 40];
        if (files)
            _snwprintf_s(label, _countof(label), _TRUNCATE,
                         L"整个文件夹 · %d 个文件", g_app.folderFiles);
        else
            wcsncpy_s(label, _countof(label), e->name, _TRUNCATE);

        RECT cap = { r.left - S(4), r.top + g_app.cellH + S(6), r.right + S(4), r.top + boxH };
        DrawTextC(dc, label, &cap, (sel || hov) ? C_TEXT : C_TEXT_DIM, g_app.fontSmall,
                  DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_TOP);
    }

    SetStretchBltMode(dc, COLORONCOLOR);   /* leave the DC as we found it */
    DrawScrollBar(dc, availH);

    LeaveCriticalSection(&lib->lock);
}

/* --------------------------------------------------- single view arrows */

#define NAV_SIZE   46
#define NAV_MARGIN 18

/* which == 0 -> previous arrow (left), 1 -> next arrow (right) */
static BOOL NavRect(int which, RECT *out)
{
    if (!g_app.single || g_app.viewCount <= 1) return FALSE;
    RECT g = g_app.rcGrid;
    int w = S(NAV_SIZE);
    if (g.right - g.left < w * 3 || g.bottom - g.top < w * 2) return FALSE;
    int y = g.top + ((g.bottom - g.top) - w) / 2;
    int x = which ? (g.right - S(NAV_MARGIN) - w) : (g.left + S(NAV_MARGIN));
    out->left = x; out->top = y; out->right = x + w; out->bottom = y + w;
    return TRUE;
}

int ViewNavGutter(void)
{
    RECT r;
    if (!NavRect(0, &r)) return 0;
    return S(NAV_SIZE) + S(NAV_MARGIN) + S(8);
}

int ViewNavHit(POINT pt)
{
    for (int i = 0; i < 2; i++) {
        RECT r;
        if (!NavRect(i, &r)) continue;
        if (!PtInRect(&r, pt)) continue;
        /* a paging arrow is only live when the filtered set has a neighbour */
        if (FilterStep(g_app.cur, i ? 1 : -1) < 0) return -1;
        return i;
    }
    return -1;
}

static void DrawNavArrow(HDC dc, int which, BOOL hot, BOOL enabled)
{
    RECT r;
    if (!NavRect(which, &r)) return;

    COLORREF fill = hot && enabled ? C_PANEL_ON : C_PANEL;
    RoundRectC(dc, &r, (r.right - r.left) / 2, fill, C_LINE);

    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    int dy = S(9);
    int dx = S(5);
    POINT p[3];
    if (which) {                       /* > */
        p[0].x = cx - dx; p[0].y = cy - dy;
        p[1].x = cx + dx; p[1].y = cy;
        p[2].x = cx - dx; p[2].y = cy + dy;
    } else {                           /* < */
        p[0].x = cx + dx; p[0].y = cy - dy;
        p[1].x = cx - dx; p[1].y = cy;
        p[2].x = cx + dx; p[2].y = cy + dy;
    }

    COLORREF ink = enabled ? (hot ? RGB(255, 255, 255) : C_TEXT) : C_TEXT_DIM;
    LOGBRUSH lb = { BS_SOLID, ink, 0 };
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                            S(3), &lb, 0, NULL);
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, p, 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void DrawNav(HDC dc)
{
    if (!g_app.single) return;
    BOOL hasPrev = FilterStep(g_app.cur, -1) >= 0;
    BOOL hasNext = FilterStep(g_app.cur, 1) >= 0;
    DrawNavArrow(dc, 0, g_app.navHot == 0, hasPrev);
    DrawNavArrow(dc, 1, g_app.navHot == 1, hasNext);
}

static void DrawSingleBody(HDC dc, RECT *bodyp)
{
    Library *lib = &g_app.lib;
    RECT body = *bodyp;

    if (g_app.cur < 0 || g_app.cur >= lib->count) return;
    MediaItem *it = &lib->items[g_app.cur];

    if (it->kind == MK_VIDEO) {
        if (VideoIsActive()) return;      /* the evr child window paints itself */

        RECT r = body;
        r.top += (body.bottom - body.top) / 2 - S(40);
        r.bottom = r.top + S(26);
        DrawTextC(dc, L"无法播放这个视频", &r, C_TEXT, g_app.fontUI,
                  DT_CENTER | DT_SINGLELINE);
        r.top += S(28); r.bottom += S(28);
        DrawTextC(dc, L"系统缺少对应解码器 · 按 E 用系统播放器打开", &r, C_TEXT_DIM,
                  g_app.fontSmall, DT_CENTER | DT_SINGLELINE);
        return;
    }

    if (!g_app.img.bits || g_app.img.item != g_app.cur) {
        if (g_app.imgFailed) {
            RECT r = body;
            r.top += (body.bottom - body.top) / 2 - S(20);
            r.bottom = r.top + S(26);
            DrawTextC(dc, L"这个文件无法解码", &r, C_TEXT_DIM, g_app.fontUI,
                      DT_CENTER | DT_SINGLELINE);
        }
        return;
    }

    RECT dest;
    double z = 1.0;
    SingleGeom(&dest, &z);
    ImageDraw(dc, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top);
    (void)z;
}

/* ------------------------------------------------------------ seek bar
 * A slim progress bar drawn across the right part of the status bar while a
 * video plays, with elapsed / total time on either end.  It lives in the
 * status-bar strip (not the grid) because the EVR child window covers the
 * whole grid with its own D3D surface; anything painted on the grid underneath
 * it is invisible.  The left third of the strip is left to the ordinary status
 * line (file name / playback errors).  Click anywhere on the bar to jump, drag
 * to scrub. */

BOOL SeekBarRect(RECT *out)
{
    if (!g_app.single || !VideoIsActive()) return FALSE;
    RECT s = g_app.rcStatus;
    int pad = S(10);
    int l = s.left + pad;
    int r = s.right - pad;
    if (r - l < S(260)) return FALSE;
    int split = l + (r - l) * 42 / 100;   /* status text keeps this side */
    int barL = split + S(8);
    if (r - barL < S(120)) return FALSE;
    out->left = barL;
    out->right = r;
    out->top = s.top + S(3);
    out->bottom = s.bottom - S(3);
    return TRUE;
}

/* width reserved for one clock label; a fixed column keeps the track from
 * shifting when the digits change */
static int SeekLabelW(HDC dc)
{
    return TextWidth(dc, L"0:00:00", g_app.fontSmall);
}

/* track endpoints: the bar minus one clock column on each side.  Falls back to
 * the whole bar when there is no room for the labels. */
static void SeekTrackBounds(RECT *bar, int *l, int *r)
{
    HDC dc = g_app.memDC;
    BOOL own = FALSE;
    if (!dc) { dc = GetDC(g_app.hwnd); own = TRUE; }
    int w = SeekLabelW(dc);
    if (own) ReleaseDC(g_app.hwnd, dc);

    int gap = S(6);
    int tl = bar->left + w + gap;
    int tr = bar->right - w - gap;
    if (tr - tl < S(60)) { tl = bar->left; tr = bar->right; }
    *l = tl;
    *r = tr;
}

static int SeekBarHit(POINT pt)
{
    RECT r;
    if (!SeekBarRect(&r)) return -1;
    RECT hit = r;
    InflateRect(&hit, 0, S(8));      /* forgiving vertical target */
    return PtInRect(&hit, pt) ? 0 : -1;
}

/* millisecond-free clock text: "0:45" below an hour, "1:02:03" above */
static void SeekTimeText(LONGLONG t100ns, WCHAR *out, size_t cap)
{
    LONGLONG secs = t100ns > 0 ? t100ns / 10000000LL : 0;
    if (secs >= 3600)
        _snwprintf_s(out, cap, _TRUNCATE, L"%lld:%02lld:%02lld",
                     secs / 3600, (secs / 60) % 60, secs % 60);
    else
        _snwprintf_s(out, cap, _TRUNCATE, L"%02lld:%02lld", secs / 60, secs % 60);
}

/* fraction 0..1 -> x within the track, used for both the thumb and hit mapping */
static int SeekBarX(int l, int r, double frac)
{
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return l + (int)((double)(r - l) * frac);
}

static double SeekBarFrac(RECT *bar, int x)
{
    int l, r;
    SeekTrackBounds(bar, &l, &r);
    if (r <= l) return 0.0;
    double f = (double)(x - l) / (double)(r - l);
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

void DrawSeekBar(HDC dc)
{
    RECT r;
    if (!SeekBarRect(&r)) return;

    LONGLONG pos = 0, dur = 0;
    BOOL hasClock = VideoClock(&pos, &dur);

    double frac = 0.0;
    if (hasClock && dur > 0) {
        frac = (double)pos / (double)dur;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
    }
    if (g_app.seekDrag) frac = g_app.seekFrac;   /* show the scrub position */
    /* a seek is still in flight: GetPosition() lags the jump for a few frames,
     * so hold the bar at the requested spot until POSITION_SET confirms it */
    else if (g_app.seekPending) frac = g_app.seekFrac;

    int tl, tr;
    SeekTrackBounds(&r, &tl, &tr);
    BOOL labels = (tl > r.left && tr < r.right);

    /* elapsed / total, the clock text the plain track never showed */
    if (labels) {
        WCHAR el[32] = L"", total[32] = L"";
        if (hasClock && dur > 0) {
            LONGLONG shown = (g_app.seekDrag || g_app.seekPending)
                           ? (LONGLONG)((double)dur * frac) : pos;
            LONGLONG left = dur - shown;
            if (left < 0) left = 0;
            WCHAR rem[32];
            SeekTimeText(left, rem, _countof(rem));
            SeekTimeText(shown, el, _countof(el));
            _snwprintf_s(total, _countof(total), _TRUNCATE, L"-%s", rem);
        }
        int w = SeekLabelW(dc);
        RECT elr = { r.left, r.top, r.left + w, r.bottom };
        DrawTextC(dc, el, &elr, C_TEXT, g_app.fontSmall,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT totr = { r.right - w, r.top, r.right, r.bottom };
        DrawTextC(dc, total, &totr, C_TEXT_DIM, g_app.fontSmall,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    /* track */
    int cy = r.top + (r.bottom - r.top) / 2;
    RECT track = { tl, cy - S(2), tr, cy + S(2) };
    RoundRectC(dc, &track, S(2), C_PANEL_ON, CLR_INVALID);

    /* elapsed fill */
    if (frac > 0.0) {
        RECT fill = track;
        fill.right = SeekBarX(tl, tr, frac);
        if (fill.right > fill.left) {
            RoundRectC(dc, &fill, S(2), C_ACCENT, CLR_INVALID);
        }
    }

    /* thumb */
    int cx = SeekBarX(tl, tr, frac);
    int rad = S(6);
    HBRUSH br = CreateSolidBrush(C_TEXT);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
}

/* The I overlay: path, dimensions, size and timestamp of what is on screen.
 * Drawn over the picture, in the same dark card style as the rest of the UI.
 * Only for stills - on a video the EVR child window owns those pixels. */
static void DrawInfoPanel(HDC dc, RECT *body)
{
    Library *lib = &g_app.lib;
    if (g_app.cur < 0 || g_app.cur >= lib->count) return;
    MediaItem *it = &lib->items[g_app.cur];
    if (it->kind == MK_VIDEO) return;

    WCHAR size[32];
    FormatSize(it->size, size, _countof(size));
    WCHAR when[64];
    FormatFileTime(&it->mtime, when, _countof(when));

    WCHAR dims[64] = L"解码中…";
    if (g_app.img.bits && g_app.img.item == g_app.cur)
        _snwprintf_s(dims, _countof(dims), _TRUNCATE, L"%d × %d", g_app.img.w, g_app.img.h);

    WCHAR ext[16] = L"";
    const WCHAR *dot = wcsrchr(it->name, L'.');
    if (dot && dot[1]) {
        wcsncpy_s(ext, _countof(ext), dot + 1, _TRUNCATE);
        CharUpperBuffW(ext, (DWORD)wcslen(ext));
    }

    int pad = S(12);
    int lh = S(17);
    int topH = S(20);
    int boxH = pad * 2 + topH + lh * 4 + S(4) * 3;

    int w = S(560);
    int avail = (body->right - body->left) - S(32);
    if (w > avail) w = avail;
    if (w < S(220)) return;

    RECT r;
    r.left = body->left + S(16);
    r.top = body->bottom - boxH - S(16);
    r.right = r.left + w;
    r.bottom = r.top + boxH;

    int save = SaveDC(dc);
    IntersectClipRect(dc, body->left, body->top, body->right, body->bottom);
    RoundRectC(dc, &r, S(8), C_PANEL, C_LINE);

    RECT t = { r.left + pad, r.top + pad, r.right - pad, r.top + pad + topH };
    DrawTextC(dc, it->name, &t, C_TEXT, g_app.fontBold,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    WCHAR line[256];
    t.top = t.bottom + S(4);
    t.bottom = t.top + lh;
    if (ext[0])
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s · %s · %s", dims, size, ext);
    else
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s · %s", dims, size);
    DrawTextC(dc, line, &t, C_TEXT_DIM, g_app.fontSmall,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    t.top = t.bottom + S(4);
    t.bottom = t.top + lh;
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"修改：%s", when);
    DrawTextC(dc, line, &t, C_TEXT_DIM, g_app.fontSmall,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    t.top = t.bottom + S(4);
    t.bottom = t.top + lh;
    DrawTextC(dc, it->path, &t, C_TEXT_DIM, g_app.fontSmall,
              DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    t.top = t.bottom + S(4);
    t.bottom = t.top + lh;
    DrawTextC(dc, L"I 关闭信息 · Ctrl+C 复制图片 · S 幻灯片", &t, C_TEXT_DIM,
              g_app.fontSmall, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RestoreDC(dc, save);
}

static void DrawSingle(HDC dc)
{
    Library *lib = &g_app.lib;
    EnterCriticalSection(&lib->lock);
    RECT body = g_app.rcGrid;
    BOOL video = g_app.cur >= 0 && g_app.cur < lib->count &&
                 lib->items[g_app.cur].kind == MK_VIDEO;

    /* the embedded video window is inset by the arrow gutter, so keep the
     * whole stage in the same near-black instead of the normal background */
    FillRectC(dc, &body, video ? C_VIDEO : C_BG);
    DrawSingleBody(dc, &body);
    DrawNav(dc);                  /* arrows float above the picture */
    if (g_app.showInfo) DrawInfoPanel(dc, &body);
    LeaveCriticalSection(&lib->lock);
}

void ViewPaint(HDC dc)
{
    g_app.clock++;          /* LRU stamp */
    g_app.frame++;          /* decode budget is per repaint, not per touch */

    int saved = SaveDC(dc);
    IntersectClipRect(dc, g_app.rcGrid.left, g_app.rcGrid.top,
                      g_app.rcGrid.right, g_app.rcGrid.bottom);
    if (g_app.single) DrawSingle(dc);
    else if (g_app.folderMode) DrawFolders(dc);
    else DrawGrid(dc);
    RestoreDC(dc, saved);
}

/* ---------------------------------------------------------------- input */

void ViewOnSize(void)
{
    ViewLayout();
    if (g_app.single && g_app.fit) AppInvalidate();
}

void ViewOnWheel(int delta, POINT pt)
{
    if (g_app.single && !VideoIsActive()) {
        double f = (delta > 0) ? 1.15 : 1.0 / 1.15;
        if (GetKeyState(VK_CONTROL) < 0) f = (delta > 0) ? 1.05 : 1.0 / 1.05;
        ZoomAt(f, pt);
        return;
    }

    /* Ctrl+wheel resizes the cells instead of scrolling: the same gesture as
     * Explorer, and the only way to get more or fewer pictures per screen. */
    if (!g_app.single && (GetKeyState(VK_CONTROL) < 0)) {
        int v = g_app.thumbScale + (delta > 0 ? 10 : -10);
        if (v < 60) v = 60;
        if (v > 180) v = 180;
        if (v != g_app.thumbScale) {
            g_app.thumbScale = v;
            g_app.cellW = S(168 * v / 100);
            g_app.cellH = S(150 * v / 100);
            ViewLayout();
            if (g_app.folderMode) EnsureFolderVisible(g_app.selFolder);
            else if (g_app.sel >= 0) EnsureVisible(g_app.sel);
            SetStatus(L"缩略图 %d%%", v);
            AppInvalidate();
        }
        return;
    }

    int step = S(120);
    GridScroll(-(delta / 120) * step);
}

/* One click enters a folder card, exactly like one click opens a picture: the
 * app's whole input model is "click the thing you see".  A double-click fires
 * a second WM_LBUTTONDOWN a few ms later, by which time the first one has
 * already replaced the grid - so swallow clicks that arrive right after an
 * entry, or a fast double-click would drill two levels. */
static void FolderClick(POINT pt)
{
    if (GetTickCount64() - g_app.folderAt < 450) return;   /* second click */
    int cell = HitTestFolder(pt);
    if (cell < 0) return;
    g_app.selFolder = cell;
    AppEnterFolder(cell);
}

void FolderSelect(int cell)
{
    int cells = FolderCells();
    if (cells <= 0) return;
    if (cell < 0) cell = 0;
    if (cell >= cells) cell = cells - 1;
    g_app.selFolder = cell;
    g_app.hoverFolder = -1;
    EnsureFolderVisible(cell);
    AppUpdateInfo();
    AppInvalidate();
}

void FolderStep(int delta)
{
    int cells = FolderCells();
    if (cells <= 0) return;
    int s = g_app.selFolder;
    if (s < 0) s = 0;
    else s += delta;
    FolderSelect(s);
}

void ViewOnDown(POINT pt)
{
    if (g_app.single) {
        int nav = ViewNavHit(pt);
        if (nav >= 0) {
            int next = FilterStep(g_app.cur, nav ? 1 : -1);
            if (next >= 0) AppGoto(next, TRUE);
            return;
        }
        if (VideoIsActive()) {
            /* clicking the seek bar starts a scrub; a click elsewhere on the
             * video simply toggles play/pause (below). */
            if (SeekBarHit(pt) >= 0) {
                RECT r;
                if (SeekBarRect(&r)) {
                    g_app.seekDrag = TRUE;
                    g_app.seekFrac = SeekBarFrac(&r, pt.x);
                    SetCapture(g_app.hwnd);
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                }
            } else {
                VideoToggle();   /* tap the picture: play / pause */
            }
            return;
        }
        g_app.dragging = TRUE;
        g_app.dragOrigin = pt;
        g_app.dragPanX = g_app.panX;
        g_app.dragPanY = g_app.panY;
        SetCapture(g_app.hwnd);
        SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
        return;
    }

    if (PtInRect(&g_app.rcScroll, pt)) {
        int maxScroll;
        ScrollMetrics(&maxScroll);
        if (maxScroll > 0 && g_app.contentH > 0) {
            int trackH = g_app.rcGrid.bottom - g_app.rcGrid.top - S(8);
            int thumbH = trackH * (g_app.rcGrid.bottom - g_app.rcGrid.top) / g_app.contentH;
            if (thumbH < S(30)) thumbH = S(30);
            int travel = trackH - thumbH;
            int ty = g_app.rcGrid.top + S(4) + (int)((LONGLONG)travel * g_app.scrollY / maxScroll);
            if (pt.y >= ty && pt.y <= ty + thumbH) {
                g_app.sbDrag = TRUE;
                g_app.sbOff = pt.y - ty;
                SetCapture(g_app.hwnd);
            } else {
                int want = (pt.y - g_app.rcGrid.top - S(4) - thumbH / 2);
                if (want < 0) want = 0;
                GridScroll((int)((LONGLONG)want * maxScroll / (travel ? travel : 1)) - g_app.scrollY);
            }
        }
        return;
    }

    if (g_app.folderMode) { FolderClick(pt); return; }

    /* A double-click on a card has already changed the view under the cursor;
     * its second press would otherwise open whatever now sits there (a picture,
     * or the "whole folder" card's file grid).  Same 450 ms guard as FolderClick. */
    if (GetTickCount64() - g_app.folderAt < 450) return;

    int idx = HitTestGrid(pt);
    if (idx >= 0) {
        /* a single click opens the viewer straight away */
        g_app.sel = idx;
        g_app.cur = idx;
        AppSetSingle(TRUE);
    }
}

void ViewOnMove(POINT pt)
{
    if (g_app.seekDrag) {
        RECT r;
        if (SeekBarRect(&r)) {
            g_app.seekFrac = SeekBarFrac(&r, pt.x);
            AppInvalidate();
        }
        return;
    }

    if (g_app.sbDrag) {
        int maxScroll;
        ScrollMetrics(&maxScroll);
        int trackH = g_app.rcGrid.bottom - g_app.rcGrid.top - S(8);
        /* contentH is 0 for an empty folder; a stray drag (or F5 while the
         * list is being rebuilt) would divide by zero here */
        if (maxScroll <= 0 || g_app.contentH <= 0 || trackH <= 0) {
            g_app.sbDrag = FALSE;
            return;
        }
        int thumbH = trackH * (g_app.rcGrid.bottom - g_app.rcGrid.top) / g_app.contentH;
        if (thumbH < S(30)) thumbH = S(30);
        int travel = trackH - thumbH;
        if (travel <= 0) { g_app.scrollY = 0; AppInvalidate(); return; }
        int want = pt.y - g_app.sbOff - g_app.rcGrid.top - S(4);
        if (want < 0) want = 0;
        if (want > travel) want = travel;
        g_app.scrollY = (int)((LONGLONG)want * maxScroll / travel);
        AppInvalidate();
        return;
    }

    if (g_app.dragging) {
        g_app.panX = g_app.dragPanX + (pt.x - g_app.dragOrigin.x);
        g_app.panY = g_app.dragPanY + (pt.y - g_app.dragOrigin.y);
        ClampPan();
        AppInvalidate();
        return;
    }

    if (!g_app.single) {
        if (g_app.folderMode) {
            int cell = HitTestFolder(pt);
            if (cell != g_app.hoverFolder) {
                g_app.hoverFolder = cell;
                AppInvalidate();
            }
        } else {
            int idx = HitTestGrid(pt);
            if (idx != g_app.hover) {
                g_app.hover = idx;
                AppInvalidate();
            }
        }
    } else {
        int nav = ViewNavHit(pt);
        if (nav != g_app.navHot) {
            g_app.navHot = nav;
            AppInvalidate();
        }
        /* hand cursor over the seek bar while a video is playing */
        if (VideoIsActive()) {
            SetCursor(LoadCursorW(NULL, SeekBarHit(pt) >= 0 ? IDC_HAND : IDC_ARROW));
        }
    }
}

void ViewOnUp(void)
{
    if (g_app.seekDrag) {
        g_app.seekDrag = FALSE;
        ReleaseCapture();
        VideoSeek(g_app.seekFrac);
        return;
    }
    if (g_app.dragging || g_app.sbDrag) {
        g_app.dragging = FALSE;
        g_app.sbDrag = FALSE;
        ReleaseCapture();
    }
}

void ViewOnDoubleClick(POINT pt)
{
    if (g_app.single) {
        if (ViewNavHit(pt) >= 0) return;   /* double-clicking an arrow stays put */
        /* the second click of a fast double-click on a thumbnail arrives here
           right after that same click already opened the viewer: keep it open
           instead of bouncing back to the grid */
        if (GetTickCount64() - g_app.singleAt < 450) return;
        AppSetSingle(FALSE);
        return;
    }
    if (g_app.folderMode) return;          /* the first click already entered */
    if (GetTickCount64() - g_app.folderAt < 450) return;   /* ditto for a card */
    int idx = HitTestGrid(pt);
    if (idx >= 0) {
        g_app.sel = idx;
        g_app.cur = idx;
        AppSetSingle(TRUE);
    }
}

/* Right-click is the "one level up" gesture, in every view: it leaves the
 * single picture the way Esc does, and on the grid it walks back out of the
 * folder that was entered - from an album's files to the album list, from the
 * album list to its parent.  Nothing destructive is ever bound to it. */
BOOL ViewOnRightClick(void)
{
    if (g_app.single) {
        AppSetSingle(FALSE);
        return TRUE;
    }
    if (!g_app.curDir[0]) return FALSE;

    if (g_app.folderMode)
        return AppLeaveFolder();           /* FALSE when already at the root */

    if (g_app.folderCount > 0) {           /* this level has cards: show them */
        AppSetFolderMode(1);
        return TRUE;
    }
    return AppLeaveFolder();
}
