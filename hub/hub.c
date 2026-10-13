/*
 * TwistedMetal1234Recomp hub.
 *
 * Each game is its own executable (the recompiled game code is linked into
 * it), so the hub is a small menu that lists the games built next to it and
 * starts the chosen one in its own directory. Each row has two buttons: Play
 * starts the game straight away with its saved settings, and Settings opens
 * the game's own launcher (disc selection, settings, mods).
 *
 * The list comes from games.txt beside this executable, one game per line:
 *   Title|Subtitle|relative/path/to/Game.exe|Cover name
 *
 * The window keeps the shape of a jewel-case front cover. The cover of the
 * highlighted game (covers/<id>.png beside the executable, <id> being the
 * game's folder name) fills the background and cross-fades as the highlight
 * moves. Covers are not shipped: any that are missing are downloaded in the
 * background from the libretro-thumbnails project the first time the hub
 * runs, by the cover name in games.txt, and appear as they arrive.
 *
 * Controls: arrow keys + Enter, mouse, or a controller (D-pad + A, B to quit).
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <stdio.h>

#define MAX_GAMES 16
#define CLIENT_W 720          /* jewel-case covers are square */
#define CLIENT_H 720
#define ROW_H 62
#define ROW_GAP 8
#define MARGIN 28
#define LIST_BOTTOM 58        /* space under the list for the hint line */
#define BTN_W 118
#define BTN_GAP 10
#define COL_PLAY 0
#define COL_SETTINGS 1
#define FADE_MS 160
#define TICK_MS 16

#define RED RGB(214, 18, 18)
#define RED_DIM RGB(120, 10, 10)
#define WHITE RGB(245, 245, 245)
#define GREY RGB(170, 170, 170)

typedef struct {
    wchar_t title[96];
    wchar_t subtitle[128];
    wchar_t exe[MAX_PATH];
    wchar_t id[32];
    wchar_t cover_name[96];     /* libretro-thumbnails box art name, or empty */
    int available;
    HBITMAP cover;      /* cover scaled to the client area, or NULL */
} Game;

static Game g_games[MAX_GAMES];
static int g_count;
static int g_sel;
static int g_col;       /* COL_PLAY or COL_SETTINGS */
static HANDLE g_child;
static wchar_t g_dir[MAX_PATH];
static HFONT g_font_item, g_font_small, g_font_btn;

/* Background cross-fade: g_bg_from is fully shown, g_bg_to fades in over it. */
static int g_bg_from, g_bg_to;
static int g_fade = 255;    /* 0..255 */

/* XInput is loaded at run time so the hub starts on systems without it. */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadState;
typedef struct { DWORD packet; PadState pad; } XState;
typedef DWORD (WINAPI *XGetState)(DWORD, XState*);
static XGetState g_xget;
static WORD g_pad_prev;

/* The few GDI+ flat-API calls used to decode and scale the cover PNGs. */
typedef struct { UINT32 version; void* debug_cb; BOOL no_thread; BOOL no_codecs; } GdipStartupInput;
int WINAPI GdiplusStartup(ULONG_PTR* token, const GdipStartupInput* input, void* output);
int WINAPI GdipLoadImageFromFile(const WCHAR* file, void** image);
int WINAPI GdipDisposeImage(void* image);
int WINAPI GdipCreateFromHDC(HDC dc, void** graphics);
int WINAPI GdipDeleteGraphics(void* graphics);
int WINAPI GdipSetInterpolationMode(void* graphics, int mode);
int WINAPI GdipDrawImageRectI(void* graphics, void* image, INT x, INT y, INT w, INT h);

static HBITMAP load_cover(const wchar_t* id) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\covers\\%ls.png", g_dir, id);
    void* image = NULL;
    if (GdipLoadImageFromFile(path, &image) != 0 || !image) return NULL;

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, CLIENT_W, CLIENT_H);
    HGDIOBJ old = SelectObject(mem, bmp);
    void* g = NULL;
    if (GdipCreateFromHDC(mem, &g) == 0 && g) {
        GdipSetInterpolationMode(g, 7);     /* high-quality bicubic */
        GdipDrawImageRectI(g, image, 0, 0, CLIENT_W, CLIENT_H);
        GdipDeleteGraphics(g);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    GdipDisposeImage(image);
    return bmp;
}

