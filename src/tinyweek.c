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
#include <winhttp.h>
#include <bcrypt.h>
#include <dwmapi.h>

#ifndef TW_VERSION_STR
#define TW_VERSION_STR "0.0.0"                  /* set by build.sh from VERSION */
#endif
#define REPO L"tonihoohoo/TinyWeek"

#define APP_NAME   L"TinyWeek"
#define WM_TRAY    (WM_APP + 1)
#define ID_TIMER   1
#define IDM_SHOW   1
#define IDM_AUTORUN 3
#define IDM_EXIT   4
#define IDM_SIZE   10              /* IDM_SIZE + index into SIZES */
#define IDM_WEIGHT 20              /* IDM_WEIGHT + index into WEIGHTS */
#define IDM_UPDATE 5
#define IDM_LABEL  6
#define WM_UPDATE  (WM_APP + 2)    /* worker thread -> UI: wParam = UPD_* result */

static const WCHAR RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR THEME_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

static NOTIFYICONDATAW g_nid;
static HICON g_icon;
static UINT  g_msgTaskbarCreated;
static int   g_week;
static int   g_size = 100;              /* digit size, % of the largest that fits */
static int   g_weight = FW_BOLD;        /* digit font weight */
static int   g_label;                   /* 1 = draw "wk" above the number */
static const int g_labPct = 50;         /* share of the icon height for "wk" */
static WCHAR g_face[LF_FACESIZE];       /* font face, follows the system UI font */

