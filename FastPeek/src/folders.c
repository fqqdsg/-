/* FastPeek - the directory index behind the sub-folder grid.
 *
 * The walker collects the whole tree in one recursive pass, so the folder view
 * never touches the disk: it regroups the items already in the Library into
 * the immediate children of g_app.curDir, remembers one cover picture in each,
 * and the grid draws that as the card.  Entering a card is a re-filter of the
 * same array, which is why browsing a stick full of albums costs nothing extra.
 */
#include "app.h"

/* TRUE when path is dir itself or lives below it; rel (optional) receives the
 * remainder after the separator.  Case-insensitive like the file system, and
 * the separator test stops "C:\a" from matching "C:\ab". */
BOOL PathIsUnder(const WCHAR *dir, const WCHAR *path, const WCHAR **rel)
{
    if (!dir || !dir[0]) { if (rel) *rel = path; return TRUE; }

    size_t n = wcslen(dir);
    if (_wcsnicmp(path, dir, n) != 0) return FALSE;

    if (path[n] == 0) {                    /* exactly dir */
        if (rel) *rel = path + n;
        return TRUE;
    }
    if (dir[n - 1] == L'\\') {             /* "C:\" or "\\server\share\" */
        if (rel) *rel = path + n;
        return TRUE;
    }
    if (path[n] != L'\\') return FALSE;
    if (rel) *rel = path + n + 1;
    return TRUE;
}

/* How many levels below dir path sits: 0 for dir itself, 1 for a direct child.
 * The walker stops at SCAN_MAX_DEPTH, so this is also how the ui knows it is
 * looking at a folder whose own sub-folders were never indexed. */
int PathDepthUnder(const WCHAR *dir, const WCHAR *path)
{
    const WCHAR *rel = NULL;
    if (!PathIsUnder(dir, path, &rel)) return -1;
    if (!rel || !*rel) return 0;

    int depth = 1;
    for (const WCHAR *p = rel; *p; p++)
        if (*p == L'\\') depth++;
    return depth;
}

void FoldersFree(void)
{
    for (int i = 0; i < g_app.folderCount; i++) free(g_app.folders[i].path);
    free(g_app.folders);
    g_app.folders = NULL;
    g_app.folderCount = g_app.folderCap = 0;
}

static FolderEntry *FolderSlot(const WCHAR *name)
{
    if (g_app.folderCount >= g_app.folderCap) {
        int nc = g_app.folderCap ? g_app.folderCap * 2 : 32;
        if (nc > MAX_FOLDERS) nc = MAX_FOLDERS;
        if (nc <= g_app.folderCap) return NULL;
        FolderEntry *grown = (FolderEntry *)realloc(g_app.folders,
                                                    (size_t)nc * sizeof(FolderEntry));
        if (!grown) return NULL;
        g_app.folders = grown;
        g_app.folderCap = nc;
    }

    size_t cap = wcslen(g_app.curDir) + wcslen(name) + 2;
    WCHAR *full = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!full) return NULL;
    PathJoin(full, cap, g_app.curDir, name);

    FolderEntry *e = &g_app.folders[g_app.folderCount++];
    memset(e, 0, sizeof(*e));
    e->path = full;
    e->name = PathFileName(full);
    return e;
}

/* Name -> slot map for the walk below.  A freshly walked tree arrives sorted,
 * so this is a little more work than strictly needed there - but a filter
 * change can land in the middle of a scan and break that ordering, and a
 * 15 000 file tree must not spend 400 ms scanning the folder list per file. */
#define FOLDER_HASH 16384          /* power of two, > 2 * MAX_FOLDERS */

static int s_hash[FOLDER_HASH];

static unsigned NameHash(const WCHAR *s)
{
    unsigned h = 2166136261u;      /* FNV-1a */
    for (; *s; s++) {
        h ^= (unsigned)(*s & 0xFFFF);
        h *= 16777619u;
    }
    return h & (FOLDER_HASH - 1);
}

static FolderEntry *FolderLookup(const WCHAR *name)
{
    unsigned h = NameHash(name);
    for (unsigned probe = 0; probe < FOLDER_HASH; probe++) {
        int slot = s_hash[(h + probe) & (FOLDER_HASH - 1)];
        if (slot < 0) return NULL;
        if (NaturalCompare(g_app.folders[slot].name, name) == 0)
            return &g_app.folders[slot];
    }
    return NULL;
}

static void FolderRegister(int slot)
{
    unsigned h = NameHash(g_app.folders[slot].name);
    for (unsigned probe = 0; probe < FOLDER_HASH; probe++) {
        int *cell = &s_hash[(h + probe) & (FOLDER_HASH - 1)];
        if (*cell < 0) { *cell = slot; return; }
    }
}

/* Groups the already-walked Library into the immediate sub-directories of
 * curDir.  One pass, no disk I/O. */