static void load_games(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\games.txt", g_dir);
    FILE* f = _wfopen(path, L"rt, ccs=UTF-8");
    if (!f) return;
    wchar_t line[512];
    while (g_count < MAX_GAMES && fgetws(line, 512, f)) {
        size_t n = wcslen(line);
        while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r')) line[--n] = 0;
        if (!n || line[0] == L'#') continue;
        wchar_t* a = wcschr(line, L'|');
        if (!a) continue;
        *a++ = 0;
        wchar_t* b = wcschr(a, L'|');
        if (!b) continue;
        *b++ = 0;
        wchar_t* cover_name = wcschr(b, L'|');
        if (cover_name) *cover_name++ = 0;
        Game* g = &g_games[g_count++];
        if (cover_name) wcsncpy(g->cover_name, cover_name, 95);
        wcsncpy(g->title, line, 95);
        wcsncpy(g->subtitle, a, 127);
        swprintf(g->exe, MAX_PATH, L"%ls\\%ls", g_dir, b);
        for (wchar_t* c = g->exe; *c; c++) if (*c == L'/') *c = L'\\';
        /* The game's folder name doubles as its id (covers/<id>.png). */
        size_t k = 0;
        while (b[k] && b[k] != L'/' && b[k] != L'\\' && k < 31) { g->id[k] = b[k]; k++; }
        g->id[k] = 0;
        g->available = GetFileAttributesW(g->exe) != INVALID_FILE_ATTRIBUTES;
        g->cover = load_cover(g->id);
    }
    fclose(f);
}

/* --- cover download ----------------------------------------------------------
 * Runs on its own thread so a slow or absent connection never holds the menu
 * up; each finished cover is handed to the window with WM_COVER_READY. */
#define WM_COVER_READY (WM_APP + 1)
HRESULT WINAPI URLDownloadToFileW(void* caller, const WCHAR* url, const WCHAR* file,
                                  DWORD reserved, void* callback);

static DWORD WINAPI fetch_covers(LPVOID param) {
    HWND hwnd = (HWND)param;
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\covers", g_dir);
    CreateDirectoryW(dir, NULL);
    for (int i = 0; i < g_count; i++) {
        Game* g = &g_games[i];
        if (g->cover || !g->cover_name[0]) continue;
        wchar_t url[512] = L"https://thumbnails.libretro.com/Sony%20-%20PlayStation/Named_Boxarts/";
        size_t n = wcslen(url);
        for (const wchar_t* c = g->cover_name; *c && n < 500; c++) {
            if (*c == L' ') { wcscpy(url + n, L"%20"); n += 3; }
            else url[n++] = *c;
        }
        wcscpy(url + n, L".png");
        wchar_t file[MAX_PATH], part[MAX_PATH];
        swprintf(file, MAX_PATH, L"%ls\\%ls.png", dir, g->id);
        swprintf(part, MAX_PATH, L"%ls.part", file);
        if (URLDownloadToFileW(NULL, url, part, 0, NULL) == 0 &&
            MoveFileExW(part, file, MOVEFILE_REPLACE_EXISTING))
            PostMessageW(hwnd, WM_COVER_READY, (WPARAM)i, 0);
        else
            DeleteFileW(part);
    }
    return 0;
}

/* Play needs a disc the game already knows about: the path its launcher saved
 * under [disc] in settings.toml, still present on this machine. Without one,
 * Play opens the launcher so the player can choose their disc image. */
static int has_disc(const wchar_t* dir) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\settings.toml", dir);
    FILE* f = _wfopen(path, L"rt, ccs=UTF-8");
    if (!f) return 0;
    wchar_t line[1024];
    int in_disc = 0, found = 0;
    while (fgetws(line, 1024, f)) {
        const wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') p++;
        if (*p == L'[') { in_disc = wcsncmp(p, L"[disc]", 6) == 0; continue; }
        if (!in_disc || wcsncmp(p, L"path", 4) != 0) continue;
        const wchar_t* open = wcschr(p, L'"');
        const wchar_t* close = open ? wcschr(open + 1, L'"') : NULL;
        size_t n = close ? (size_t)(close - open - 1) : 0;
        if (!n || n >= MAX_PATH) break;
        wchar_t disc[MAX_PATH], full[MAX_PATH];
        wcsncpy(disc, open + 1, n);
        disc[n] = 0;
        for (wchar_t* c = disc; *c; c++) if (*c == L'/') *c = L'\\';
        if (disc[1] == L':' || disc[0] == L'\\') wcsncpy(full, disc, MAX_PATH);   /* absolute */
        else swprintf(full, MAX_PATH, L"%ls\\%ls", dir, disc);                    /* beside the game */
        found = GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;
        break;
    }
    fclose(f);
    return found;
}