static const WCHAR SETTINGS_KEY[] = L"Software\\TinyWeek";
static const BYTE SIZES[] = {100, 90, 80, 70};
static const short WEIGHTS[] = {FW_LIGHT, FW_NORMAL, FW_SEMIBOLD, FW_BOLD};
static const WCHAR *const WEIGHT_NAMES[] = {L"Light", L"Regular", L"Semibold", L"Bold"};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The compiler may emit calls to memset for struct initialisation. */
void *memset(void *d, int c, SIZE_T n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

static int memcmp_(const void *a, const void *b, SIZE_T n)
{
    const BYTE *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
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
    return CreateFontW(-em, 0, 0, 0, g_weight, 0, 0, 0, DEFAULT_CHARSET,
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

/* Largest font at which `s` fits a bw x bh box, by its true ink (not the
 * font cell). Uniform scaling only: glyphs keep their proportions. */
typedef struct { int em, w, h, top; RECT k[4]; } FIT;

static void fit(HDC dc, DWORD *px, int stride, const WCHAR *s, int n,
                int bw, int bh, int gap, FIT *f)
{
    int bottom;
    f->em = bh;
    f->w = measure(dc, px, stride, s, n, f->em, gap, f->k, &f->top, &bottom);
    f->h = bottom - f->top + 1;
    if (f->w > 0 && f->h > 0)
        f->em = (f->w * bh > f->h * bw ? f->em * bw / f->w : f->em * bh / f->h) + 1;
    for (;;) {                                       /* hinting may shift a pixel */
        f->w = measure(dc, px, stride, s, n, f->em, gap, f->k, &f->top, &bottom);
        f->h = bottom - f->top + 1;
        if ((f->w <= bw && f->h <= bh) || f->em <= 3) break;
        f->em--;
    }
}

/* Draws `s` character by character, ink box centred horizontally in the
 * icon (width S) with its top ink row at y. */
static void draw_fit(HDC dc, const WCHAR *s, int n, const FIT *f, int S, int y, int gap)
{
    HGDIOBJ old = SelectObject(dc, digit_font(f->em));
    int x = (S - f->w) / 2;
    for (int i = 0; i < n; i++) {
        TextOutW(dc, x - f->k[i].left, y - f->top, s + i, 1);
        x += f->k[i].right - f->k[i].left + 1 + gap;
    }
    DeleteObject(SelectObject(dc, old));
}

static HICON make_icon(int week)
{
    int S = GetSystemMetrics(SM_CXSMICON);          /* real tray size at current DPI */
    int T = S * g_size / 100;                       /* target box for the text */
    int W = S * 2;                                  /* scratch canvas for measuring */
    int gap = T >= 24 ? T / 12 : 1;                 /* tight but readable spacing */
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

    FIT num, lab;
    if (!g_label) {
        /* Number only: as large as the whole box allows. */
        fit(dc, px, W, txt, n, T, T, gap, &num);
        memset(px, 0, (SIZE_T)W * W * 4);
        GdiFlush();
        draw_fit(dc, txt, n, &num, S, (S - num.h) / 2, gap);
    } else {
        /* "wk" on a small top line, the number below it. */
        int lineGap = T >= 24 ? T / 16 : 1;
        int labH = T * g_labPct / 100;
        fit(dc, px, W, L"wk", 2, T, labH, gap, &lab);
        fit(dc, px, W, txt, n, T, T - lab.h - lineGap, gap, &num);
        int y = (S - (lab.h + lineGap + num.h)) / 2;
        memset(px, 0, (SIZE_T)W * W * 4);
        GdiFlush();
        draw_fit(dc, L"wk", 2, &lab, S, y, gap);
        draw_fit(dc, txt, n, &num, S, y + lab.h + lineGap, gap);
    }
    GdiFlush();

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

/* Settings: HKCU\\Software\\TinyWeek, DWORD values "Size", "Weight", "Label". */
static DWORD get_setting(const WCHAR *name, DWORD def)
{
    DWORD v, cb = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD,
                        NULL, &v, &cb) ? def : v;
}
static void set_setting(const WCHAR *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
        return;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof v);
    RegCloseKey(k);
}
static void load_settings(void)
{
    DWORD v = get_setting(L"Size", 100);
    for (int i = 0; i < COUNT(SIZES); i++)
        if (SIZES[i] == v) g_size = v;
    g_label = get_setting(L"Label", 0) != 0;
    v = get_setting(L"Weight", FW_BOLD);
    for (int i = 0; i < COUNT(WEIGHTS); i++)
        if (WEIGHTS[i] == (int)v) g_weight = v;
}

/* ---------- Themed dialog ----------
 * MessageBox cannot be dark, so TinyWeek draws its own small dialog in the
 * Windows 11 style. It follows the apps light/dark mode, the accent colour
 * and high contrast, uses real STATIC controls so screen readers read the
 * text, and copies its text with Ctrl+C like MessageBox. */
static struct {
    const WCHAR *heading, *body;
    int result, done, dpi, dark, hc, hot;
    HFONT fHead, fBody;
    HBRUSH bgContent, bgFooter;
    COLORREF text, accent, accentText;
    int footerY, iconSz, pad;
    HICON icon;
    HWND hwnd;
    WNDPROC btnProc;
} D;

#define SC(x) MulDiv((x), D.dpi, 96)

static DWORD reg_dword(const WCHAR *key, const WCHAR *name, DWORD def)
{
    DWORD v, cb = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &v, &cb) ? def : v;
}

static COLORREF mix(COLORREF a, COLORREF b, int pctA)
{
    return RGB((GetRValue(a) * pctA + GetRValue(b) * (100 - pctA)) / 100,
               (GetGValue(a) * pctA + GetGValue(b) * (100 - pctA)) / 100,
               (GetBValue(a) * pctA + GetBValue(b) * (100 - pctA)) / 100);
}

/* Accent shade used by Windows 11 for primary buttons: a lighter shade in
 * dark mode (Light 2), a darker one in light mode (Dark 1). */
static void pick_accent(void)
{
    BYTE pal[32];
    DWORD cb = sizeof pal;
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                      L"AccentPalette", RRF_RT_REG_BINARY, NULL, pal, &cb) && cb >= 32) {
        BYTE *c = pal + (D.dark ? 1 : 4) * 4;         /* palette entries are R,G,B,A */
        D.accent = RGB(c[0], c[1], c[2]);
    } else {
        COLORREF base = reg_dword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFD77800) & 0xFFFFFF;
        D.accent = D.dark ? mix(base, RGB(255, 255, 255), 55) : mix(base, RGB(0, 0, 0), 85);
    }
    D.accentText = D.dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

