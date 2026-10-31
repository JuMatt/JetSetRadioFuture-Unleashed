/*
 * JSRF Unleashed -- the Windows launcher ("JSRF Unleashed.exe").
 *
 * Finds your copy of Jet Set Radio Future (the folder with default.xbe and
 * Media in it, or a disc image, which it unpacks once), remembers it, and
 * starts the engine (engine\jsrf_recomp.exe) with the settings it needs --
 * what launcher/linux/jsrf-unleashed does on Linux and the app does on a Mac.
 *
 *   JSRF Unleashed.exe                    play (asks where the game is, the first time)
 *   JSRF Unleashed.exe <folder or .iso>   use that, and remember it (dropping a
 *                                         folder or an image on the .exe does this)
 *   JSRF Unleashed.exe --choose           ask where the game is again
 *   JSRF Unleashed.exe --scale N          internal resolution, 1 to 6 times 640x480
 *                                         (default 2; 6 is for a 4K screen); remembered
 *   JSRF Unleashed.exe --fullscreen       start full screen this time
 *   JSRF Unleashed.exe --windowed         start in a window this time
 *   JSRF Unleashed.exe --portable         keep everything in this folder from now on
 *   JSRF Unleashed.exe --version          which version this is
 *
 * Settings, the cache, logs and unpacked disc images live in
 * %LOCALAPPDATA%\JSRF Unleashed -- or, portable, in the launcher's own folder
 * (see "portable" below). In game: F11 (or Alt+Enter) full screen, F10
 * the picture (4:3, 16:9, 21:9). The game starts the way it was left, in full
 * screen or in a window; --fullscreen and --windowed choose for one start.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiso.h"
#include "xbe_check.h"

#define APP L"JSRF Unleashed"

static wchar_t g_here[MAX_PATH];       /* the launcher's folder */
static wchar_t g_base[MAX_PATH];       /* %LOCALAPPDATA%\JSRF Unleashed, or g_here when portable */
static wchar_t g_cfg[MAX_PATH];        /* ...\settings, shared with the engine */
static int     g_portable;             /* portable.txt beside the launcher */

/* ---- small helpers -------------------------------------------------------- */

static void say(const wchar_t *text, UINT icon)
{
    MessageBoxW(NULL, text, APP, MB_OK | icon | MB_SETFOREGROUND);
}

static int fail(const wchar_t *text)
{
    say(text, MB_ICONERROR);
    return 1;
}

static int is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_file(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void join(wchar_t *out, size_t n, const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a);
    _snwprintf(out, n, L"%ls%ls%ls", a, (la && (a[la - 1] == L'\\' || a[la - 1] == L'/')) ? L"" : L"\\", b);
    out[n - 1] = 0;
}

static int is_game(const wchar_t *dir)
{
    wchar_t p[MAX_PATH * 2];
    if (!dir || !dir[0]) return 0;
    join(p, MAX_PATH * 2, dir, L"default.xbe");
    if (!is_file(p)) return 0;
    join(p, MAX_PATH * 2, dir, L"Media");
    return is_dir(p);
}

static int has_suffix(const wchar_t *s, const wchar_t *suf)
{
    size_t ls = wcslen(s), lf = wcslen(suf);
    return ls >= lf && _wcsicmp(s + ls - lf, suf) == 0;
}

static int is_image_name(const wchar_t *p)
{
    return has_suffix(p, L".iso") || has_suffix(p, L".xiso");
}

static void to_utf8(const wchar_t *w, char *out, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) out[0] = 0;
}

static void from_utf8(const char *s, wchar_t *out, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0;
}

/* ---- the settings file: lines of key=value, UTF-8 -------------------------
 *
 * The engine keeps its own keys (widescreen) in the same file; every line
 * the launcher does not set is kept as it was. */

