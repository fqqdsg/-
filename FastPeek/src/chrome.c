/* FastPeek - self-drawn toolbar and status bar. */
#include "app.h"

typedef struct { BtnId id; const WCHAR *label; int fixedW; int group; } BtnSpec;

static const BtnSpec kSpec[] = {
    { BT_OPEN,      L"打开文件夹", 0, 1 },
    { BT_UP,        L"↑",        0, 1 },
    { BT_PREV,      L"◀",        0, 1 },
    { BT_NEXT,      L"▶",        0, 1 },
    { BT_ZOOM_OUT,  L"−",        0, 2 },
    { BT_FIT,       L"适合窗口",   0, 2 },
    { BT_ONE,       L"1:1",      0, 2 },
    { BT_ZOOM_IN,   L"+",        0, 2 },
    { BT_GRID,      L"缩略图",     0, 3 },
    { BT_FOLDERS,   L"子文件夹",   0, 3 },
    { BT_SINGLE,    L"大图",      0, 3 },
    { BT_SLIDE,     L"幻灯片",     0, 3 },
    { BT_ROTATE,    L"旋转",      0, 4 },
    { BT_EXPLORE,   L"资源管理器",  0, 4 },
    { BT_SORT,      L"名称",      0, 5 },
    { BT_FILTER,    L"全部",      0, 5 },
};

#define NSPEC ((int)(sizeof(kSpec) / sizeof(kSpec[0])))

static int s_divs[8];
static int s_nDivs;

int ChromeToolbarHeight(void) { return S(46); }

void ChromeUpdateButtons(void)
{
    int n = g_app.viewCount;          /* filtered count governs what is reachable */
    BOOL single = g_app.single;
    BOOL image = FALSE, video = FALSE;
    if (g_app.cur >= 0 && g_app.cur < g_app.lib.count) {
        image = g_app.lib.items[g_app.cur].kind == MK_IMAGE;
        video = !image;
    }
    for (int i = 0; i < g_app.nButtons; i++) {
        Button *b = &g_app.buttons[i];
        switch (b->id) {
        case BT_PREV: case BT_NEXT: b->enabled = n > 0; break;
        case BT_ZOOM_OUT: case BT_ZOOM_IN: case BT_FIT: case BT_ONE:
            b->enabled = single && image; break;
        case BT_ROTATE: b->enabled = single && image && g_app.img.bits != NULL; break;
        case BT_GRID: b->enabled = single || g_app.folderMode; break;
        case BT_FOLDERS: b->enabled = g_app.folderCount > 0; break;
        case BT_SINGLE: b->enabled = !single && n > 0; break;
        case BT_EXPLORE: b->enabled = n > 0; break;
        case BT_SORT: b->enabled = !g_app.folderMode && n > 0; break;
        case BT_SLIDE: b->enabled = n > 0; break;
        default: b->enabled = TRUE; break;
        }
        (void)video;
    }
}

void ChromeLayout(void)
{
    HDC dc = GetDC(g_app.hwnd);
    int x = S(10);
    int h = S(30);
    int y = g_app.rcToolbar.top + (g_app.rcToolbar.bottom - g_app.rcToolbar.top - h) / 2;
    int lastGroup = kSpec[0].group;

    g_app.nButtons = 0;
    s_nDivs = 0;

    for (int i = 0; i < NSPEC && g_app.nButtons < MAX_BUTTONS; i++) {
        if (kSpec[i].group != lastGroup) {
            x += S(10);
            if (s_nDivs < 8) s_divs[s_nDivs++] = x;
            x += S(10);
            lastGroup = kSpec[i].group;
        }
        int tw = TextWidth(dc, kSpec[i].label, g_app.fontUI);
        int w = kSpec[i].fixedW ? S(kSpec[i].fixedW) : tw + S(24);
        if (w < S(30)) w = S(30);
        Button *b = &g_app.buttons[g_app.nButtons++];
        b->id = kSpec[i].id;
        b->rc.left = x; b->rc.top = y; b->rc.right = x + w; b->rc.bottom = y + h;
        b->enabled = TRUE;
        x += w + S(4);
    }
    ReleaseDC(g_app.hwnd, dc);
    ChromeUpdateButtons();
}

static const WCHAR *LabelOf(BtnId id)
{
    if (id == BT_SORT) {
        switch (g_app.sortMode) {
        case SORT_DATE: return L"时间 ▾";
        case SORT_SIZE: return L"大小 ▾";
        default: return L"名称 ▾";
        }
    }
    if (id == BT_FILTER) {
        /* the trailing glyph hints "click to cycle", three two-char labels keep
           the button width stable so the folder path on the right never jumps */
        switch (g_app.filter) {
        case 1: return L"图片 ▾";
        case 2: return L"视频 ▾";
        default: return L"全部 ▾";
        }
    }
    for (int i = 0; i < NSPEC; i++)
        if (kSpec[i].id == id) return kSpec[i].label;
    return L"";
}