static void draw_button(const DRAWITEMSTRUCT *di)
{
    int primary = di->CtlID == IDOK, pressed = di->itemState & ODS_SELECTED;
    int hot = D.hot == (int)di->CtlID;
    int focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);
    COLORREF fill, border, fg;
    if (D.hc) {                                     /* high contrast: system colours */
        int lit = primary || hot || pressed;
        fill = GetSysColor(lit ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        fg = GetSysColor(lit ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
        border = GetSysColor(COLOR_BTNTEXT);
    } else if (primary) {
        COLORREF bg = D.dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
        fill = pressed ? mix(D.accent, bg, 80) : hot ? mix(D.accent, bg, 90) : D.accent;
        fg = D.accentText;
        border = fill;
    } else if (D.dark) {
        fill = pressed ? RGB(50, 50, 50) : hot ? RGB(61, 61, 61) : RGB(55, 55, 55);
        border = RGB(70, 70, 70); fg = pressed ? RGB(206, 206, 206) : RGB(255, 255, 255);
    } else {
        fill = pressed ? RGB(245, 245, 245) : hot ? RGB(246, 246, 246) : RGB(251, 251, 251);
        border = RGB(214, 214, 214); fg = pressed ? RGB(93, 93, 93) : RGB(26, 26, 26);
    }
    if (focus) border = D.text;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, D.bgFooter);
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focus ? SC(2) : 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, SC(8), SC(8));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pen);
    WCHAR label[40];
    GetWindowTextW(di->hwndItem, label, 40);
    HGDIOBJ of = SelectObject(dc, D.fBody);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

/* Button subclass: hover highlight. */
static LRESULT CALLBACK btn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int id = GetDlgCtrlID(hwnd);
    if (msg == WM_MOUSEMOVE && D.hot != id) {
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        D.hot = id;
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        D.hot = 0;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return CallWindowProcW(D.btnProc, hwnd, msg, wp, lp);
}

/* Ctrl+C: copy the dialog text, like MessageBox does. */
static void copy_text(void)
{
    int n = lstrlenW(D.heading) + lstrlenW(D.body) * 2 + 8;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(WCHAR));
    WCHAR *p = mem ? GlobalLock(mem) : NULL;
    if (!p) return;
    const WCHAR *src = D.heading;
    for (int part = 0; part < 2; part++, src = D.body) {
        for (; *src; src++) {
            if (*src == '\n') *p++ = '\r';
            *p++ = *src;
        }
        if (!part) { *p++ = '\r'; *p++ = '\n'; *p++ = '\r'; *p++ = '\n'; }
    }
    *p = 0;
    GlobalUnlock(mem);
    if (OpenClipboard(D.hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, mem)) mem = NULL;
        CloseClipboard();
    }
    if (mem) GlobalFree(mem);
}

static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc, top, bottom;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        top = bottom = rc;
        top.bottom = bottom.top = D.footerY;
        FillRect(dc, &top, D.bgContent);
        FillRect(dc, &bottom, D.bgFooter);
        DrawIconEx(dc, D.pad, D.pad, D.icon, D.iconSz, D.iconSz, 0, NULL, DI_NORMAL);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, D.text);
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)D.bgContent;
    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLORBTN:
        return (LRESULT)D.bgFooter;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            D.result = LOWORD(wp) == IDOK ? 1 : 2;
            D.done = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        D.result = 2; D.done = 1;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT ui_font(HDC dc, LOGFONTW lf, const WCHAR *variable, int heightPct, int weight)
{
    if (font_exists(dc, variable)) lstrcpynW(lf.lfFaceName, variable, LF_FACESIZE);
    lf.lfHeight = lf.lfHeight * heightPct / 100;
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&lf);
}

/* Shows a modal dialog. Returns 1 for the main button, 2 for the other
 * button, Esc or close. `no` may be NULL for a single-button dialog. */