void FoldersRebuild(void)
{
    FoldersFree();
    g_app.folderFiles = 0;
    g_app.fileNPreview = 0;
    memset(s_hash, 0xFF, sizeof(s_hash));      /* all -1 */
    if (!g_app.curDir[0]) return;

    Library *lib = &g_app.lib;
    WCHAR name[MAX_PATH_LONG];

    EnterCriticalSection(&lib->lock);
    int n = lib->count;
    for (int i = 0; i < n; i++) {
        MediaItem *it = &lib->items[i];
        /* counts and previews follow the toolbar filter, so the badge on a
         * card is always the number of files clicking it will show */
        if (g_app.filter == 1 && it->kind != MK_IMAGE) continue;
        if (g_app.filter == 2 && it->kind != MK_VIDEO) continue;
        if (!SearchMatchName(it->name)) continue;

        const WCHAR *rel = NULL;
        if (!PathIsUnder(g_app.curDir, it->path, &rel)) continue;
        if (!rel || !*rel) continue;

        /* every media file below curDir, at any depth: that is what the
         * leading "whole folder" card promises when it is clicked */
        g_app.folderFiles++;

        /* and its collage comes from the same pool, so a folder whose pictures
         * all live one level down still shows a picture, not a blank card */
        if (it->kind == MK_IMAGE && g_app.fileNPreview < FOLDER_PREVIEW)
            g_app.filePreview[g_app.fileNPreview++] = i;

        const WCHAR *sep = wcschr(rel, L'\\');
        if (!sep) continue;                          /* a loose file here */

        size_t len = (size_t)(sep - rel);
        if (len >= MAX_PATH_LONG) continue;
        memcpy(name, rel, len * sizeof(WCHAR));
        name[len] = 0;

        FolderEntry *e = FolderLookup(name);
        if (!e) {
            int slot = g_app.folderCount;
            e = FolderSlot(name);
            if (!e) continue;
            FolderRegister(slot);
        }

        e->count++;
        if (it->kind == MK_IMAGE && e->nPreview < FOLDER_PREVIEW)
            e->preview[e->nPreview++] = i;
    }
    LeaveCriticalSection(&lib->lock);
}

FolderEntry *FolderAt(int index)
{
    if (index < 0 || index >= g_app.folderCount) return NULL;
    return &g_app.folders[index];
}

/* ------------------------------------------------- remembered grid places
 * Walking back out of a folder used to reset the grid to row 0, which throws
 * away the only context the user had: a scrolled half-way down album list.  A
 * mark is the scroll offset plus the selection of one grid, keyed by its
 * directory and mode.  Entering a directory saves the grid it replaces;
 * arriving applies the mark of the grid it lands on, so coming back is a
 * literal undo of the view. */

static void MarkDrop(int slot)
{
    free(g_app.marks[slot].dir);
    memmove(&g_app.marks[slot], &g_app.marks[slot + 1],
            (size_t)(g_app.markCount - slot - 1) * sizeof(ViewMark));
    if (g_app.markCount > 0) g_app.markCount--;
    memset(&g_app.marks[g_app.markCount], 0, sizeof(ViewMark));
}

static int MarkFind(const WCHAR *dir, int folderMode)
{
    for (int i = 0; i < g_app.markCount; i++) {
        if (g_app.marks[i].folderMode != folderMode) continue;
        if (CompareStringOrdinal(g_app.marks[i].dir, -1, dir, -1, TRUE) == CSTR_EQUAL)
            return i;
    }
    return -1;
}

void DirMarksClear(void)
{
    for (int i = 0; i < g_app.markCount; i++) free(g_app.marks[i].dir);
    g_app.markCount = 0;
}

void DirMarkSave(void)
{
    if (!g_app.curDir[0]) return;

    int slot = MarkFind(g_app.curDir, g_app.folderMode);
    if (slot < 0) {
        if (g_app.markCount >= VIEW_MARK_MAX) MarkDrop(0);   /* forget the oldest */
        slot = g_app.markCount;
        memset(&g_app.marks[slot], 0, sizeof(ViewMark));
        size_t n = wcslen(g_app.curDir) + 1;
        WCHAR *dup = (WCHAR *)malloc(n * sizeof(WCHAR));
        if (!dup) return;
        memcpy(dup, g_app.curDir, n * sizeof(WCHAR));
        g_app.marks[slot].dir = dup;
        g_app.marks[slot].folderMode = g_app.folderMode;
        g_app.markCount++;
    }

    ViewMark *m = &g_app.marks[slot];
    m->scrollY   = g_app.scrollY;
    m->selFolder = g_app.selFolder;
    m->sel       = g_app.sel;
}

void DirMarkRestore(void)
{
    int slot = MarkFind(g_app.curDir, g_app.folderMode);
    if (slot < 0) return;

    g_app.scrollY   = g_app.marks[slot].scrollY;
    g_app.selFolder = g_app.marks[slot].selFolder;
    g_app.sel       = g_app.marks[slot].sel;
    MarkDrop(slot);            /* one visit, one mark: a second one starts over */

    /* the list behind the indices may have been rebuilt while we were away
     * (F5, a filter change), so clamp to what is actually there now */
    if (g_app.folderMode) {
        int cells = FolderCells();
        if (g_app.selFolder >= cells) g_app.selFolder = cells > 0 ? cells - 1 : -1;
    } else if (g_app.sel >= 0 && FilterPos(g_app.sel) < 0) {
        g_app.sel = -1;
    }
}

/* Cards drawn by the folder grid: the sub-directories, plus a leading card for
 * "everything below this folder" whenever there are media files at all. */
int FolderCells(void)
{
    if (!g_app.folderMode) return 0;
    return g_app.folderCount + (g_app.folderFiles > 0 ? 1 : 0);
}

BOOL CellIsFilesRow(int cell)
{
    return g_app.folderFiles > 0 && cell == 0;
}
