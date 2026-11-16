/*
 * TinyWeek - ISO 8601 week number in the Windows notification area.
 * Plain Win32 C, no C runtime (CRT). Builds into a single tiny .exe.
 *
 * Features: crisp icon drawn at the tray's real pixel size (DPI-aware),
 * white/black digits following the taskbar theme, refresh at midnight,
 * after sleep/resume, clock change and Explorer restart; single instance;
 * optional "Start with Windows" (HKCU Run key, no admin rights).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define APP_NAME   L"TinyWeek"
#define WM_TRAY    (WM_APP + 1)
#define ID_TIMER   1
#define IDM_SHOW   1
#define IDM_REFRESH 2
#define IDM_AUTORUN 3
#define IDM_EXIT   4
#define IDM_SIZE   10              /* IDM_SIZE + index into SIZES */

static const WCHAR RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR THEME_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

static NOTIFYICONDATAW g_nid;
static HICON g_icon;
static UINT  g_msgTaskbarCreated;
static int   g_week;
static int   g_size = 100;              /* digit size, % of the largest that fits */
static WCHAR g_face[LF_FACESIZE];       /* font face, follows the system UI font */

static const WCHAR SETTINGS_KEY[] = L"Software\\TinyWeek";
static const BYTE SIZES[] = {100, 90, 80, 70, 60};

/* The compiler may emit calls to memset for struct initialisation. */
void *memset(void *d, int c, SIZE_T n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

/* ---------- ISO 8601 week number (pure integer arithmetic) ---------- */
static int jan1_dow(int y)            /* 0 = Monday ... 6 = Sunday, for Jan 1 of y */
{
    int p = y - 1;
    return (p + p / 4 - p / 100 + p / 400) % 7;  /* Jan 1, 1 AD was a Monday */
}
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int weeks_in_year(int y)
{
    int d = jan1_dow(y);
    return (d == 3 || (d == 2 && is_leap(y))) ? 53 : 52;   /* starts Thu, or Wed in leap year */
}
static int iso_week(const SYSTEMTIME *t)
{
    static const short cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int y = t->wYear;
    int ordinal = cum[t->wMonth - 1] + t->wDay + (t->wMonth > 2 && is_leap(y));
    int wday = t->wDayOfWeek ? t->wDayOfWeek : 7;          /* Mon=1 .. Sun=7 */
    int w = (ordinal - wday + 10) / 7;
    if (w < 1)  return weeks_in_year(y - 1);
    if (w > weeks_in_year(y)) return 1;
    return w;
}

/* ---------- Icon rendering ---------- */
static int light_taskbar(void)
{
    DWORD v = 0, cb = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, L"SystemUsesLightTheme",
                 RRF_RT_REG_DWORD, NULL, &v, &cb);
    return v != 0;
}

static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    *(int *)lp = 1;
    return 0;
}
static int font_exists(HDC dc, const WCHAR *face)
{
    LOGFONTW lf = {0};
    int found = 0;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
    return found;
}

/* Use the system UI font (the one Windows uses for menus and dialogs). On
 * Windows 11 that is Segoe UI, while the taskbar clock uses Segoe UI
 * Variable, so prefer the latter to match the clock next to the icon. */
static void pick_font(HDC dc)
{
    NONCLIENTMETRICSW ncm = {0};
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0) && ncm.lfMessageFont.lfFaceName[0])
        lstrcpynW(g_face, ncm.lfMessageFont.lfFaceName, LF_FACESIZE);
    else
        lstrcpynW(g_face, L"Segoe UI", LF_FACESIZE);
    if (!lstrcmpiW(g_face, L"Segoe UI") && font_exists(dc, L"Segoe UI Variable Display"))
        lstrcpynW(g_face, L"Segoe UI Variable Display", LF_FACESIZE);
}

static HFONT digit_font(int em)
{
    return CreateFontW(-em, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, g_face);
}

/* Bounding box of the pixels drawn into the scratch DIB. */
static void ink_bounds(const DWORD *px, int stride, RECT *ink)
{
    ink->left = ink->top = 0x7fff; ink->right = ink->bottom = -1;
    for (int r = 0; r < stride; r++)
        for (int c = 0; c < stride; c++)
            if (px[r * stride + c] & 0xff) {
                if (c < ink->left) ink->left = c;
                if (c > ink->right) ink->right = c;
                if (r < ink->top) ink->top = r;
                if (r > ink->bottom) ink->bottom = r;
            }
}