BtnId ChromeHit(POINT pt)
{
    for (int i = 0; i < g_app.nButtons; i++) {
        if (!g_app.buttons[i].enabled) continue;
        if (PtInRect(&g_app.buttons[i].rc, pt)) return g_app.buttons[i].id;
    }
    return BT_NONE;
}

void ChromePaint(HDC dc)
{
    RECT tb = g_app.rcToolbar;
    FillRectC(dc, &tb, C_PANEL);

    RECT line = tb;
    line.top = tb.bottom - 1;
    FillRectC(dc, &line, C_LINE);

    for (int i = 0; i < s_nDivs; i++) {
        RECT d = { s_divs[i], tb.top + S(11), s_divs[i] + 1, tb.bottom - S(11) };
        FillRectC(dc, &d, C_LINE);
    }

    for (int i = 0; i < g_app.nButtons; i++) {
        Button *b = &g_app.buttons[i];
        BOOL active = (b->id == BT_SINGLE && g_app.single) ||
                      (b->id == BT_GRID && !g_app.single && !g_app.folderMode) ||
                      (b->id == BT_FOLDERS && g_app.folderMode) ||
                      (b->id == BT_SLIDE && g_app.slideshow);
        BOOL hot = (g_app.hoverBtn == (int)b->id);

        COLORREF fill = CLR_INVALID, text = b->enabled ? C_TEXT : C_TEXT_DIM;
        if (active) { fill = C_ACCENT_D; text = RGB(255, 255, 255); }
        else if (hot) fill = C_PANEL_HI;

        RECT rc = b->rc;
        if (fill != CLR_INVALID) RoundRectC(dc, &rc, S(6), fill, CLR_INVALID);

        RECT tr = rc;
        DrawTextC(dc, LabelOf(b->id), &tr, text, g_app.fontUI,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* right hand side: the search that is being typed, else the current folder
 * (skipped when the buttons need the room) */
    BOOL searching = g_app.search[0] != 0 || g_app.searchActive;
    const WCHAR *dir = g_app.curDir[0] ? g_app.curDir : g_app.lib.root;
    if (searching || dir[0]) {
        RECT r = tb;
        r.right -= S(14);
        r.left = r.right - S(300);
        if (g_app.nButtons > 0) {
            int after = g_app.buttons[g_app.nButtons - 1].rc.right + S(12);
            if (after > r.left) r.left = after;
        }
        if (r.right - r.left > S(60)) {
            WCHAR buf[MAX_PATH_LONG];
            if (searching) {
                _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"搜索：%s%s",
                             g_app.search, g_app.searchActive ? L"_" : L"");
                DrawTextC(dc, buf, &r, g_app.searchActive ? C_ACCENT : C_TEXT_DIM,
                          g_app.fontSmall,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                wcsncpy_s(buf, MAX_PATH_LONG, dir, _TRUNCATE);
                DrawTextC(dc, buf, &r, C_TEXT_DIM, g_app.fontSmall,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            }
        }
    }

    /* status bar */
    RECT st = g_app.rcStatus;
    FillRectC(dc, &st, C_PANEL);
    RECT top = st;
    top.bottom = st.top + 1;
    FillRectC(dc, &top, C_LINE);

    /* While a video plays the right part of the strip is taken over by the
     * seek bar (see DrawSeekBar).  The ordinary status line still has to be
     * drawn - otherwise playback errors and pause/ended hints set through
     * SetStatus would never reach the screen - so it lives in the left zone
     * that SeekBarRect leaves free. */
    if (g_app.single && VideoIsActive()) {
        RECT bar;
        if (SeekBarRect(&bar)) {
            RECT v = st;
            v.left += S(12);
            v.right = bar.left - S(12);
            if (v.right > v.left + S(40))
                DrawTextC(dc, g_app.status, &v, C_TEXT_DIM, g_app.fontSmall,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return;
        }
    }

    RECT l = st;
    l.left += S(12);
    l.right = l.left + (st.right - st.left) * 45 / 100;
    DrawTextC(dc, g_app.status, &l, C_TEXT_DIM, g_app.fontSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT rr = st;
    rr.right -= S(12);
    DrawTextC(dc, g_app.info, &rr, C_TEXT, g_app.fontSmall,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
