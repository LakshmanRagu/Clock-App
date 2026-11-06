// ============================================================
//  Clock & Timers — Win32 + GDI+ (Windows 11 style)
//  Build:  cl /std:c++17 /EHsc /DUNICODE /D_UNICODE main.cpp
//          /link /SUBSYSTEM:WINDOWS
//  Requires: Windows 10 SDK (Win11 recommended for rounded corners)
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------- layout
static const int   WIN_W = 420;
static const int   WIN_H = 540;
static const float TOPBAR_H = 44.f;

// ---------------------------------------------------------------- colors
static const Color C_BG      (255,  32,  32,  32);
static const Color C_CARD    (255,  43,  43,  43);
static const Color C_CARD2   (255,  38,  38,  38);
static const Color C_TEXT    (255, 255, 255, 255);
static const Color C_SUBTEXT (255, 150, 150, 150);
static const Color C_ACCENT  (255,  96, 205, 255);
static const Color C_DANGER  (255, 255,  99,  99);
static const Color C_BTNBG   (255,  45,  45,  45);
static const Color C_BTNHOV  (255,  58,  58,  58);
static const Color C_BTNDWN  (255,  70,  70,  70);

// ---------------------------------------------------------------- ids
enum {
    IDC_MIN = 100, IDC_CLOSE,
    IDC_TAB0, IDC_TAB1, IDC_TAB2,
    IDC_SW_START, IDC_SW_LAP, IDC_SW_RESET,
    IDC_T_MINUS60, IDC_T_MINUS10, IDC_T_PLUS10, IDC_T_PLUS60,
    IDC_T_START, IDC_T_RESET,
    IDC_P1, IDC_P5, IDC_P10, IDC_P25,
    IDC_BLANK = -1
};

enum { HK_SW = 1, HK_TMR, HK_ADD, HK_SHOW, HK_RESET };
enum { WM_TRAY = WM_APP + 1 };

// ---------------------------------------------------------------- state
struct UIButton {
    int          id;
    RECT         rc;
    std::wstring label;
    bool         primary;
    bool         enabled;
};

struct Stopwatch {
    bool         running  = false;
    double       accum    = 0.0;
    LONGLONG     startQpc = 0;
    struct Lap { double total; double split; };
    std::vector<Lap> laps;
};

struct CountdownTimer {
    double    total     = 300.0;
    double    remaining = 300.0;
    bool      running   = false;
    bool      finished  = false;
    ULONGLONG lastTick  = 0;
};

static HWND              g_hwnd    = nullptr;
static HICON             g_icon    = nullptr;
static NOTIFYICONDATAW   g_nid     = {};
static bool              g_tray    = false;
static int               g_tab     = 0;
static int               g_hover   = -1;
static int               g_pressed = -1;
static UINT              g_interval = 0;
static float             g_scale   = 1.0f;
static LARGE_INTEGER     g_qpcFreq = {};
static std::vector<UIButton> g_buttons;

static Stopwatch      g_sw;
static CountdownTimer g_timer;

// ---------------------------------------------------------------- helpers
static void FillRound(Graphics& g, const RectF& r, float rad, const Color& c)
{
    GraphicsPath p;
    float d = rad * 2.f;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    p.CloseFigure();
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void DrawStr(Graphics& g, const std::wstring& text, const Font& font,
                    const RectF& rc, const Color& color,
                    StringAlignment ha = StringAlignmentNear,
                    StringAlignment va = StringAlignmentCenter)
{
    StringFormat sf;
    sf.SetAlignment(ha);
    sf.SetLineAlignment(va);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush br(color);
    g.DrawString(text.c_str(), (INT)text.size(), &font, rc, &sf, &br);
}

static std::wstring FormatStopwatch(double t)
{
    if (t < 0) t = 0;
    int totalCs = (int)(t * 100.0 + 0.5);
    int cs = totalCs % 100;
    int ts = totalCs / 100;
    int s  = ts % 60;
    int m  = (ts / 60) % 60;
    int h  = ts / 3600;
    wchar_t buf[64];
    if (h > 0) swprintf_s(buf, L"%d:%02d:%02d.%02d", h, m, s, cs);
    else       swprintf_s(buf, L"%02d:%02d.%02d", m, s, cs);
    return buf;
}

static std::wstring FormatTimer(double t)
{
    if (t < 0) t = 0;
    int total = (int)ceil(t - 1e-9);
    int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    wchar_t buf[64];
    if (h > 0) swprintf_s(buf, L"%d:%02d:%02d", h, m, s);
    else       swprintf_s(buf, L"%02d:%02d", m, s);
    return buf;
}

static double SwElapsed()
{
    double t = g_sw.accum;
    if (g_sw.running) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        t += double(now.QuadPart - g_sw.startQpc) / double(g_qpcFreq.QuadPart);
    }
    return t;
}