/* Measures every digit's own ink box at em size `em`. Returns the width of
 * the number with digits placed `gap` px apart; top/bottom = ink rows. */
static int measure(HDC dc, DWORD *px, int stride, const WCHAR *s, int n,
                   int em, int gap, RECT *k, int *top, int *bottom)
{
    HGDIOBJ old = SelectObject(dc, digit_font(em));
    int w = gap * (n - 1);
    *top = 0x7fff; *bottom = -1;
    for (int i = 0; i < n; i++) {
        memset(px, 0, (SIZE_T)stride * stride * 4);
        GdiFlush();
        TextOutW(dc, 0, 0, s + i, 1);
        GdiFlush();
        ink_bounds(px, stride, &k[i]);
        w += k[i].right - k[i].left + 1;
        if (k[i].top < *top) *top = k[i].top;
        if (k[i].bottom > *bottom) *bottom = k[i].bottom;
    }
    DeleteObject(SelectObject(dc, old));
    return w;
}

static HICON make_icon(int week)
{
    int S = GetSystemMetrics(SM_CXSMICON);          /* real tray size at current DPI */
    int T = S * g_size / 100;                       /* target box for the digits */
    int W = S * 2;                                  /* scratch canvas for measuring */
    int gap = T >= 24 ? T / 12 : 1;                 /* tight but readable digit spacing */
    WCHAR txt[4];
    int n = wsprintfW(txt, L"%d", week);

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -W;                      /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    pick_font(dc);

    /* Largest font whose true ink (not the font cell) fits the whole icon.
     * Uniform scaling only: the glyphs keep their proportions. The gap
     * between digits is tightened instead of using the font's spacing. */
    RECT k[3];
    int top, bottom, w, h, em = S;
    w = measure(dc, px, W, txt, n, em, gap, k, &top, &bottom);
    h = bottom - top + 1;
    em = (w * h > 0) ? (w > h ? em * T / w : em * T / h) + 1 : T;
    for (;;) {                                       /* hinting may shift a pixel */
        w = measure(dc, px, W, txt, n, em, gap, k, &top, &bottom);
        h = bottom - top + 1;
        if ((w <= T && h <= T) || em <= 4) break;
        em--;
    }

    /* Final draw: digits one by one, the whole ink box centred. */
    HGDIOBJ old = SelectObject(dc, digit_font(em));
    memset(px, 0, (SIZE_T)W * W * 4);
    GdiFlush();
    int x = (S - w) / 2, y = (S - h) / 2 - top;
    for (int i = 0; i < n; i++) {
        TextOutW(dc, x - k[i].left, y, txt + i, 1);
        x += k[i].right - k[i].left + 1 + gap;
    }
    GdiFlush();
    DeleteObject(SelectObject(dc, old));

    /* Coverage -> alpha. Colour follows taskbar theme. */
    DWORD rgb = light_taskbar() ? 0x000000 : 0xFFFFFF;
    BITMAPINFO bo = bi;
    bo.bmiHeader.biWidth = S; bo.bmiHeader.biHeight = -S;
    DWORD *out;
    HBITMAP color = CreateDIBSection(dc, &bo, DIB_RGB_COLORS, (void **)&out, NULL, 0);
    for (int r = 0; r < S; r++)
        for (int c = 0; c < S; c++) {
            DWORD a = px[r * W + c] & 0xff;
            out[r * S + c] = a ? (a << 24) | rgb : 0;
        }
    static BYTE zeros[128 * 128 / 8];
    HBITMAP mask = CreateBitmap(S, S, 1, 1, zeros);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);

    SelectObject(dc, oldBmp);
    DeleteObject(scratch); DeleteObject(color); DeleteObject(mask);
    DeleteDC(dc);
    return ic;
}

/* ---------- Tray ---------- */
static void long_date(const SYSTEMTIME *t, WCHAR *buf, int cch)
{
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, t, NULL, buf, cch);
}

static void update_tray(HWND hwnd, BOOL add)
{
    SYSTEMTIME t; WCHAR date[80];
    GetLocalTime(&t);
    g_week = iso_week(&t);
    long_date(&t, date, 80);

    HICON old = g_icon;
    g_icon = make_icon(g_week);

    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wsprintfW(g_nid.szTip, L"Week %d\n%s", g_week, date);
    if (add) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    if (old) DestroyIcon(old);
}