static int themed_dialog(const WCHAR *heading, const WCHAR *body, const WCHAR *yes, const WCHAR *no)
{
    if (D.hwnd) { SetForegroundWindow(D.hwnd); return 2; }   /* one at a time */
    HINSTANCE inst = GetModuleHandleW(NULL);
    static ATOM cls;
    if (!cls) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"TinyWeekDialog";
        cls = RegisterClassExW(&wc);
    }

    HDC sdc = GetDC(NULL);
    HIGHCONTRASTW hcm = { sizeof hcm };
    D.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    D.heading = heading; D.body = body; D.done = 0; D.result = 2; D.hot = 0;
    D.hc = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hcm, &hcm, 0) && (hcm.dwFlags & HCF_HIGHCONTRASTON);
    D.dark = !D.hc && reg_dword(THEME_KEY, L"AppsUseLightTheme", 1) == 0;
    if (D.hc) {
        D.text = GetSysColor(COLOR_WINDOWTEXT);
        D.bgContent = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        D.bgFooter = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    } else {
        D.text = D.dark ? RGB(255, 255, 255) : RGB(26, 26, 26);
        D.bgContent = CreateSolidBrush(D.dark ? RGB(43, 43, 43) : RGB(255, 255, 255));
        D.bgFooter = CreateSolidBrush(D.dark ? RGB(32, 32, 32) : RGB(243, 243, 243));
        pick_accent();
    }

    /* Windows 11 dialog fonts: Segoe UI Variable, falling back to the
     * system message font on older Windows. */
    NONCLIENTMETRICSW ncm = {0};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    D.fBody = ui_font(sdc, ncm.lfMessageFont, L"Segoe UI Variable Text", 100, FW_NORMAL);
    D.fHead = ui_font(sdc, ncm.lfMessageFont, L"Segoe UI Variable Display", 150, FW_SEMIBOLD);

    /* Layout */
    D.pad = SC(24); D.iconSz = SC(32);
    int textX = D.pad + D.iconSz + SC(16), maxW = SC(400);
    int btnH = SC(32), btnGap = SC(8), btnW[2] = {0, 0};
    RECT rHead = {textX, D.pad, textX + maxW, D.pad}, rBody;
    HGDIOBJ of = SelectObject(sdc, D.fHead);
    DrawTextW(sdc, heading, -1, &rHead, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SetRect(&rBody, textX, rHead.bottom + SC(8), textX + maxW, rHead.bottom + SC(8));
    SelectObject(sdc, D.fBody);
    DrawTextW(sdc, body, -1, &rBody, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    for (int i = 0; i < 2; i++) {
        const WCHAR *t = i ? no : yes;
        RECT r = {0};
        if (!t) continue;
        DrawTextW(sdc, t, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        btnW[i] = r.right + SC(40) > SC(110) ? r.right + SC(40) : SC(110);
    }
    SelectObject(sdc, of);
    ReleaseDC(NULL, sdc);

    int textR = rHead.right > rBody.right ? rHead.right : rBody.right;
    int w = textR + D.pad, buttons = btnW[0] + btnW[1] + (no ? btnGap : 0) + 2 * D.pad;
    if (w < SC(380)) w = SC(380);
    if (w < buttons) w = buttons;
    D.footerY = (rBody.bottom > D.pad + D.iconSz ? rBody.bottom : D.pad + D.iconSz) + D.pad;
    int h = D.footerY + btnH + 2 * D.pad;

    /* Window, centred on the monitor under the mouse */
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW;
    RECT wr = {0, 0, w, h};
    AdjustWindowRectEx(&wr, style, FALSE, ex);
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int x = (mi.rcWork.left + mi.rcWork.right - ww) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - wh) / 2;
    D.icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, D.iconSz, D.iconSz, 0);
    HWND dlg = CreateWindowExW(ex, L"TinyWeekDialog", L"TinyWeek", style, x, y, ww, wh,
                               NULL, NULL, inst, NULL);
    D.hwnd = dlg;
    BOOL dark = D.dark;                              /* dark title bar */
    if (DwmSetWindowAttribute(dlg, 20, &dark, sizeof dark))
        DwmSetWindowAttribute(dlg, 19, &dark, sizeof dark);

    /* Text as STATIC controls, so Narrator and other screen readers read it. */
    HWND sh = CreateWindowExW(0, L"STATIC", heading, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                              rHead.left, rHead.top, rHead.right - rHead.left, rHead.bottom - rHead.top,
                              dlg, NULL, inst, NULL);
    HWND sb = CreateWindowExW(0, L"STATIC", body, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                              rBody.left, rBody.top, rBody.right - rBody.left, rBody.bottom - rBody.top,
                              dlg, NULL, inst, NULL);
    SendMessageW(sh, WM_SETFONT, (WPARAM)D.fHead, 0);
    SendMessageW(sb, WM_SETFONT, (WPARAM)D.fBody, 0);

    int bx = w - D.pad - btnW[0] - (no ? btnGap + btnW[1] : 0), by = D.footerY + D.pad;
    HWND btn[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (i && !no) break;
        btn[i] = CreateWindowExW(0, L"BUTTON", i ? no : yes, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 i ? bx + btnW[0] + btnGap : bx, by, btnW[i], btnH, dlg,
                                 (HMENU)(INT_PTR)(i ? IDCANCEL : IDOK), inst, NULL);
        D.btnProc = (WNDPROC)SetWindowLongPtrW(btn[i], GWLP_WNDPROC, (LONG_PTR)btn_proc);
    }

    /* Focus outlines only after keyboard use, as in standard dialogs. */
    SendMessageW(dlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(btn[0]);

    MSG m;
    while (!D.done && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && (m.hwnd == dlg || IsChild(dlg, m.hwnd))) {
            if (m.wParam == VK_ESCAPE) { SendMessageW(dlg, WM_CLOSE, 0, 0); continue; }
            if (m.wParam == 'C' && GetKeyState(VK_CONTROL) < 0) { copy_text(); continue; }
        }
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (!D.done) PostQuitMessage(0);                 /* app is exiting */
    D.hwnd = NULL;
    DestroyIcon(D.icon);
    DeleteObject(D.fHead); DeleteObject(D.fBody);
    DeleteObject(D.bgContent); DeleteObject(D.bgFooter);
    return D.result;
}

static void show_date(HWND hwnd)
{
    SYSTEMTIME t; WCHAR date[80], head[32];
    GetLocalTime(&t);
    long_date(&t, date, 80);
    wsprintfW(head, L"Week %d", iso_week(&t));
    themed_dialog(head, date, L"OK", NULL);
}

/* ---------- Dark context menu ----------
 * Win32 menus only turn dark through undocumented uxtheme.dll exports
 * (Windows 10 1903+), exported by ordinal only. Missing exports = no-op. */
static void apply_menu_theme(HWND hwnd)
{
    static HMODULE ux;
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    int (WINAPI *SetPreferredAppMode)(int) = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    BOOL (WINAPI *AllowDarkModeForWindow)(HWND, BOOL) = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    void (WINAPI *FlushMenuThemes)(void) = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    HIGHCONTRASTW hc = { sizeof hc };
    int hcOn = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    int dark = !hcOn && !light_taskbar();       /* follow the taskbar, like the tray */
    if (SetPreferredAppMode) SetPreferredAppMode(hcOn ? 0 : dark ? 2 : 3);   /* Default : ForceDark : ForceLight */
    if (AllowDarkModeForWindow) AllowDarkModeForWindow(hwnd, dark);
    if (FlushMenuThemes) FlushMenuThemes();
}

/* ---------- Updates from GitHub Releases ---------- */
enum { UPD_LATEST, UPD_NEWER, UPD_FAILED, UPD_READY, UPD_BAD_FILE };

static volatile LONG g_busy;           /* an update check/download is running */
static char  g_latest[32];             /* newest version on GitHub, e.g. "1.2.0" */
static WCHAR g_notes[1200];            /* its release notes, shown before updating */
static BYTE *g_dl;                     /* downloaded exe */
static DWORD g_dlLen;
static HANDLE g_mutex;

/* HTTPS GET into buf; returns bytes read, or -1 on any error / non-200. */
static int http_get(const WCHAR *host, const WCHAR *path, BYTE *buf, int cap)
{
    int len = -1;
    HINTERNET s = WinHttpOpen(L"TinyWeek", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    DWORD status = 0, sz = sizeof status, got;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, NULL, 0, 0, 0)
          && WinHttpReceiveResponse(r, NULL)
          && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 NULL, &status, &sz, NULL)
          && status == 200) {
        len = 0;
        while (len < cap && WinHttpReadData(r, buf + len, cap - len, &got) && got)
            len += got;
        if (len >= cap) len = -1;              /* too large: refuse */
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return len;
}