// ---------------------------------------------------------------- buttons
static void AddButton(Graphics& g, int id, float x, float y, float w, float h,
                      const std::wstring& label, bool primary, bool enabled = true)
{
    RECT r = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    g_buttons.push_back({ id, r, label, primary, enabled });

    bool hov = (g_hover == id), prs = (g_pressed == id);
    Color bg, fg;

    if (primary) {
        bg = prs ? Color(255, 70, 175, 220)
                 : (hov ? Color(255, 125, 218, 255) : C_ACCENT);
        fg = Color(255, 8, 26, 38);
    } else {
        bg = prs ? C_BTNDWN : (hov ? C_BTNHOV : C_BTNBG);
        fg = C_TEXT;
    }
    if (!enabled) { bg = Color(255, 38, 38, 38); fg = Color(255, 110, 110, 110); }

    FillRound(g, RectF(x, y, w, h), 6.f, bg);
    Font f(L"Segoe UI", 14.f, FontStyleRegular, UnitPixel);
    DrawStr(g, label, f, RectF(x, y, w, h), fg, StringAlignmentCenter);
}

static void AddGlyphButton(Graphics& g, int id, float x, float y, float w, float h,
                           const wchar_t* glyph, bool danger)
{
    RECT r = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    g_buttons.push_back({ id, r, L"", false, true });

    bool hov = (g_hover == id), prs = (g_pressed == id);
    if (hov || prs) {
        Color c = danger ? (prs ? Color(255, 160, 30, 20) : Color(255, 196, 43, 28))
                         : (prs ? C_BTNDWN : C_BTNHOV);
        FillRound(g, RectF(x, y, w, h), 6.f, c);
    }
    Font f(L"Segoe MDL2 Assets", 11.f, FontStyleRegular, UnitPixel);
    DrawStr(g, glyph, f, RectF(x, y, w, h), C_TEXT, StringAlignmentCenter);
}

// ---------------------------------------------------------------- drawing
static void DrawTopBar(Graphics& g, int W)
{
    Font fTitle(L"Segoe UI Semibold", 14.f, FontStyleRegular, UnitPixel);
    DrawStr(g, L"Clock & Timers", fTitle,
            RectF(18.f, 0.f, 240.f, TOPBAR_H), C_TEXT);

    float bw = 44.f, bh = 32.f, by = 6.f;
    float bxClose = W - 12.f - bw;
    float bxMin   = bxClose - bw - 2.f;
    AddGlyphButton(g, IDC_MIN,   bxMin,   by, bw, bh, L"\uE921", false);
    AddGlyphButton(g, IDC_CLOSE, bxClose, by, bw, bh, L"\uE8BB", true);
}

static void DrawTabs(Graphics& g, int W)
{
    const wchar_t* names[3] = { L"Clock", L"Stopwatch", L"Timer" };
    const int ids[3] = { IDC_TAB0, IDC_TAB1, IDC_TAB2 };

    float tw = 118.f, th = 36.f, gap = 6.f, y = 54.f;
    float total = tw * 3 + gap * 2;
    float x0 = (W - total) / 2.f;

    FillRound(g, RectF(x0 - 5.f, y - 5.f, total + 10.f, th + 10.f), 9.f, C_CARD2);

    Font f(L"Segoe UI", 14.f, FontStyleRegular, UnitPixel);
    for (int i = 0; i < 3; ++i) {
        float x = x0 + i * (tw + gap);
        RECT r = { (LONG)x, (LONG)y, (LONG)(x + tw), (LONG)(y + th) };
        g_buttons.push_back({ ids[i], r, L"", false, true });

        bool sel = (g_tab == i);
        bool hov = (g_hover == ids[i]);

        if (sel || hov)
            FillRound(g, RectF(x, y, tw, th), 7.f,
                      sel ? Color(255, 62, 62, 62) : Color(255, 50, 50, 50));

        DrawStr(g, names[i], f, RectF(x, y, tw, th),
                sel ? C_TEXT : C_SUBTEXT, StringAlignmentCenter);
    }
}