static int cfg_get(const char *key, char *val, size_t n)
{
    char line[2048];
    size_t kl = strlen(key);
    int found = 0;
    FILE *f = _wfopen(g_cfg, L"rb");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            snprintf(val, n, "%s", line + kl + 1);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static void cfg_set(const char *key, const char *val)
{
    char line[2048];
    wchar_t tmp[MAX_PATH];
    size_t kl = strlen(key);
    FILE *in, *out;
    _snwprintf(tmp, MAX_PATH, L"%ls.tmp", g_cfg);
    tmp[MAX_PATH - 1] = 0;
    if (!(out = _wfopen(tmp, L"wb"))) return;
    if ((in = _wfopen(g_cfg, L"rb")) != NULL) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, key, kl) && line[kl] == '=') continue;
            fputs(line, out);
            if (line[0] && line[strlen(line) - 1] != '\n') fputc('\n', out);
        }
        fclose(in);
    }
    fprintf(out, "%s=%s\n", key, val);
    fclose(out);
    MoveFileExW(tmp, g_cfg, MOVEFILE_REPLACE_EXISTING);
}

static int find_image_in(const wchar_t *dir, wchar_t *out, size_t n);

/* ---- portable ----------------------------------------------------------------
 *
 * With a file named portable.txt beside JSRF Unleashed.exe, everything the
 * launcher and the engine keep -- the settings, the cache (shaders, the
 * game's own cache drive), the logs and unpacked disc images -- goes in this
 * folder instead of %LOCALAPPDATA%\JSRF Unleashed. Saves are in the game's
 * folder (UDATA) either way, so a game unpacked here keeps them here too, and
 * the whole folder can move: to another computer, onto a USB drive. A game
 * folder inside it is remembered relative to it for that reason.
 *
 * The first start asks which, once; --portable switches later (taking the
 * settings along); deleting portable.txt switches back. */

/* The remembered game folder, as stored: relative to this folder when it is
 * inside it and portable. */
static void game_store(const wchar_t *game)
{
    char u8[MAX_PATH * 4];
    size_t lh = wcslen(g_here);
    if (g_portable && lh && !_wcsnicmp(game, g_here, lh) && game[lh] == L'\\' && game[lh + 1])
        to_utf8(game + lh + 1, u8, sizeof u8);
    else
        to_utf8(game, u8, sizeof u8);
    cfg_set("game_dir", u8);
}

static int game_recall(wchar_t *game, size_t n)
{
    char u8[MAX_PATH * 4];
    wchar_t w[MAX_PATH * 2];
    if (!cfg_get("game_dir", u8, sizeof u8) || !u8[0]) return 0;
    from_utf8(u8, w, MAX_PATH * 2);
    /* A drive ("C:...") or a network path ("\\server...") is where it says;
     * anything else is relative to this folder. */
    if ((w[0] && w[1] == L':') || (w[0] == L'\\' && w[1] == L'\\')) _snwprintf(game, n, L"%ls", w);
    else join(game, n, g_here, w);
    game[n - 1] = 0;
    return 1;
}

/* Portable, and nothing remembered: a game folder or a disc image put in this
 * folder is the one, when there is exactly one. */