/* "1.2.3" / "v1.2.3" -> comparable number */
static DWORD parse_version(const char *v)
{
    DWORD n = 0, part = 0;
    int parts = 0;
    if (*v == 'v' || *v == 'V') v++;
    for (;; v++) {
        if (*v >= '0' && *v <= '9') part = part * 10 + (*v - '0');
        else {
            n = n * 1000 + part; part = 0; parts++;
            if (*v != '.' || parts == 3) break;
        }
    }
    while (parts++ < 3) n *= 1000;
    return n;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Finds "key": in JSON (not inside another string) and returns a pointer
 * just past it, or NULL. */
static char *json_key(char *j, const char *key, int keylen)
{
    for (char *t = j; *t; t++)
        if (t[0] == '"' && (t == j || t[-1] != '\\') && !memcmp_(t + 1, key, keylen) && t[1 + keylen] == '"') {
            t += keylen + 2;
            while (*t == ' ' || *t == ':') t++;
            return t;
        }
    return NULL;
}

/* Decodes the JSON string at q (just after its opening quote) into UTF-8. */
static void json_string(const char *q, char *out, int cap)
{
    int n = 0;
    while (*q && *q != '"' && n < cap - 4) {
        char ch = *q++;
        if (ch == '\\' && *q) {
            ch = *q++;
            if (ch == 'n') ch = '\n';
            else if (ch == 'r') continue;
            else if (ch == 't') ch = ' ';
            else if (ch == 'u') {
                unsigned u = 0;
                for (int i = 0; i < 4 && *q; i++) u = u * 16 + (unsigned)hexval(*q++);
                if (u >= 0xD800 && u < 0xE000) u = '?';
                if (u < 0x80) out[n++] = (char)u;
                else if (u < 0x800) { out[n++] = (char)(0xC0 | u >> 6); out[n++] = (char)(0x80 | (u & 63)); }
                else { out[n++] = (char)(0xE0 | u >> 12); out[n++] = (char)(0x80 | (u >> 6 & 63)); out[n++] = (char)(0x80 | (u & 63)); }
                continue;
            }
        }
        out[n++] = ch;
    }
    out[n] = 0;
}

/* Markdown release notes -> plain text for a message box. */
static void notes_to_text(const char *md)
{
    WCHAR tmp[1200];
    int n = MultiByteToWideChar(CP_UTF8, 0, md, -1, tmp, 1199), o = 0, bol = 1;
    tmp[n > 0 ? n : 0] = 0;
    for (WCHAR *c = tmp; *c && o < 1150; c++) {
        if (bol) {                                  /* start of a line */
            while (*c == '#') c++;
            while (*c == ' ' && c[1] == ' ') c++;
            if (*c == ' ' && (c == tmp || c[-1] == '#')) c++;
            if ((*c == '-' || *c == '*') && c[1] == ' ') { g_notes[o++] = 0x2022; c++; }
            bol = 0;
        }
        if (*c == '*' && c[1] == '*') { c++; continue; }   /* **bold** */
        if (*c == '`') continue;
        if (*c == '\n') {
            WCHAR *d = c + 1;
            while (*d == ' ') d++;
            if (d > c + 1 && *d && *d != '\n' && !((*d == '-' || *d == '*') && d[1] == ' ')) {
                g_notes[o++] = ' ';                 /* wrapped line: join it */
                c = d - 1;
                continue;
            }
            if (o >= 2 && g_notes[o - 1] == '\n' && g_notes[o - 2] == '\n') continue;
            bol = 1;
        }
        g_notes[o++] = *c;
    }
    while (o && (g_notes[o - 1] == '\n' || g_notes[o - 1] == ' ')) o--;
    if (o >= 1150) { g_notes[o++] = 0x2026; }
    g_notes[o] = 0;
}

/* Worker thread. lp = 0: check the latest release. lp = 1: download it. */
static DWORD WINAPI update_thread(LPVOID lp)
{
    HWND hwnd = FindWindowW(APP_NAME, APP_NAME);
    const int cap = 1 << 20;
    BYTE *buf = VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WPARAM result = UPD_FAILED;
    WCHAR path[160];

    if (buf && !lp) {
        int n = http_get(L"api.github.com", L"/repos/" REPO L"/releases/latest", buf, cap - 1);
        if (n > 0) {
            buf[n] = 0;
            char *q = json_key((char *)buf, "tag_name", 8);   /* "tag_name": "v1.2.3" */
            char *b = json_key((char *)buf, "body", 4);        /* release notes */
            g_notes[0] = 0;
            if (b && *b == '"') {
                static char md[2400];
                json_string(b + 1, md, sizeof md);
                notes_to_text(md);
            }
            if (q) {
                if (*q == '"') {
                    q++;
                    int i = 0;
                    if (*q == 'v' || *q == 'V') q++;
                    while (*q && *q != '"' && i < (int)sizeof g_latest - 1) g_latest[i++] = *q++;
                    g_latest[i] = 0;
                    result = parse_version(g_latest) > parse_version(TW_VERSION_STR) ? UPD_NEWER : UPD_LATEST;
                }
            }
        }
    } else if (buf) {
        /* Download the exe and its published SHA-256, and check they match. */
        BYTE want[32], have[32];
        int n, ok = 0;
        wsprintfW(path, L"/" REPO L"/releases/download/v%S/TinyWeek.exe.sha256", g_latest);
        n = http_get(L"github.com", path, buf, 256);
        if (n >= 64) {
            ok = 1;
            for (int i = 0; i < 32; i++) {
                int hi = hexval(buf[2 * i]), lo = hexval(buf[2 * i + 1]);
                if (hi < 0 || lo < 0) { ok = 0; break; }
                want[i] = (BYTE)(hi << 4 | lo);
            }
        }
        wsprintfW(path, L"/" REPO L"/releases/download/v%S/TinyWeek.exe", g_latest);
        n = ok ? http_get(L"github.com", path, buf, cap) : -1;
        result = UPD_FAILED;
        if (n > 0) {
            BCRYPT_ALG_HANDLE alg;
            result = UPD_BAD_FILE;
            if (n > 1024 && buf[0] == 'M' && buf[1] == 'Z'
                && !BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) {
                if (!BCryptHash(alg, NULL, 0, buf, n, have, 32) && !memcmp_(have, want, 32)) {
                    g_dl = buf; g_dlLen = n; buf = NULL;
                    result = UPD_READY;
                }
                BCryptCloseAlgorithmProvider(alg, 0);
            }
        }
    }
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    PostMessageW(hwnd, WM_UPDATE, result, 0);
    return 0;
}

static void start_update(LPVOID stage)
{
    if (InterlockedExchange(&g_busy, 1)) return;
    HANDLE t = CreateThread(NULL, 0, update_thread, stage, 0, NULL);
    if (t) CloseHandle(t); else g_busy = 0;
}

static void open_releases(void)
{
    ShellExecuteW(NULL, L"open", L"https://github.com/" REPO L"/releases/latest", NULL, NULL, SW_SHOWNORMAL);
}

/* Swap the running exe for the downloaded one and restart. Windows allows
 * renaming a running exe, so: self -> self.old, write new, launch it, exit. */
static void install_update(HWND hwnd)
{
    WCHAR self[MAX_PATH], old[MAX_PATH + 8];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH), w = 0;
    lstrcpyW(old, self); lstrcatW(old, L".old");
    DeleteFileW(old);
    if (n && MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING)) {
        HANDLE f = CreateFileW(self, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, g_dl, g_dlLen, &w, NULL) && w == g_dlLen;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (ok) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = NULL;
            if ((INT_PTR)ShellExecuteW(NULL, NULL, self, L"/updated", NULL, SW_SHOWNORMAL) > 32) {
                DestroyWindow(hwnd);
                return;
            }
        }
        DeleteFileW(self);                       /* roll back */
        MoveFileExW(old, self, 0);
    }
    if (themed_dialog(L"Could not update", L"TinyWeek could not replace its own file, for example "
                      L"because the folder needs admin rights.\n\nOpen the download page instead?",
                      L"Open download page", L"Close") == 1)
        open_releases();
    /* the icon may have been removed above: bring it back */
    update_tray(hwnd, TRUE);
}