static void DrawClockTab(Graphics& g, int W)
{
    SYSTEMTIME st; GetLocalTime(&st);

    wchar_t tbuf[32];
    swprintf_s(tbuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    Font fBig(L"Segoe UI", 68.f, FontStyleRegular, UnitPixel);
    DrawStr(g, tbuf, fBig, RectF(0, 108, (REAL)W, 96), C_TEXT, StringAlignmentCenter);

    wchar_t dbuf[128] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd, MMMM d, yyyy",
                    dbuf, 128, nullptr);
    Font fDate(L"Segoe UI", 15.f, FontStyleRegular, UnitPixel);
    DrawStr(g, dbuf, fDate, RectF(0, 208, (REAL)W, 24), C_SUBTEXT, StringAlignmentCenter);

    RectF card(24.f, 262.f, (REAL)W - 48.f, 226.f);
    FillRound(g, card, 8.f, C_CARD);

    Font fHead(L"Segoe UI", 12.f, FontStyleBold, UnitPixel);
    DrawStr(g, L"GLOBAL HOTKEYS",
            fHead, RectF(card.X + 18.f, card.Y + 14.f, 240.f, 18.f), C_SUBTEXT);

    struct { const wchar_t* desc; const wchar_t* key; } rows[] = {
        { L"Start / stop stopwatch",    L"Ctrl + Alt + S" },
        { L"Start / pause timer",       L"Ctrl + Alt + T" },
        { L"Add 1 minute to timer",     L"Ctrl + Alt + A" },
        { L"Reset everything",          L"Ctrl + Alt + R" },
        { L"Show / hide this window",   L"Ctrl + Alt + W" },
    };
    Font fDesc(L"Segoe UI", 13.f, FontStyleRegular, UnitPixel);
    Font fKey(L"Segoe UI", 12.f, FontStyleRegular, UnitPixel);

    float ry = card.Y + 46.f;
    for (auto& r : rows) {
        DrawStr(g, r.desc, fDesc, RectF(card.X + 18.f, ry, 200.f, 26.f), C_TEXT);
        DrawStr(g, r.key, fKey,
                RectF(card.X + card.Width - 178.f, ry, 160.f, 26.f),
                C_ACCENT, StringAlignmentFar);
        ry += 34.f;
    }
}