static int game_beside(wchar_t *game, size_t n)
{
    wchar_t pat[MAX_PATH * 2], one[MAX_PATH * 2], p[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = 0;
    join(pat, MAX_PATH * 2, g_here, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        join(p, MAX_PATH * 2, g_here, fd.cFileName);
        if (is_game(p)) { if (found++) break; wcscpy(one, p); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found == 1) { _snwprintf(game, n, L"%ls", one); game[n - 1] = 0; return 1; }
    if (found == 0 && find_image_in(g_here, game, n)) return 1;
    return 0;
}

/* Where to keep things: decided before anything is read or written. */
static int choose_base(int want_portable)
{
    wchar_t mark[MAX_PATH * 2], la[MAX_PATH], la_cfg[MAX_PATH * 2], here_cfg[MAX_PATH * 2];
    {
        PWSTR k = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &k)) && k) {
            join(la, MAX_PATH, k, L"JSRF Unleashed");
            CoTaskMemFree(k);
        } else {
            const wchar_t *e = _wgetenv(L"LOCALAPPDATA");
            join(la, MAX_PATH, e ? e : g_here, L"JSRF Unleashed");
        }
    }
    join(mark, MAX_PATH * 2, g_here, L"portable.txt");
    join(la_cfg, MAX_PATH * 2, la, L"settings");
    join(here_cfg, MAX_PATH * 2, g_here, L"settings");
    if (!is_file(mark) && !want_portable && !is_file(la_cfg) && !is_file(here_cfg)) {
        /* The very first start: ask, once. */
        int r = MessageBoxW(NULL,
            L"Where should JSRF Unleashed keep its settings, cache, logs and unpacked "
            L"disc images?\n\n"
            L"Yes -- in this folder, beside JSRF Unleashed.exe (portable: the whole folder "
            L"can be moved, to another computer or onto a USB drive).\n\n"
            L"No -- in your user profile (%LOCALAPPDATA%\\JSRF Unleashed).\n\n"
            L"Saves are kept in the game's folder either way. You can change this later: "
            L"start with --portable, or delete portable.txt.",
            APP, MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (r == IDCANCEL) return 1;
        if (r == IDYES) want_portable = 1;
    }
    if (want_portable && !is_file(mark)) {
        FILE *f = _wfopen(mark, L"wb");
        if (!f) {
            wchar_t msg[1024];
            _snwprintf(msg, 1024, L"This folder cannot be written to, so JSRF Unleashed cannot keep "
                                  L"its files in it:\n\n%ls\n\nMove the folder somewhere you can write "
                                  L"to (not Program Files) for a portable install. For now everything "
                                  L"is kept in %ls.", g_here, la);
            msg[1023] = 0;
            say(msg, MB_ICONWARNING);
        } else {
            fputs("While this file is here, JSRF Unleashed keeps its settings, cache, logs and\r\n"
                  "unpacked disc images in this folder. Delete it to keep them in\r\n"
                  "%LOCALAPPDATA%\\JSRF Unleashed instead.\r\n", f);
            fclose(f);
            /* Switching with settings already kept in the profile: take them
             * along (the remembered game, the resolution, ...). */
            if (is_file(la_cfg) && !is_file(here_cfg)) CopyFileW(la_cfg, here_cfg, TRUE);
        }
    }
    g_portable = is_file(mark);
    _snwprintf(g_base, MAX_PATH, L"%ls", g_portable ? g_here : la);
    g_base[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(NULL, g_base, NULL);
    join(g_cfg, MAX_PATH, g_base, L"settings");
    return 0;
}

/* ---- asking where the game is ---------------------------------------------- */

static int pick_folder(wchar_t *out, size_t n)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD opts = 0;
    int ok = 0;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&dlg);
    if (SUCCEEDED(hr)) {
        IFileOpenDialog_GetOptions(dlg, &opts);
        IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(dlg, L"Where is Jet Set Radio Future? Pick the folder that "
                                      L"contains default.xbe (or a disc image)");
        if (SUCCEEDED(IFileOpenDialog_Show(dlg, NULL))
            && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))
            && SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            _snwprintf(out, n, L"%ls", path);
            out[n - 1] = 0;
            ok = 1;
        }
        if (path) CoTaskMemFree(path);
        if (item) IShellItem_Release(item);
        IFileOpenDialog_Release(dlg);
        return ok;
    }
    /* No IFileOpenDialog (an old Wine): the classic folder browser. */
    {
        BROWSEINFOW bi;
        PIDLIST_ABSOLUTE pidl;
        wchar_t buf[MAX_PATH];
        ZeroMemory(&bi, sizeof bi);
        bi.lpszTitle = L"Where is Jet Set Radio Future? Pick the folder that contains default.xbe.";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderW(&bi);
        if (pidl && SHGetPathFromIDListW(pidl, buf)) {
            _snwprintf(out, n, L"%ls", buf);
            out[n - 1] = 0;
            ok = 1;
        }
        if (pidl) CoTaskMemFree(pidl);
    }
    return ok;
}