static void on_update_result(HWND hwnd, WPARAM result)
{
    WCHAR msg[200];
    g_busy = 0;
    switch (result) {
    case UPD_LATEST:
        wsprintfW(msg, L"You have the latest version, TinyWeek %S.", TW_VERSION_STR);
        themed_dialog(L"TinyWeek is up to date", msg, L"OK", NULL);
        break;
    case UPD_NEWER:
        {
            static WCHAR ask[1400];
            wsprintfW(ask, L"TinyWeek %S is available. You have %S.", g_latest, TW_VERSION_STR);
            if (g_notes[0]) {
                lstrcatW(ask, L"\n\nWhat\x2019s new:\n");
                lstrcatW(ask, g_notes);
            }
            if (themed_dialog(L"Update available", ask, L"Update", L"Not now") == 1)
                start_update((LPVOID)1);
        }
        break;
    case UPD_READY:
        install_update(hwnd);
        if (g_dl) { VirtualFree(g_dl, 0, MEM_RELEASE); g_dl = NULL; }
        break;
    case UPD_BAD_FILE:
        themed_dialog(L"Update not installed", L"The downloaded file did not match its published "
                      L"checksum, so it was not installed.", L"OK", NULL);
        break;
    default:
        if (themed_dialog(L"Could not check for updates", L"TinyWeek could not reach GitHub. "
                          L"Check your internet connection, or open the download page in your browser.",
                          L"Open download page", L"Close") == 1)
            open_releases();
    }
}