static void DrawStopwatchTab(Graphics& g, int W)
{
    double t = SwElapsed();
    Font fBig(L"Segoe UI", 62.f, FontStyleRegular, UnitPixel);
    DrawStr(g, FormatStopwatch(t), fBig,
            RectF(0, 100, (REAL)W, 88), C_TEXT, StringAlignmentCenter);

    float bw = 112.f, bh = 44.f, gap = 12.f;
    float total = bw * 3 + gap * 2;
    float x = (W - total) / 2.f, y = 206.f;

    AddButton(g, IDC_SW_START, x, y, bw, bh,
              g_sw.running ? L"Stop" : L"Start", true);
    AddButton(g, IDC_SW_LAP,   x + (bw + gap), y, bw, bh, L"Lap", false,
              g_sw.running || t > 0.0);
    AddButton(g, IDC_SW_RESET, x + 2 * (bw + gap), y, bw, bh, L"Reset", false,
              t > 0.0);

    RectF card(24.f, 280.f, (REAL)W - 48.f, 220.f);
    FillRound(g, card, 8.f, C_CARD);

    Font fHead(L"Segoe UI", 12.f, FontStyleBold, UnitPixel);
    DrawStr(g, L"LAPS", fHead, RectF(card.X + 18.f, card.Y + 12.f, 200.f, 18.f), C_SUBTEXT);

    wchar_t cnt[16];
    swprintf_s(cnt, L"%d", (int)g_sw.laps.size());
    Font fCnt(L"Segoe UI", 12.f, FontStyleRegular, UnitPixel);
    DrawStr(g, cnt, fCnt,
            RectF(card.X + card.Width - 60.f, card.Y + 12.f, 42.f, 18.f),
            C_SUBTEXT, StringAlignmentFar);

    if (g_sw.laps.empty()) {
        Font fE(L"Segoe UI", 13.f, FontStyleRegular, UnitPixel);
        DrawStr(g, L"Press Lap to record splits", fE,
                RectF(card.X, card.Y + 90.f, card.Width, 24.f),
                C_SUBTEXT, StringAlignmentCenter);
        return;
    }

    Font fRow(L"Segoe UI", 13.f, FontStyleRegular, UnitPixel);
    Font fNum(L"Segoe UI", 13.f, FontStyleRegular, UnitPixel);

    int count = (int)g_sw.laps.size();
    int shown = count < 7 ? count : 7;
    float ry = card.Y + 40.f;

    for (int i = 0; i < shown; ++i) {
        int idx = count - 1 - i;
        const auto& lap = g_sw.laps[idx];

        wchar_t lab[32];
        swprintf_s(lab, L"Lap %d", idx + 1);
        DrawStr(g, lab, fRow, RectF(card.X + 18.f, ry, 160.f, 24.f), C_TEXT);

        DrawStr(g, FormatStopwatch(lap.split), fNum,
                RectF(card.X + card.Width - 200.f, ry, 110.f, 24.f),
                C_ACCENT, StringAlignmentFar);

        DrawStr(g, FormatStopwatch(lap.total), fNum,
                RectF(card.X + card.Width - 100.f, ry, 82.f, 24.f),
                C_SUBTEXT, StringAlignmentFar);

        ry += 24.f;
    }
}

static void DrawTimerTab(Graphics& g, int W)
{
    Color timeColor = C_TEXT;
    if (g_timer.finished) {
        bool on = ((GetTickCount64() / 400) % 2) == 0;
        timeColor = on ? C_DANGER : Color(255, 110, 60, 60);
    }

    Font fBig(L"Segoe UI", 62.f, FontStyleRegular, UnitPixel);
    DrawStr(g, FormatTimer(g_timer.remaining), fBig,
            RectF(0, 96, (REAL)W, 88), timeColor, StringAlignmentCenter);

    // progress bar
    float barW = 300.f, barX = (W - barW) / 2.f, barY = 194.f, barH = 6.f;
    FillRound(g, RectF(barX, barY, barW, barH), 3.f, C_CARD);
    double frac = (g_timer.total > 0.0) ? (g_timer.remaining / g_timer.total) : 0.0;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    if (frac > 0.001)
        FillRound(g, RectF(barX, barY, (float)(barW * frac), barH), 3.f,
                  g_timer.finished ? C_DANGER : C_ACCENT);

    // adjust row
    float aw = 84.f, ah = 38.f, agap = 8.f;
    float atot = aw * 4 + agap * 3;
    float ax = (W - atot) / 2.f, ay = 216.f;
    AddButton(g, IDC_T_MINUS60, ax, ay, aw, ah, L"-1 min", false);
    AddButton(g, IDC_T_MINUS10, ax + (aw + agap), ay, aw, ah, L"-10 s", false);
    AddButton(g, IDC_T_PLUS10,  ax + 2 * (aw + agap), ay, aw, ah, L"+10 s", false);
    AddButton(g, IDC_T_PLUS60,  ax + 3 * (aw + agap), ay, aw, ah, L"+1 min", false);

    // start / reset
    float sw2 = 176.f, sh = 44.f, sgap = 10.f;
    float sx = (W - (sw2 * 2 + sgap)) / 2.f, sy = 268.f;
    AddButton(g, IDC_T_START, sx, sy, sw2, sh,
              g_timer.running ? L"Pause" : L"Start", true);
    AddButton(g, IDC_T_RESET, sx + sw2 + sgap, sy, sw2, sh, L"Reset", false);

    // status
    const wchar_t* status = g_timer.finished ? L"TIME'S UP"
                          : g_timer.running  ? L"Running"
                                             : L"Ready";
    Font fStat(L"Segoe UI", 13.f, FontStyleRegular, UnitPixel);
    DrawStr(g, status, fStat, RectF(0, 324, (REAL)W, 22),
            g_timer.finished ? C_DANGER : C_SUBTEXT, StringAlignmentCenter);

    // presets
    Font fHead(L"Segoe UI", 12.f, FontStyleBold, UnitPixel);
    DrawStr(g, L"PRESETS", fHead, RectF(30.f, 360.f, 200.f, 18.f), C_SUBTEXT);

    float pw = 84.f, ph = 38.f, pgap = 8.f;
    float ptot = pw * 4 + pgap * 3;
    float px = (W - ptot) / 2.f, py = 386.f;
    AddButton(g, IDC_P1,  px,                    py, pw, ph, L"1 min",  false);
    AddButton(g, IDC_P5,  px + (pw + pgap),      py, pw, ph, L"5 min",  false);
    AddButton(g, IDC_P10, px + 2 * (pw + pgap),  py, pw, ph, L"10 min", false);
    AddButton(g, IDC_P25, px + 3 * (pw + pgap),  py, pw, ph, L"25 min", false);
}