/* A disc image in the folder that was picked, if there is one. */
static int find_image_in(const wchar_t *dir, wchar_t *out, size_t n)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = 0;
    join(pat, MAX_PATH * 2, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_image_name(fd.cFileName)) {
            char u8[MAX_PATH * 4];
            join(out, n, dir, fd.cFileName);
            to_utf8(out, u8, sizeof u8);
            if (xiso_probe(u8)) { found = 1; break; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* ---- unpacking a disc image, with a progress window ------------------------ */

typedef struct {
    char iso[MAX_PATH * 4], dest[MAX_PATH * 4];
    char err[512];
    volatile LONG percent, done, cancel;
    int rc;
    HWND win, bar, text;
} Unpack;

static int unpack_progress(void *ctx, uint64_t done, uint64_t total)
{
    Unpack *u = (Unpack *)ctx;
    LONG pc = total ? (LONG)(done * 100 / total) : 0;
    InterlockedExchange(&u->percent, pc);
    if (u->win) PostMessageW(u->win, WM_APP, 0, 0);
    return u->cancel ? 1 : 0;
}

static DWORD WINAPI unpack_thread(void *arg)
{
    Unpack *u = (Unpack *)arg;
    u->rc = xiso_extract(u->iso, u->dest, unpack_progress, u, u->err, sizeof u->err);
    InterlockedExchange(&u->done, 1);
    if (u->win) PostMessageW(u->win, WM_APP + 1, 0, 0);
    return 0;
}

static LRESULT CALLBACK unpack_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    Unpack *u = (Unpack *)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (msg) {
    case WM_APP:
        if (u) {
            wchar_t t[64];
            SendMessageW(u->bar, PBM_SETPOS, (WPARAM)u->percent, 0);
            _snwprintf(t, 64, L"Unpacking the disc image... %ld%%", (long)u->percent);
            t[63] = 0;
            SetWindowTextW(u->text, t);
        }
        return 0;
    case WM_APP + 1:
        DestroyWindow(w);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL && u) InterlockedExchange(&u->cancel, 1);
        return 0;
    case WM_CLOSE:
        if (u) InterlockedExchange(&u->cancel, 1);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Unpack iso into dest (created), showing progress. 0 on success. */
static int unpack(const wchar_t *iso, const wchar_t *dest)
{
    static Unpack u;
    WNDCLASSW wc;
    HINSTANCE hi = GetModuleHandleW(NULL);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HANDLE th;
    MSG m;
    RECT r;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    ZeroMemory(&u, sizeof u);
    to_utf8(iso, u.iso, sizeof u.iso);
    to_utf8(dest, u.dest, sizeof u.dest);
    SHCreateDirectoryExW(NULL, dest, NULL);

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = unpack_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"JSRFUnpack";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    SetRect(&r, 0, 0, 420, 120);
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    u.win = CreateWindowW(L"JSRFUnpack", APP, WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                          (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 2,
                          r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    if (u.win) {
        SetWindowLongPtrW(u.win, GWLP_USERDATA, (LONG_PTR)&u);
        u.text = CreateWindowW(L"STATIC", L"Unpacking the disc image... (this happens once)",
                               WS_CHILD | WS_VISIBLE, 16, 14, 388, 20, u.win, NULL, hi, NULL);
        u.bar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE,
                              16, 42, 388, 20, u.win, NULL, hi, NULL);
        {
            HWND b = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   324, 78, 80, 26, u.win, (HMENU)(INT_PTR)IDCANCEL, hi, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
        }
        SendMessageW(u.text, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(u.bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    }
    th = CreateThread(NULL, 0, unpack_thread, &u, 0, NULL);
    if (!th) return fail(L"Could not start unpacking the disc image.");
    if (u.win)
        while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (u.rc) {
        wchar_t msg[1024], werr[512];
        from_utf8(u.err, werr, 512);
        _snwprintf(msg, 1024, L"The disc image could not be unpacked:\n\n%ls", werr);
        msg[1023] = 0;
        if (!u.cancel) say(msg, MB_ICONERROR);
        return 1;
    }
    return 0;
}

/* An image is unpacked into %LOCALAPPDATA%\JSRF Unleashed\<its name> (portable:
 * into this folder), once. */
static int game_from_image(const wchar_t *iso, wchar_t *game, size_t n)
{
    wchar_t name[MAX_PATH], *dot, msg[1024];
    const wchar_t *slash = wcsrchr(iso, L'\\');
    char u8[MAX_PATH * 4];
    _snwprintf(name, MAX_PATH, L"%ls", slash ? slash + 1 : iso);
    name[MAX_PATH - 1] = 0;
    if ((dot = wcsrchr(name, L'.')) != NULL) *dot = 0;
    join(game, n, g_base, name);
    if (is_game(game)) return 0;
    to_utf8(iso, u8, sizeof u8);
    {
        /* What the file is, before minutes and gigabytes go into unpacking
         * it: not an Xbox disc image (a zip...), another game's disc, another
         * build of the game (port/xbe_check.c). */
        char why[640];
        wchar_t w[1024], text[2048];
        int r = jsrf_xbe_check_iso(u8, why, sizeof why);
        if (r != JSRF_XBE_OK) {
            from_utf8(why, w, 1024);
            _snwprintf(text, 2048, L"%hs.\n\n%ls%hs%hs", JSRF_XBE_HEADING, w,
                       r == JSRF_XBE_OTHER ? "\n\n" : "",
                       r == JSRF_XBE_OTHER ? JSRF_XBE_OTHER_ADVICE : "");
            text[2047] = 0;
            say(text, MB_ICONWARNING);
            return 1;
        }
    }
    _snwprintf(msg, 1024, L"JSRF Unleashed will unpack your disc image once, into:\n\n%ls\n\n"
                          L"It takes a minute or two and needs about as much free space as "
                          L"the image. Go ahead?", game);
    msg[1023] = 0;
    if (MessageBoxW(NULL, msg, APP, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK)
        return 1;
    if (unpack(iso, game)) return 1;
    if (!is_game(game))
        return fail(L"The disc image does not look like Jet Set Radio Future "
                    L"(no default.xbe and Media after unpacking).");
    return 0;
}

/* ---- logs: the previous run's is kept (five of them), then a fresh one ----- */

static void rotate_logs(const wchar_t *log, const wchar_t *dir)
{
    wchar_t to[MAX_PATH * 2], pat[MAX_PATH * 2], names[64][MAX_PATH];
    SYSTEMTIME st;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0, i, j;
    if (!is_file(log)) return;
    SHCreateDirectoryExW(NULL, dir, NULL);
    GetLocalTime(&st);
    _snwprintf(to, MAX_PATH * 2, L"%ls\\run-%04u%02u%02u-%02u%02u%02u.log", dir,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    to[MAX_PATH * 2 - 1] = 0;
    MoveFileExW(log, to, MOVEFILE_REPLACE_EXISTING);
    join(pat, MAX_PATH * 2, dir, L"run-*.log");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (count < 64) { wcsncpy(names[count], fd.cFileName, MAX_PATH - 1); names[count][MAX_PATH - 1] = 0; count++; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* The names sort by date: keep the newest five. */
    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++)
            if (wcscmp(names[j], names[i]) > 0) {
                wchar_t t[MAX_PATH];
                wcscpy(t, names[i]); wcscpy(names[i], names[j]); wcscpy(names[j], t);
            }
    for (i = 5; i < count; i++) {
        join(to, MAX_PATH * 2, dir, names[i]);
        DeleteFileW(to);
    }
}

/* ---- is it the standalone US disc? -------------------------------------------
 *
 * The engine is the standalone US disc's program translated address for
 * address; the European and Japanese releases and the Sega GT 2002 combo
 * discs are built differently, start, and crash a few seconds in.
 * port/xbe_check.c compares default.xbe's section table with that build's.
 * Returns 0 if these are its files (or the file cannot be read: the engine
 * checks again), else says what they are and returns 1. */
static int other_release(const wchar_t *game)
{
    wchar_t p[MAX_PATH * 2], text[2048], w[1024];
    unsigned char head[16384];
    char why[512];
    size_t got;
    FILE *f;

    join(p, MAX_PATH * 2, game, L"default.xbe");
    f = _wfopen(p, L"rb");
    if (!f) return 0;
    got = fread(head, 1, sizeof head, f);
    fclose(f);
    if (jsrf_xbe_check(head, got, why, sizeof why) != JSRF_XBE_OTHER) return 0;
    from_utf8(why, w, 1024);
    _snwprintf(text, 2048, L"%hs.\n\n%ls\n\n%hs", JSRF_XBE_HEADING, w, JSRF_XBE_OTHER_ADVICE);
    text[2047] = 0;
    say(text, MB_ICONWARNING);
    return 1;
}

/* ---- the run ---------------------------------------------------------------- */

static const wchar_t *const k_env[][2] = {
    { L"RECOMP_USB", L"1" }, { L"RECOMP_GL", L"1" }, { L"RECOMP_PB_EXEC", L"1" },
    { L"RECOMP_VBLANK", L"1" }, { L"RECOMP_AC97_READY_NOTRAP", L"1" },
    { L"RECOMP_TRACE_BUDGET", L"0" }, { L"RECOMP_GL_READBACK", L"0" }, { L"RECOMP_FPS", L"60" },
    { L"RECOMP_ABI_RESTORE", L"1" }, { L"RECOMP_LOCK_IDLE", L"1" },
    { L"RECOMP_GL_CURTAIN", L"skip" }, { L"RECOMP_MUTE", L"0" }, { L"RECOMP_WINDOW", L"1" },
};

/* fullscreen: 1 or 0 for this start (--fullscreen, --windowed), -1 to leave it
 * to the engine, which starts the way the game was left. */
static int run(const wchar_t *game, int scale, int fullscreen)
{
    wchar_t engine[MAX_PATH * 2], cmd[MAX_PATH * 6], log[MAX_PATH * 2], logs[MAX_PATH * 2],
            save[MAX_PATH * 2], sc[8], msg[2048];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE out;
    DWORD code = 0;
    size_t i;

    join(engine, MAX_PATH * 2, g_here, L"engine\\jsrf_recomp.exe");
    if (!is_file(engine))
        return fail(L"engine\\jsrf_recomp.exe is missing. Unzip the whole download "
                    L"and keep the engine folder beside JSRF Unleashed.exe.");
    join(save, MAX_PATH * 2, g_base, L"save");
    SHCreateDirectoryExW(NULL, save, NULL);
    join(log, MAX_PATH * 2, g_base, L"last-run.log");
    join(logs, MAX_PATH * 2, g_base, L"logs");
    rotate_logs(log, logs);

    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        SetEnvironmentVariableW(k_env[i][0], k_env[i][1]);
    _snwprintf(sc, 8, L"%d", scale);
    SetEnvironmentVariableW(L"RECOMP_GL_SCALE", sc);
    if (fullscreen >= 0) SetEnvironmentVariableW(L"RECOMP_FULLSCREEN", fullscreen ? L"1" : L"0");
    SetEnvironmentVariableW(L"JSRF_GAME_DIR", game);
    SetEnvironmentVariableW(L"JSRF_SAVE_DIR", save);
    SetEnvironmentVariableW(L"JSRF_SETTINGS_FILE", g_cfg);

    ZeroMemory(&sa, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    out = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = NULL;
        si.hStdOutput = out;
        si.hStdError = out;
    }
    _snwprintf(cmd, MAX_PATH * 6, L"\"%ls\" \"%ls\"", engine, game);
    cmd[MAX_PATH * 6 - 1] = 0;
    if (!CreateProcessW(engine, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, game, &si, &pi)) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        _snwprintf(msg, 2048, L"Could not start the engine (error %lu).", GetLastError());
        msg[2047] = 0;
        return fail(msg);
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code == JSRF_XBE_OTHER) {
        /* The engine's own check (if this launcher's could not read the file):
         * its log says what the files are. Ask for the folder next time. */
        cfg_set("game_dir", "");
        _snwprintf(msg, 2048, L"%hs.\n\n%hs\n\n"
                              L"The log says what the files are:\n%ls", JSRF_XBE_HEADING,
                              JSRF_XBE_OTHER_ADVICE, log);
        msg[2047] = 0;
        say(msg, MB_ICONWARNING);
    } else if (code != 0) {
        _snwprintf(msg, 2048, L"JSRF Unleashed stopped unexpectedly (exit %lu).\n\n"
                              L"The log is at:\n%ls\n\nIf you report the problem, please attach it.\n\n"
                              L"Show the log in its folder?", (unsigned long)code, log);
        msg[2047] = 0;
        if (MessageBoxW(NULL, msg, APP, MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES) {
            wchar_t arg[MAX_PATH * 3];
            _snwprintf(arg, MAX_PATH * 3, L"/select,\"%ls\"", log);
            arg[MAX_PATH * 3 - 1] = 0;
            ShellExecuteW(NULL, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
        }
    }
    return (int)code;
}

/* ---- main ------------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show)
{
    int argc = 0, i, choose = 0, fullscreen = -1, scale = 2, portable = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t game[MAX_PATH * 2] = L"", arg[MAX_PATH * 2] = L"", *slash;
    char val[64];
    INITCOMMONCONTROLSEX icc;
    (void)hi; (void)prev; (void)cmdline; (void)show;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    GetModuleFileNameW(NULL, g_here, MAX_PATH);
    if ((slash = wcsrchr(g_here, L'\\')) != NULL) *slash = 0;
    for (i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"--portable")) portable = 1;
    if (choose_base(portable)) return 0;            /* the first-start question was cancelled */

    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--choose")) choose = 1;
        else if (!wcscmp(argv[i], L"--fullscreen")) fullscreen = 1;
        else if (!wcscmp(argv[i], L"--windowed")) fullscreen = 0;
        else if (!wcscmp(argv[i], L"--portable")) ;
        else if (!wcscmp(argv[i], L"--scale") && i + 1 < argc) {
            int s = _wtoi(argv[++i]);
            if (s < 1 || s > 6) return fail(L"--scale takes 1 to 6 (6 is for a 4K screen).");
            snprintf(val, sizeof val, "%d", s);
            cfg_set("scale", val);
        } else if (!wcscmp(argv[i], L"--version")) {
            wchar_t vf[MAX_PATH * 2], msg[256];
            FILE *f;
            char v[64] = "?";
            join(vf, MAX_PATH * 2, g_here, L"VERSION");
            if ((f = _wfopen(vf, L"rb")) != NULL) {
                if (fgets(v, sizeof v, f)) { size_t l = strlen(v); while (l && (v[l-1] == '\n' || v[l-1] == '\r')) v[--l] = 0; }
                fclose(f);
            }
            _snwprintf(msg, 256, L"JSRF Unleashed %hs", v);
            msg[255] = 0;
            say(msg, MB_ICONINFORMATION);
            return 0;
        } else if (argv[i][0] != L'-') {
            _snwprintf(arg, MAX_PATH * 2, L"%ls", argv[i]);
            arg[MAX_PATH * 2 - 1] = 0;
        }
    }

    /* Which game: the argument, else the remembered one, else ask. */
    if (arg[0]) {
        DWORD n = GetFullPathNameW(arg, MAX_PATH * 2, game, NULL);
        if (!n || n >= MAX_PATH * 2) return fail(L"That path is too long.");
    } else if (!choose && game_recall(game, MAX_PATH * 2)) {
        if (!is_game(game)) game[0] = 0;
    }
    if (!game[0] && !choose && g_portable) game_beside(game, MAX_PATH * 2);
    if (!game[0] && !pick_folder(game, MAX_PATH * 2))
        return 0;                                   /* the dialog was cancelled */

    if (is_file(game)) {
        /* default.xbe itself means its folder; any other file is taken for a
         * disc image, and game_from_image says what it is if it is not one. */
        wchar_t *slash = wcsrchr(game, L'\\');
        if (slash && !_wcsicmp(slash + 1, L"default.xbe")) {
            *slash = 0;
        } else {
            wchar_t iso[MAX_PATH * 2];
            wcscpy(iso, game);
            if (game_from_image(iso, game, MAX_PATH * 2)) return 1;
        }
    } else if (is_dir(game) && !is_game(game)) {
        wchar_t iso[MAX_PATH * 2];
        if (find_image_in(game, iso, MAX_PATH * 2)) {
            if (game_from_image(iso, game, MAX_PATH * 2)) return 1;
        }
    }
    if (!is_dir(game)) return fail(L"That folder does not exist.");
    if (!is_game(game))
        return fail(L"That folder has no default.xbe and Media in it. Pick the folder "
                    L"you extracted the game into (or the one with the disc image).");
    if (other_release(game)) {
        cfg_set("game_dir", "");                    /* ask again next time */
        return 1;
    }
    game_store(game);
    if (cfg_get("scale", val, sizeof val)) {
        int s = atoi(val);
        if (s >= 1 && s <= 6) scale = s;
    }
    return run(game, scale, fullscreen);
}