static void arm_midnight_timer(HWND hwnd)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    DWORD ms = ((23 - t.wHour) * 3600u + (59 - t.wMinute) * 60u + (60 - t.wSecond)) * 1000u
               - t.wMilliseconds + 1000u;            /* ~00:00:01 */
    SetTimer(hwnd, ID_TIMER, ms, NULL);
}

static void refresh(HWND hwnd) { update_tray(hwnd, FALSE); arm_midnight_timer(hwnd); }

static BOOL autorun_enabled(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ,
                        NULL, NULL, NULL) == ERROR_SUCCESS;
}
/* Writes (enable) or removes (disable) the per-user Run entry. */
static void autorun_set(BOOL enable)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k)) return;
    if (!enable) {
        RegDeleteValueW(k, APP_NAME);
    } else {
        WCHAR path[MAX_PATH + 3];
        path[0] = L'"';
        DWORD n = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        path[n + 1] = L'"'; path[n + 2] = 0;
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)path, (n + 3) * sizeof(WCHAR));
    }
    RegCloseKey(k);
}

/* Digit size is the only setting: HKCU\\Software\\TinyWeek, value "Size". */
static void load_size(void)
{
    DWORD v = 0, cb = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Size", RRF_RT_REG_DWORD, NULL, &v, &cb))
        return;
    for (int i = 0; i < (int)sizeof SIZES; i++)
        if (SIZES[i] == v) g_size = v;
}
static void save_size(void)
{
    HKEY k;
    DWORD v = g_size;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
        return;
    RegSetValueExW(k, L"Size", 0, REG_DWORD, (BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

static void show_date(HWND hwnd)
{
    SYSTEMTIME t; WCHAR date[80], msg[128];
    GetLocalTime(&t);
    long_date(&t, date, 80);
    wsprintfW(msg, L"Week %d\n%s", iso_week(&t), date);
    MessageBoxW(hwnd, msg, L"TinyWeek", MB_OK | MB_ICONINFORMATION);
}

static void show_menu(HWND hwnd, int x, int y)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"Show date");
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"Refresh");
    HMENU sz = CreatePopupMenu();
    for (int i = 0; i < (int)sizeof SIZES; i++) {
        WCHAR label[24];
        wsprintfW(label, i ? L"%d %%" : L"%d %% (largest)", SIZES[i]);
        AppendMenuW(sz, MF_STRING | (SIZES[i] == g_size ? MF_CHECKED : 0), IDM_SIZE + i, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, L"Size");
    AppendMenuW(m, MF_STRING | (autorun_enabled() ? MF_CHECKED : 0), IDM_AUTORUN, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                              x, y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_SHOW:    show_date(hwnd); break;
    case IDM_REFRESH: refresh(hwnd); break;
    case IDM_AUTORUN: autorun_set(!autorun_enabled()); break;
    case IDM_EXIT:    DestroyWindow(hwnd); break;
    default:
        if (cmd >= IDM_SIZE && cmd < IDM_SIZE + sizeof SIZES) {
            g_size = SIZES[cmd - IDM_SIZE];
            save_size();
            update_tray(hwnd, FALSE);
        }
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
            show_menu(hwnd, (short)LOWORD(wp), (short)HIWORD(wp)); break;
        case WM_LBUTTONDBLCLK:
            show_date(hwnd); break;
        }
        return 0;
    case WM_TIMER:                     /* midnight */
    case WM_TIMECHANGE:                /* clock / time zone changed */
        refresh(hwnd); return 0;
    case WM_POWERBROADCAST:            /* resumed from sleep/hibernate */
        if (wp == PBT_APMRESUMEAUTOMATIC) refresh(hwnd);
        return TRUE;
    case WM_SETTINGCHANGE:             /* light/dark theme, fonts, DPI, locale */
    case WM_DISPLAYCHANGE:
        update_tray(hwnd, FALSE); return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_msgTaskbarCreated && msg) {   /* Explorer restarted */
        update_tray(hwnd, TRUE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run(void)
{
    if (CreateMutexW(NULL, TRUE, L"Local\\TinyWeek-1") && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;                                  /* already running */

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    load_size();

    /* Hidden top-level window (never shown): receives tray + broadcast messages. */
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, APP_NAME, APP_NAME, WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, inst, NULL);
    update_tray(hwnd, TRUE);
    arm_midnight_timer(hwnd);
    if (autorun_enabled()) autorun_set(TRUE);       /* keep path valid if the exe was moved */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_icon) DestroyIcon(g_icon);
    return 0;
}

void entry(void) { ExitProcess(run()); }