static void show_menu(HWND hwnd, int x, int y)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"Show date");
    HMENU sz = CreatePopupMenu();
    for (int i = 0; i < COUNT(SIZES); i++) {
        WCHAR label[24];
        wsprintfW(label, i ? L"%d %%" : L"%d %% (largest)", SIZES[i]);
        AppendMenuW(sz, MF_STRING | (SIZES[i] == g_size ? MF_CHECKED : 0), IDM_SIZE + i, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, L"Size");
    HMENU wt = CreatePopupMenu();
    for (int i = 0; i < COUNT(WEIGHTS); i++)
        AppendMenuW(wt, MF_STRING | (WEIGHTS[i] == g_weight ? MF_CHECKED : 0), IDM_WEIGHT + i, WEIGHT_NAMES[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)wt, L"Weight");
    AppendMenuW(m, MF_STRING | (g_label ? MF_CHECKED : 0), IDM_LABEL, L"Show \x201cwk\x201d");
    AppendMenuW(m, MF_STRING | (autorun_enabled() ? MF_CHECKED : 0), IDM_AUTORUN, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (g_busy ? MF_GRAYED : 0), IDM_UPDATE,
                g_busy ? L"Checking for updates 26" : L"Check for updates");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                              x, y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_SHOW:    show_date(hwnd); break;
    case IDM_AUTORUN: autorun_set(!autorun_enabled()); break;
    case IDM_UPDATE:  start_update(0); break;
    case IDM_LABEL:
        g_label = !g_label;
        set_setting(L"Label", g_label);
        update_tray(hwnd, FALSE);
        break;
    case IDM_EXIT:    DestroyWindow(hwnd); break;
    default:
        if (cmd >= IDM_SIZE && cmd < IDM_SIZE + COUNT(SIZES)) {
            g_size = SIZES[cmd - IDM_SIZE];
            set_setting(L"Size", g_size);
            update_tray(hwnd, FALSE);
        } else if (cmd >= IDM_WEIGHT && cmd < IDM_WEIGHT + COUNT(WEIGHTS)) {
            g_weight = WEIGHTS[cmd - IDM_WEIGHT];
            set_setting(L"Weight", g_weight);
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
        apply_menu_theme(hwnd);
        /* fall through */
    case WM_DISPLAYCHANGE:
        update_tray(hwnd, FALSE); return 0;
    case WM_UPDATE:
        on_update_result(hwnd, wp); return 0;
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
    /* After a self-update: the previous instance is exiting; remove its file. */
    WCHAR old[MAX_PATH + 8];
    BOOL updated = 0;
    for (const WCHAR *c = GetCommandLineW(); *c; c++)
        if (!memcmp_(c, L"/updated", 16)) updated = 1;
    GetModuleFileNameW(NULL, old, MAX_PATH);
    lstrcatW(old, L".old");
    for (int i = 0; i < (updated ? 40 : 1) && !DeleteFileW(old)
                    && GetLastError() != ERROR_FILE_NOT_FOUND; i++)
        Sleep(250);

    for (int i = 0;; i++) {                        /* single instance */
        g_mutex = CreateMutexW(NULL, TRUE, L"Local\\TinyWeek-1");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(g_mutex);
        if (!updated || i == 40) return 0;         /* already running */
        Sleep(250);
    }

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    load_settings();

    /* Hidden top-level window (never shown): receives tray + broadcast messages. */
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, APP_NAME, APP_NAME, WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, inst, NULL);
    apply_menu_theme(hwnd);
    update_tray(hwnd, TRUE);
    if (updated) {                                 /* confirm the update */
        g_nid.uFlags = NIF_INFO;
        g_nid.dwInfoFlags = NIIF_INFO;
        lstrcpyW(g_nid.szInfoTitle, L"TinyWeek updated");
        wsprintfW(g_nid.szInfo, L"You now have version %S.", TW_VERSION_STR);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.szInfo[0] = 0;
    }
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
