/* FastPeek - small shared helpers: DPI, text, paths, painting, settings. */
#include "app.h"
#include <shobjidl.h>
#include <stdarg.h>

App g_app;

int S(int v) { return MulDiv(v, (int)g_app.dpi, 96); }
int SF(double v) { return (int)(v * (double)g_app.dpi / 96.0 + 0.5); }

void UtilInit(HFONT *ui, HFONT *fSmall, HFONT *fBold)
{
    int h = S(14);
    *ui    = CreateFontW(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    *fSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    *fBold  = CreateFontW(-h, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void SetStatus(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(g_app.status, _countof(g_app.status), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_app.hwnd) InvalidateRect(g_app.hwnd, &g_app.rcStatus, FALSE);
}

/* ------------------------------------------------------------------ paths */

WCHAR *PathFileName(WCHAR *path)
{
    WCHAR *p = wcsrchr(path, L'\\');
    return p ? p + 1 : path;
}

BOOL PathIsDir(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void PathJoin(WCHAR *dst, size_t cap, const WCHAR *dir, const WCHAR *name)
{
    size_t n = wcslen(dir);
    if (n && (dir[n - 1] == L'\\' || dir[n - 1] == L'/'))
        _snwprintf_s(dst, cap, _TRUNCATE, L"%s%s", dir, name);
    else
        _snwprintf_s(dst, cap, _TRUNCATE, L"%s\\%s", dir, name);
}

/* strip the last component; leaves "C:\" untouched */
void PathParent(WCHAR *path)
{
    WCHAR *p = wcsrchr(path, L'\\');
    if (!p) return;
    if (p == path) { path[0] = 0; return; }
    if (p - path == 2 && path[1] == L':') { p[1] = 0; return; }
    *p = 0;
}

/* Unify separators, collapse runs, then drop a trailing separator unless this
 * is a drive or UNC root.  Without the collapse, something like
 * "C:////Users////me////pics" passes PathIsDir (Win32 tolerates it) but then makes
 * FindFirstFile fail, so the folder silently looks empty. */
void PathNormalize(WCHAR *path)
{
    if (!path || !path[0]) return;

    WCHAR *d = path;
    const WCHAR *s = path;
    int lead = 0;
    while (lead < 2 && (s[lead] == L'\\' || s[lead] == L'/')) lead++;
    for (int i = 0; i < lead; i++) *d++ = L'\\';   /* keep a UNC prefix */
    s += lead;

    for (; *s; s++) {
        WCHAR c = (*s == L'/') ? L'\\' : *s;
        if (c == L'\\' && d > path && d[-1] == L'\\') continue;
        *d++ = c;
    }
    *d = 0;

    size_t n = wcslen(path);
    while (n > 1 && path[n - 1] == L'\\') {
        if (n == 3 && path[1] == L':') break;      /* C://      */
        if (n == 2 && path[0] == L'\\') break;     /* \\       */
        path[--n] = 0;
    }
}

static BOOL IsDigit(WCHAR c) { return c >= L'0' && c <= L'9'; }

/* Explorer-like ordering: runs of digits compare by value, so 2 < 10 */
int NaturalCompare(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        if (IsDigit(*a) && IsDigit(*b)) {
            const WCHAR *a0 = a, *b0 = b;
            while (*a == L'0') a++;
            while (*b == L'0') b++;
            const WCHAR *as = a, *bs = b;
            while (IsDigit(*a)) a++;
            while (IsDigit(*b)) b++;
            size_t alen = (size_t)(a - as), blen = (size_t)(b - bs);
            if (alen != blen) return alen < blen ? -1 : 1;
            for (size_t i = 0; i < alen; i++) {
                if (as[i] != bs[i]) return as[i] < bs[i] ? -1 : 1;
            }
            (void)a0; (void)b0;
            continue;
        }
        WCHAR ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = (WCHAR)(ca + 32);
        if (cb >= L'A' && cb <= L'Z') cb = (WCHAR)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++; b++;
    }
    return (*a != 0) - (*b != 0);
}

void FormatSize(LONGLONG bytes, WCHAR *out, size_t cap)
{
    if (bytes < 1024)                _snwprintf_s(out, cap, _TRUNCATE, L"%lld B", bytes);
    else if (bytes < 1024LL * 1024)  _snwprintf_s(out, cap, _TRUNCATE, L"%.0f KB", (double)bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        _snwprintf_s(out, cap, _TRUNCATE, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else _snwprintf_s(out, cap, _TRUNCATE, L"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

void FormatDuration(LONGLONG t, WCHAR *out, size_t cap)
{
    LONGLONG secs = t / 10000000LL;
    if (secs < 0) secs = 0;
    if (secs >= 3600)
        _snwprintf_s(out, cap, _TRUNCATE, L"%lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
    else
        _snwprintf_s(out, cap, _TRUNCATE, L"%lld:%02lld", secs / 60, secs % 60);
}

void FormatFileTime(const FILETIME *ft, WCHAR *out, size_t cap)
{
    out[0] = 0;
    if (!ft) return;
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(ft, &local) || !FileTimeToSystemTime(&local, &st)) return;

    WCHAR date[32] = L"", time[32] = L"";
    if (!GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, L"yyyy'-'MM'-'dd", date,
                        (int)_countof(date)))
        wcsncpy_s(date, _countof(date), L"----/--/--", _TRUNCATE);
    if (!GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, L"HH':'mm':'ss", time,
                        (int)_countof(time)))
        wcsncpy_s(time, _countof(time), L"--:--:--", _TRUNCATE);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s %s", date, time);
}

/* ---------------------------------------------------------------- painting */

/* The palette is a fixed handful of colours, so a grid frame used to create
 * and destroy several hundred brushes and pens.  Cache them instead: one
 * lookup per fill, zero kernel churn.  Freed once, at shutdown. */
#define GDICACHE_N 32

static struct { COLORREF c; HBRUSH h; } s_brush[GDICACHE_N];
static int s_brushN;
static struct { COLORREF c; HPEN h; } s_pen[GDICACHE_N];
static int s_penN;

static HBRUSH BrushFor(COLORREF c)
{
    for (int i = 0; i < s_brushN; i++)
        if (s_brush[i].c == c) return s_brush[i].h;
    if (s_brushN >= GDICACHE_N) return NULL;
    HBRUSH h = CreateSolidBrush(c);
    if (!h) return NULL;
    s_brush[s_brushN].c = c;
    s_brush[s_brushN].h = h;
    s_brushN++;
    return h;
}

static HPEN PenFor(COLORREF c)
{
    for (int i = 0; i < s_penN; i++)
        if (s_pen[i].c == c) return s_pen[i].h;
    if (s_penN >= GDICACHE_N) return NULL;
    HPEN h = CreatePen(PS_SOLID, 1, c);
    if (!h) return NULL;
    s_pen[s_penN].c = c;
    s_pen[s_penN].h = h;
    s_penN++;
    return h;
}

void GdiCacheFree(void)
{
    for (int i = 0; i < s_brushN; i++) DeleteObject(s_brush[i].h);
    for (int i = 0; i < s_penN; i++)   DeleteObject(s_pen[i].h);
    s_brushN = s_penN = 0;
}

void FillRectC(HDC dc, RECT *rc, COLORREF c)
{
    HBRUSH br = BrushFor(c);
    if (!br) { HBRUSH t = CreateSolidBrush(c); FillRect(dc, rc, t); DeleteObject(t); return; }
    FillRect(dc, rc, br);
}

void FrameRectC(HDC dc, RECT *rc, COLORREF c)
{
    HBRUSH br = BrushFor(c);
    if (!br) { HBRUSH t = CreateSolidBrush(c); FrameRect(dc, rc, t); DeleteObject(t); return; }
    FrameRect(dc, rc, br);
}

void RoundRectC(HDC dc, RECT *rc, int radius, COLORREF fill, COLORREF line)
{
    HBRUSH br = fill == CLR_INVALID ? NULL : BrushFor(fill);
    HPEN   pen = line == CLR_INVALID ? NULL : PenFor(line);
    HGDIOBJ ob = (HGDIOBJ)SelectObject(dc, br ? br : GetStockObject(NULL_BRUSH));
    HGDIOBJ op = (HGDIOBJ)SelectObject(dc, pen ? pen : GetStockObject(NULL_PEN));
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

void DrawTextC(HDC dc, const WCHAR *text, RECT *rc, COLORREF color, HFONT font, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, font);
    int     om  = SetBkMode(dc, TRANSPARENT);
    COLORREF oc = SetTextColor(dc, color);
    DrawTextW(dc, text, -1, rc, flags);
    SetTextColor(dc, oc);
    SetBkMode(dc, om);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, const WCHAR *text, HFONT font)
{
    if (!text || !*text) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE    sz  = { 0, 0 };
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

/* ---------------------------------------------------------------- settings */

static WCHAR s_ini[MAX_PATH_LONG];

static void IniPath(void)
{
    WCHAR dir[MAX_PATH_LONG], sub[MAX_PATH_LONG];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH_LONG);
    if (!n || n >= MAX_PATH_LONG) { s_ini[0] = 0; return; }
    PathJoin(sub, _countof(sub), dir, L"FastPeek");
    CreateDirectoryW(sub, NULL);
    PathJoin(s_ini, _countof(s_ini), sub, L"settings.ini");
}

void SettingsLoad(void)
{
    IniPath();
    if (!s_ini[0]) return;
    WCHAR buf[MAX_PATH_LONG] = { 0 };
    GetPrivateProfileStringW(L"app", L"folder", L"", buf, MAX_PATH_LONG, s_ini);
    if (buf[0] && PathIsDir(buf)) {
        wcsncpy_s(g_app.lib.root, MAX_PATH_LONG, buf, _TRUNCATE);
    }
    g_app.sortMode = GetPrivateProfileIntW(L"view", L"sort", SORT_NAME, s_ini);
    if (g_app.sortMode < SORT_NAME || g_app.sortMode > SORT_SIZE) g_app.sortMode = SORT_NAME;
    g_app.thumbScale = GetPrivateProfileIntW(L"view", L"thumbScale", 100, s_ini);
    if (g_app.thumbScale < 60 || g_app.thumbScale > 180) g_app.thumbScale = 100;
    g_app.slideSec = GetPrivateProfileIntW(L"view", L"slideSec", 5, s_ini);
    if (g_app.slideSec < 1) g_app.slideSec = 1;
    if (g_app.slideSec > 60) g_app.slideSec = 60;

    g_app.volume = GetPrivateProfileIntW(L"video", L"volume", 100, s_ini);
    if (g_app.volume < 0) g_app.volume = 0;
    if (g_app.volume > 100) g_app.volume = 100;
    g_app.muted = GetPrivateProfileIntW(L"video", L"muted", 0, s_ini) != 0;

    int x = GetPrivateProfileIntW(L"app", L"x", -32000, s_ini);
    int y = GetPrivateProfileIntW(L"app", L"y", -32000, s_ini);
    int w = GetPrivateProfileIntW(L"app", L"w", 0, s_ini);
    int h = GetPrivateProfileIntW(L"app", L"h", 0, s_ini);
    if (x != -32000 && y != -32000 && w > 400 && h > 300) {
        g_app.startX = x;
        g_app.startY = y;
        g_app.startW = w;
        g_app.startH = h;
    }
}

void SettingsSave(void)
{
    IniPath();
    if (!s_ini[0] || !g_app.hwnd) return;
    WritePrivateProfileStringW(L"app", L"folder", g_app.lib.root, s_ini);

    WCHAR num[32];
    _snwprintf_s(num, 32, _TRUNCATE, L"%d", g_app.sortMode);
    WritePrivateProfileStringW(L"view", L"sort", num, s_ini);
    _snwprintf_s(num, 32, _TRUNCATE, L"%d", g_app.thumbScale);
    WritePrivateProfileStringW(L"view", L"thumbScale", num, s_ini);
    _snwprintf_s(num, 32, _TRUNCATE, L"%d", g_app.slideSec);
    WritePrivateProfileStringW(L"view", L"slideSec", num, s_ini);
    _snwprintf_s(num, 32, _TRUNCATE, L"%d", g_app.volume);
    WritePrivateProfileStringW(L"video", L"volume", num, s_ini);
    _snwprintf_s(num, 32, _TRUNCATE, L"%d", g_app.muted ? 1 : 0);
    WritePrivateProfileStringW(L"video", L"muted", num, s_ini);

    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (GetWindowPlacement(g_app.hwnd, &wp)) {
        RECT r = wp.rcNormalPosition;
        _snwprintf_s(num, 32, _TRUNCATE, L"%d", (int)r.left);
        WritePrivateProfileStringW(L"app", L"x", num, s_ini);
        _snwprintf_s(num, 32, _TRUNCATE, L"%d", (int)r.top);
        WritePrivateProfileStringW(L"app", L"y", num, s_ini);
        _snwprintf_s(num, 32, _TRUNCATE, L"%d", (int)(r.right - r.left));
        WritePrivateProfileStringW(L"app", L"w", num, s_ini);
        _snwprintf_s(num, 32, _TRUNCATE, L"%d", (int)(r.bottom - r.top));
        WritePrivateProfileStringW(L"app", L"h", num, s_ini);
    }
}

BOOL PickFolder(HWND owner, WCHAR *out, size_t cap)
{
    IFileOpenDialog *dlg = NULL;
    BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dlg)))
        return FALSE;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->lpVtbl->GetOptions(dlg, &opts)))
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    /* Start where the user is.  The walker only goes SCAN_MAX_DEPTH levels
     * below the folder that was opened, so the way deeper is "open that folder
     * itself" - and making the dialog start at This PC would turn a depth limit
     * into a wall. */
    IShellItem *start = NULL;
    if (g_app.curDir[0] &&
        SUCCEEDED(SHCreateItemFromParsingName(g_app.curDir, NULL, &IID_IShellItem,
                                              (void **)&start))) {
        dlg->lpVtbl->SetFolder(dlg, start);
        start->lpVtbl->Release(start);
    }
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item))) {
            LPWSTR p = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &p))) {
                wcsncpy_s(out, cap, p, _TRUNCATE);
                ok = TRUE;
                CoTaskMemFree(p);
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
    return ok;
}

void OpenFolderInExplorer(const WCHAR *path)
{
    if (path && path[0]) ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

void ShowInExplorer(const WCHAR *path)
{
    WCHAR args[MAX_PATH_LONG + 8];
    if (!path || !path[0]) return;
    _snwprintf_s(args, _countof(args), _TRUNCATE, L"/select,\"%s\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

void OpenWithShell(const WCHAR *path)
{
    if (path && path[0]) ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}