// ---------------------------------------------------------------- paint
static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    int pw = (int)(WIN_W * g_scale + 0.5f);
    int ph = (int)(WIN_H * g_scale + 0.5f);

    Bitmap bmp(pw, ph, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(g_scale, g_scale);

    g_buttons.clear();

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0.f, 0.f, (REAL)WIN_W, (REAL)WIN_H);

    DrawTopBar(g, WIN_W);
    DrawTabs(g, WIN_W);

    switch (g_tab) {
        case 0: DrawClockTab(g, WIN_W); break;
        case 1: DrawStopwatchTab(g, WIN_W); break;
        case 2: DrawTimerTab(g, WIN_W); break;
    }

    Graphics screen(hdc);
    screen.DrawImage(&bmp, 0, 0, pw, ph);

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- actions
static void SyncTickRate();

static void OnTimerFinished()
{
    MessageBeep(MB_ICONASTERISK);
    FLASHWINFO fi = { sizeof(FLASHWINFO), g_hwnd, FLASHW_ALL, 5, 0 };
    FlashWindowEx(&fi);
}

static void ToggleStopwatch()
{
    if (g_sw.running) {
        g_sw.accum   = SwElapsed();
        g_sw.running = false;
    } else {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        g_sw.startQpc = now.QuadPart;
        g_sw.running  = true;
    }
    SyncTickRate();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ResetStopwatch()
{
    g_sw.running = false;
    g_sw.accum   = 0.0;
    g_sw.laps.clear();
    SyncTickRate();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void AddLap()
{
    double total = SwElapsed();
    if (total <= 0.0) return;
    double prev = g_sw.laps.empty() ? 0.0 : g_sw.laps.back().total;
    g_sw.laps.push_back({ total, total - prev });
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ToggleTimer()
{
    if (g_timer.running) {
        g_timer.running = false;
    } else {
        if (g_timer.remaining <= 0.0) g_timer.remaining = g_timer.total;
        if (g_timer.remaining <= 0.0) g_timer.remaining = 300.0;
        g_timer.running  = true;
        g_timer.finished = false;
        g_timer.lastTick = GetTickCount64();
    }
    SyncTickRate();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ResetTimer()
{
    g_timer.running   = false;
    g_timer.finished  = false;
    g_timer.remaining = g_timer.total;
    SyncTickRate();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void AdjustTimer(double delta)
{
    if (g_timer.running) {
        g_timer.remaining += delta;
        if (g_timer.remaining < 0.0) g_timer.remaining = 0.0;
    } else {
        g_timer.total += delta;
        if (g_timer.total < 5.0) g_timer.total = 5.0;
        g_timer.remaining = g_timer.total;
    }
    g_timer.finished = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SetPreset(double seconds)
{
    g_timer.running   = false;
    g_timer.finished  = false;
    g_timer.total     = seconds;
    g_timer.remaining = seconds;
    SyncTickRate();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ResetAll()
{
    ResetStopwatch();
    ResetTimer();
}

// ---------------------------------------------------------------- tick
static void SyncTickRate()
{
    UINT want = (g_sw.running || g_timer.running || g_timer.finished) ? 33u : 500u;
    if (want != g_interval) {
        g_interval = want;
        SetTimer(g_hwnd, 1, want, nullptr);
    }
}

static void OnTick()
{
    if (g_timer.running) {
        ULONGLONG now = GetTickCount64();
        double dt = double(now - g_timer.lastTick) / 1000.0;
        g_timer.lastTick = now;
        g_timer.remaining -= dt;
        if (g_timer.remaining <= 0.0) {
            g_timer.remaining = 0.0;
            g_timer.running   = false;
            g_timer.finished  = true;
            OnTimerFinished();
        }
    }
    SyncTickRate();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- tray
static HICON CreateClockIcon(int size)
{
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    SolidBrush bgC(Color(255, 96, 205, 255));
    g.FillEllipse(&bgC, 1.f, 1.f, (REAL)size - 2.f, (REAL)size - 2.f);

    Pen p(Color(255, 20, 30, 40), size / 11.f);
    p.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    float c = size / 2.f;
    g.DrawLine(&p, c, c, c, c - size * 0.30f);
    g.DrawLine(&p, c, c, c + size * 0.20f, c);

    HBITMAP hbm = nullptr;
    bmp.GetHBITMAP(Color(0, 0, 0, 0), &hbm);

    std::vector<BYTE> maskBits(((size + 31) / 32) * 4 * size, 0);
    HBITMAP hMask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = hbm;
    ii.hbmMask  = hMask;
    HICON icon  = CreateIconIndirect(&ii);

    DeleteObject(hbm);
    DeleteObject(hMask);
    return icon;
}

static void AddTrayIcon(HWND hwnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = g_icon;
    wcscpy_s(g_nid.szTip, L"Clock & Timers");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray = true;
}

static void RemoveTrayIcon()
{
    if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; }
}

static void ToggleWindow()
{
    if (IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_HIDE);
    } else {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
}

static void ShowTrayMenu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Show / Hide");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, L"Toggle Stopwatch");
    AppendMenuW(m, MF_STRING, 3, L"Toggle Timer");
    AppendMenuW(m, MF_STRING, 4, L"Add 1 Minute to Timer");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"Exit");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = (int)TrackPopupMenu(m,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    PostMessage(hwnd, WM_NULL, 0, 0);

    switch (cmd) {
        case 1: ToggleWindow(); break;
        case 2: ToggleStopwatch(); break;
        case 3: ToggleTimer(); break;
        case 4: AdjustTimer(60.0); break;
        case 5: DestroyWindow(hwnd); break;
    }
}

// ---------------------------------------------------------------- chrome
static void ApplyWindowChrome(HWND hwnd)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // immersive dark mode
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // fallback id

    int pref = 2; // DWMWCP_ROUND
    HRESULT hr = DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
    if (FAILED(hr)) {
        RECT rc; GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 16, 16);
        SetWindowRgn(hwnd, rgn, TRUE);
    }
}

// ---------------------------------------------------------------- wndproc
static int HitTest(int x, int y)
{
    int lx = (int)(x / g_scale);
    int ly = (int)(y / g_scale);
    for (auto& b : g_buttons)
        if (lx >= b.rc.left && lx < b.rc.right && ly >= b.rc.top && ly < b.rc.bottom)
            return b.id;
    return IDC_BLANK;
}

static void DoAction(int id)
{
    switch (id) {
        case IDC_MIN:       ShowWindow(g_hwnd, SW_HIDE); break;
        case IDC_CLOSE:     DestroyWindow(g_hwnd); break;

        case IDC_TAB0:      g_tab = 0; InvalidateRect(g_hwnd, nullptr, FALSE); break;
        case IDC_TAB1:      g_tab = 1; InvalidateRect(g_hwnd, nullptr, FALSE); break;
        case IDC_TAB2:      g_tab = 2; InvalidateRect(g_hwnd, nullptr, FALSE); break;

        case IDC_SW_START:  ToggleStopwatch(); break;
        case IDC_SW_LAP:    AddLap(); break;
        case IDC_SW_RESET:  ResetStopwatch(); break;

        case IDC_T_MINUS60: AdjustTimer(-60.0); break;
        case IDC_T_MINUS10: AdjustTimer(-10.0); break;
        case IDC_T_PLUS10:  AdjustTimer(10.0); break;
        case IDC_T_PLUS60:  AdjustTimer(60.0); break;
        case IDC_T_START:   ToggleTimer(); break;
        case IDC_T_RESET:   ResetTimer(); break;

        case IDC_P1:        SetPreset(60.0); break;
        case IDC_P5:        SetPreset(300.0); break;
        case IDC_P10:       SetPreset(600.0); break;
        case IDC_P25:       SetPreset(1500.0); break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        ApplyWindowChrome(hwnd);
        AddTrayIcon(hwnd);

        QueryPerformanceFrequency(&g_qpcFreq);

        RegisterHotKey(hwnd, HK_SW,    MOD_CONTROL | MOD_ALT, 'S');
        RegisterHotKey(hwnd, HK_TMR,   MOD_CONTROL | MOD_ALT, 'T');
        RegisterHotKey(hwnd, HK_ADD,   MOD_CONTROL | MOD_ALT, 'A');
        RegisterHotKey(hwnd, HK_SHOW,  MOD_CONTROL | MOD_ALT, 'W');
        RegisterHotKey(hwnd, HK_RESET, MOD_CONTROL | MOD_ALT, 'R');

        SyncTickRate();
        return 0;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wp == 1) OnTick();
        return 0;

    case WM_HOTKEY:
        switch (wp) {
            case HK_SW:    ToggleStopwatch(); break;
            case HK_TMR:   ToggleTimer(); break;
            case HK_ADD:   AdjustTimer(60.0); break;
            case HK_SHOW:  ToggleWindow(); break;
            case HK_RESET: ResetAll(); break;
        }
        return 0;

    case WM_MOUSEMOVE: {
        int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (id != g_hover) {
            g_hover = id;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover != IDC_BLANK) {
            g_hover = IDC_BLANK;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int id = HitTest(x, y);
        if (id == IDC_BLANK) {
            if (y / g_scale < TOPBAR_H) {
                ReleaseCapture();
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        g_pressed = id;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_pressed != IDC_BLANK) {
            int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            int was = g_pressed;
            g_pressed = IDC_BLANK;
            ReleaseCapture();
            if (id == was) DoAction(was);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TRAY:
        if (lp == WM_LBUTTONUP)       ToggleWindow();
        else if (lp == WM_RBUTTONUP)  ShowTrayMenu(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        UnregisterHotKey(hwnd, HK_SW);
        UnregisterHotKey(hwnd, HK_TMR);
        UnregisterHotKey(hwnd, HK_ADD);
        UnregisterHotKey(hwnd, HK_SHOW);
        UnregisterHotKey(hwnd, HK_RESET);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // DPI
    typedef BOOL(WINAPI* SetDpiCtxFn)(DPI_AWARENESS_CONTEXT);
    auto setDpi = (SetDpiCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                              "SetProcessDpiAwarenessContext");
    if (setDpi) setDpi(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    else SetProcessDPIAware();

    typedef UINT(WINAPI* GetDpiFn)();
    auto getDpi = (GetDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                           "GetDpiForSystem");
    UINT dpi = getDpi ? getDpi() : 96;
    g_scale = (float)dpi / 96.0f;

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);

    g_icon = CreateClockIcon(GetSystemMetrics(SM_CXSMICON));
    if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ClockAppWnd";
    wc.hIcon         = g_icon;
    wc.hIconSm       = g_icon;
    RegisterClassExW(&wc);

    int w = (int)(WIN_W * g_scale + 0.5f);
    int h = (int)(WIN_H * g_scale + 0.5f);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(0, L"ClockAppWnd", L"Clock & Timers",
                                WS_POPUP, x, y, w, h,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon) DestroyIcon(g_icon);
    GdiplusShutdown(token);
    return 0;
}