static void launch(HWND hwnd) {
    if (g_child || g_sel < 0 || g_sel >= g_count) return;
    Game* g = &g_games[g_sel];
    if (!g->available) { MessageBeep(MB_ICONWARNING); return; }

    wchar_t cwd[MAX_PATH];
    wcsncpy(cwd, g->exe, MAX_PATH);
    wchar_t* slash = wcsrchr(cwd, L'\\');
    if (slash) *slash = 0;

    int direct = g_col == COL_PLAY && has_disc(cwd);
    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" %ls", g->exe,
             direct ? L"--no-launcher" : L"--launcher");

    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(g->exe, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) {
        MessageBoxW(hwnd, L"Could not start the game.", g->title, MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    g_child = pi.hProcess;
    ShowWindow(hwnd, SW_HIDE);
}

/* Highlight a row and start fading the background to its cover. */
static void select_row(HWND hwnd, int row) {
    if (row < 0 || row >= g_count) return;
    if (row != g_bg_to) {
        /* Whatever is showing now becomes the base of the new fade. */
        if (g_fade >= 128) g_bg_from = g_bg_to;
        g_bg_to = row;
        g_fade = 0;
    }
    g_sel = row;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void move_sel(HWND hwnd, int d) {
    if (g_count) select_row(hwnd, (g_sel + d + g_count) % g_count);
}

static void set_col(HWND hwnd, int col) {
    g_col = col;
    InvalidateRect(hwnd, NULL, FALSE);
}

static int list_top(void) {
    return CLIENT_H - LIST_BOTTOM - g_count * (ROW_H + ROW_GAP);
}

static RECT row_rect(int row) {
    RECT r;
    r.left = MARGIN;
    r.right = CLIENT_W - MARGIN;
    r.top = list_top() + row * (ROW_H + ROW_GAP);
    r.bottom = r.top + ROW_H;
    return r;
}

static RECT button_rect(int row, int col) {
    RECT rr = row_rect(row), r;
    r.right = rr.right - 12 - (col == COL_PLAY ? BTN_W + BTN_GAP : 0);
    r.left = r.right - BTN_W;
    r.top = rr.top + 11;
    r.bottom = rr.bottom - 11;
    return r;
}

/* Fill a rectangle with a colour at the given opacity (0..255). */
static void fill_alpha(HDC dc, RECT r, COLORREF c, int alpha) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetPixel(mem, 0, 0, c);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, 0 };
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void frame_rect(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

static void blit_cover(HDC dc, int index, int alpha) {
    if (index < 0 || index >= g_count || !g_games[index].cover) return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, g_games[index].cover);
    if (alpha >= 255) {
        BitBlt(dc, 0, 0, CLIENT_W, CLIENT_H, mem, 0, 0, SRCCOPY);
    } else {
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, 0 };
        AlphaBlend(dc, 0, 0, CLIENT_W, CLIENT_H, mem, 0, 0, CLIENT_W, CLIENT_H, bf);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

static void draw_button(HDC dc, RECT r, const wchar_t* text, int focused, int enabled) {
    fill_alpha(dc, r, focused && enabled ? RED : RGB(0, 0, 0), focused && enabled ? 255 : 100);
    frame_rect(dc, r, enabled ? RED : RED_DIM);
    SetTextColor(dc, enabled ? WHITE : GREY);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

static void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, CLIENT_W, CLIENT_H);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT rc = { 0, 0, CLIENT_W, CLIENT_H };

    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem, &rc, black);
    DeleteObject(black);
    blit_cover(mem, g_bg_from, 255);
    blit_cover(mem, g_bg_to, g_fade);

    /* Darken the cover: lightly overall, heavily behind the list. */
    fill_alpha(mem, rc, RGB(0, 0, 0), 70);
    RECT panel = { 0, list_top() - 22, CLIENT_W, CLIENT_H };
    fill_alpha(mem, panel, RGB(0, 0, 0), 75);
    RECT rule = { 0, panel.top, CLIENT_W, panel.top + 3 };
    fill_alpha(mem, rule, RED, 255);

    SetBkMode(mem, TRANSPARENT);

    for (int i = 0; i < g_count; i++) {
        RECT r = row_rect(i);
        fill_alpha(mem, r, i == g_sel ? RED_DIM : RGB(0, 0, 0), i == g_sel ? 95 : 75);
        if (i == g_sel) {
            RECT bar = { r.left, r.top, r.left + 5, r.bottom };
            fill_alpha(mem, bar, RED, 255);
        }

        /* Column 1: the label, not clickable. */
        int label_right = r.right - 12 - 2 * BTN_W - BTN_GAP - 14;
        RECT a = { r.left + 18, r.top + 4, label_right, r.top + 36 };
        SelectObject(mem, g_font_item);
        SetTextColor(mem, g_games[i].available ? WHITE : GREY);
        DrawTextW(mem, g_games[i].title, -1, &a, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT s = { r.left + 18, r.top + 36, label_right, r.bottom - 4 };
        SelectObject(mem, g_font_small);
        SetTextColor(mem, GREY);
        DrawTextW(mem, g_games[i].available ? g_games[i].subtitle : L"Not built",
                  -1, &s, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        /* Columns 2 and 3: Play and Settings. */
        SelectObject(mem, g_font_btn);
        draw_button(mem, button_rect(i, COL_PLAY), L"PLAY",
                    i == g_sel && g_col == COL_PLAY, g_games[i].available);
        draw_button(mem, button_rect(i, COL_SETTINGS), L"SETTINGS",
                    i == g_sel && g_col == COL_SETTINGS, g_games[i].available);
    }
    if (!g_count) {
        RECT e = { MARGIN, CLIENT_H / 2 - 20, CLIENT_W - MARGIN, CLIENT_H / 2 + 20 };
        SelectObject(mem, g_font_item);
        SetTextColor(mem, WHITE);
        DrawTextW(mem, L"games.txt is missing or empty.", -1, &e, DT_CENTER | DT_SINGLELINE);
    }

    RECT h = { MARGIN, CLIENT_H - 44, CLIENT_W - MARGIN, CLIENT_H - 14 };
    SelectObject(mem, g_font_small);
    SetTextColor(mem, GREY);
    DrawTextW(mem, L"Arrows to choose   Enter or A to select   Esc or B to quit",
              -1, &h, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    /* The layout is drawn at its design size and stretched to the window. */
    RECT client;
    GetClientRect(hwnd, &client);
    if (client.right == CLIENT_W && client.bottom == CLIENT_H) {
        BitBlt(dc, 0, 0, CLIENT_W, CLIENT_H, mem, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, CLIENT_W, CLIENT_H, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* Map a point in the (possibly resized) window back to design coordinates. */
static void to_design(HWND hwnd, int* x, int* y) {
    RECT client;
    GetClientRect(hwnd, &client);
    if (client.right > 0 && client.bottom > 0) {
        *x = *x * CLIENT_W / client.right;
        *y = *y * CLIENT_H / client.bottom;
    }
}

/* Keep the client area cover-shaped while the user drags a window edge. */
static void keep_aspect(HWND hwnd, WPARAM edge, RECT* r) {
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRect(&frame, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
    int extra_w = frame.right - frame.left, extra_h = frame.bottom - frame.top;
    int cw = r->right - r->left - extra_w, ch = r->bottom - r->top - extra_h;
    int side;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) side = cw * CLIENT_H / CLIENT_W;
    else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) side = ch;
    else side = cw * CLIENT_H / CLIENT_W > ch ? cw * CLIENT_H / CLIENT_W : ch;
    if (side < 360) side = 360;
    int w = side * CLIENT_W / CLIENT_H + extra_w, h = side + extra_h;
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - w;
    else r->right = r->left + w;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - h;
    else r->bottom = r->top + h;
}

static int row_at(int x, int y) {
    POINT pt = { x, y };
    for (int i = 0; i < g_count; i++) {
        RECT r = row_rect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

/* Which button, if any, is under the point. The label column is not clickable. */
static int button_at(int x, int y, int* row, int* col) {
    POINT pt = { x, y };
    for (int i = 0; i < g_count; i++) {
        for (int c = COL_PLAY; c <= COL_SETTINGS; c++) {
            RECT r = button_rect(i, c);
            if (PtInRect(&r, pt)) { *row = i; *col = c; return 1; }
        }
    }
    return 0;
}

static void tick(HWND hwnd) {
    if (g_child) {
        if (WaitForSingleObject(g_child, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_child);
            g_child = NULL;
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return;
    }
    if (g_fade < 255) {
        g_fade += 255 * TICK_MS / FADE_MS;
        if (g_fade >= 255) { g_fade = 255; g_bg_from = g_bg_to; }
        InvalidateRect(hwnd, NULL, FALSE);
    }
    if (!g_xget || GetForegroundWindow() != hwnd) return;
    WORD now = 0;
    for (DWORD i = 0; i < 4; i++) {
        XState st;
        if (g_xget(i, &st) == ERROR_SUCCESS) now |= st.pad.buttons;
    }
    WORD pressed = now & ~g_pad_prev;
    g_pad_prev = now;
    if (pressed & 0x0001) move_sel(hwnd, -1);           /* D-pad up */
    if (pressed & 0x0002) move_sel(hwnd, 1);            /* D-pad down */
    if (pressed & 0x0004) set_col(hwnd, COL_PLAY);      /* D-pad left */
    if (pressed & 0x0008) set_col(hwnd, COL_SETTINGS);  /* D-pad right */
    if (pressed & 0x1000) launch(hwnd);                 /* A */
    if (pressed & 0x2000) DestroyWindow(hwnd);          /* B */
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: tick(hwnd); return 0;
    case WM_KEYDOWN:
        if (wp == VK_UP) move_sel(hwnd, -1);
        else if (wp == VK_DOWN) move_sel(hwnd, 1);
        else if (wp == VK_LEFT) set_col(hwnd, COL_PLAY);
        else if (wp == VK_RIGHT) set_col(hwnd, COL_SETTINGS);
        else if (wp == VK_TAB) set_col(hwnd, !g_col);
        else if (wp == VK_RETURN || wp == VK_SPACE) launch(hwnd);
        else if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_SIZING: keep_aspect(hwnd, wp, (RECT*)lp); return TRUE;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        to_design(hwnd, &x, &y);
        int row = row_at(x, y), brow, col;
        if (row >= 0 && row != g_sel) select_row(hwnd, row);
        if (button_at(x, y, &brow, &col) && col != g_col) set_col(hwnd, col);
        return 0;
    }
    case WM_LBUTTONUP: {
        int row, col;
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        to_design(hwnd, &x, &y);
        if (button_at(x, y, &row, &col)) {
            select_row(hwnd, row);
            g_col = col;
            launch(hwnd);
        }
        return 0;
    }
    case WM_COVER_READY:
        if (wp < (WPARAM)g_count && !g_games[wp].cover) {
            g_games[wp].cover = load_cover(g_games[wp].id);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;

    ULONG_PTR gdip_token = 0;
    GdipStartupInput gdip_in = { 1, NULL, FALSE, FALSE };
    GdiplusStartup(&gdip_token, &gdip_in, NULL);
    load_games();

    static const wchar_t* xinput_dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !g_xget; i++) {
        HMODULE m = LoadLibraryW(xinput_dlls[i]);
        if (m) g_xget = (XGetState)(void*)GetProcAddress(m, "XInputGetState");
    }

    /* Impact, slanted, is the closest stock Windows face to the series' heavy
     * condensed logo lettering. */
    g_font_item = CreateFontW(-27, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Impact");
    g_font_btn = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Impact");
    g_font_small = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"TwistedMetal1234RecompHub";
    RegisterClassW(&wc);

    /* Resizable, but WM_SIZING keeps the client area cover-shaped; there is no
     * maximize box because a maximized window could not keep that shape. */
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
    RECT r = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"TwistedMetal1234Recomp", style,
                              (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                              (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
                              w, h, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    HANDLE covers = CreateThread(NULL, 0, fetch_covers, hwnd, 0, NULL);
    if (covers) CloseHandle(covers);
    SetTimer(hwnd, 1, TICK_MS, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
