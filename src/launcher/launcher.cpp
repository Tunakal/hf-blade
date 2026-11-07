// HF Blade launcher: edits hfblade.ini (next to this exe) and starts the game through Steam.
// Native Win32 dialog with a custom dark theme (GDI+ for anti-aliased shapes) and a "cut" intro.
// Optional key art is loaded at runtime from hfblade-launcher-art.jpg/.png next to the exe; no
// game art is embedded in the binary or the repository.
// Build: see build.bat. No game code or data is used here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"
#include "audio.h"
#include "../config.h"

using namespace Gdiplus;

static wchar_t g_dir[MAX_PATH];   // folder of this exe, with trailing backslash
static wchar_t g_ini[MAX_PATH];
static const wchar_t kGameExe[] = L"METAL GEAR RISING REVENGEANCE.exe";
static const wchar_t kSteamUrl[] = L"steam://rungameid/235460";

// ---------------------------------------------------------------------------
// Theme and layout
// ---------------------------------------------------------------------------
static const COLORREF kBg      = RGB(0x0E, 0x0F, 0x12);
static const COLORREF kCard    = RGB(0x18, 0x1A, 0x1F);
static const COLORREF kField   = RGB(0x22, 0x25, 0x2B);
static const COLORREF kFieldHi = RGB(0x2A, 0x2D, 0x34);
static const COLORREF kBorder  = RGB(0x2B, 0x2E, 0x35);
static const COLORREF kText    = RGB(0xE8, 0xE9, 0xEC);
static const COLORREF kMuted   = RGB(0x8B, 0x90, 0x9A);
static const COLORREF kAccent  = RGB(0x1C, 0x9C, 0xEC);   // MGR electric blue
static const COLORREF kAccentHi = RGB(0x5C, 0xCB, 0xFF);
static const COLORREF kAccentDn = RGB(0x12, 0x6E, 0xB0);   // pressed
static const COLORREF kDanger  = RGB(0xC4, 0x2B, 0x1C);   // close-button hover
static const COLORREF kWarn    = RGB(0xF0, 0xB4, 0x29);
static const COLORREF kOff     = RGB(0x3A, 0x3D, 0x45);

static Color C(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

static const int kArtW = 196;     // left column width (dialog units)
static const int kColX = 196;     // right column origin
struct Card { int tab, top, bottom; const wchar_t* title; };   // dialog units; x = kColX + 8 .. kColX + 260
static const Card kCards[] = {
    { 0, 22, 86, L"FRAME RATE" },
    { 0, 92, 146, L"FIXES" },
    { 0, 152, 200, L"AFTERMATH" },
    { 0, 206, 238, L"LAUNCHER" },
    { 1, 22, 138, L"RESOLUTION" },
    { 1, 144, 238, L"SHAPE" },
};
static int g_tab = 0;
static int g_resW = 1920, g_resH = 1200;   // selected render size (H = 0: game default)
static const int kTab0Ids[] = { IDC_L_FPS, IDC_FPS, IDC_L_MENU, IDC_MENUFPS, IDC_HFR, IDC_QTE, IDC_BLADE, IDC_L_EASE, IDC_EASE,
                                IDC_EASEVAL, IDC_L_PIECES, IDC_KEEPCUTS, IDC_L_BODIES, IDC_KEEPBODIES, IDC_L_INTRO, IDC_INTRO, IDC_PREVIEW };
static const int kTab1Ids[] = { IDC_L_RES, IDC_RES, IDC_VERTPLUS, IDC_RESINFO, IDC_RESNOTE };

static HBRUSH g_bgBrush, g_cardBrush, g_accentBrush;
static HFONT  g_titleFont, g_subFont, g_sectionFont, g_uiFont, g_btnFont;
static HWND   g_hot;          // owner-drawn control under the mouse
static Bitmap* g_art;         // optional key art
static bool    g_soundOn = true; // cut sound effects ([Launcher] Sound)

static RECT DluRect(HWND dlg, int l, int t, int r, int b)
{
    RECT rc = { l, t, r, b };
    MapDialogRect(dlg, &rc);
    return rc;
}

static void AddRoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    const REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static HFONT MakeFont(HWND dlg, int pt, int weight, const wchar_t* face)
{
    const int dpi = (int)GetDpiForWindow(dlg);
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void DrawTextIn(HDC dc, const wchar_t* s, RECT rc, HFONT font, COLORREF color, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s, -1, &rc, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void LoadArt()
{
    const wchar_t* names[] = { L"hfblade-launcher-art.jpg", L"hfblade-launcher-art.png", L"mgr-hfr-launcher-art.jpg", L"mgr-hfr-launcher-art.png" };
    for (const wchar_t* n : names) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s%s", g_dir, n);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        Bitmap* b = Bitmap::FromFile(path);
        if (b && b->GetLastStatus() == Ok) { g_art = b; return; }
        delete b;
    }
}

// Left column: key art (fit to width, top aligned, faded into the background) + title.
static void PaintArtColumn(HWND dlg, HDC dc, Graphics& g)
{
    RECT client;
    GetClientRect(dlg, &client);
    const RECT col = DluRect(dlg, 0, 0, kArtW, 0);
    const REAL w = REAL(col.right), h = REAL(client.bottom);
    REAL artBottom = h * 0.55f;
    if (g_art) {
        const REAL ih = w * REAL(g_art->GetHeight()) / REAL(g_art->GetWidth());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(g_art, RectF(0, 0, w, ih));
        artBottom = ih;
        // Fade the bottom of the art and its right edge into the background.
        const REAL f0 = ih * 0.62f;
        LinearGradientBrush fadeV(PointF(0, f0), PointF(0, ih + 1), C(kBg, 0), C(kBg, 255));
        g.FillRectangle(&fadeV, RectF(0, f0, w + 1, ih - f0 + 1));
        LinearGradientBrush fadeH(PointF(w * 0.80f, 0), PointF(w + 1, 0), C(kBg, 0), C(kBg, 255));
        g.FillRectangle(&fadeH, RectF(w * 0.80f, 0, w * 0.20f + 1, ih));
    } else {
        // No art file: red slashes.
        SolidBrush red(C(kAccent, 200)), dim(C(kAccent, 60));
        for (int i = 0; i < 4; ++i) {
            const REAL x = w * (0.05f + 0.22f * i), s = w * 0.08f;
            PointF p[4] = { PointF(x, artBottom), PointF(x + s, artBottom), PointF(x + s + w * 0.35f, 0), PointF(x + w * 0.35f, 0) };
            g.FillPolygon(i == 2 ? &red : &dim, p, 4);
        }
    }

    // Title block below (or over the faded end of) the art.
    REAL ty = artBottom - REAL(DluRect(dlg, 0, 0, 0, 14).bottom);
    const REAL maxTy = h - REAL(DluRect(dlg, 0, 0, 0, 62).bottom);
    if (ty > maxTy) ty = maxTy;
    const RECT bar = { DluRect(dlg, 10, 0, 12, 0).left, LONG(ty) + 4, DluRect(dlg, 10, 0, 12, 0).right, LONG(ty) + DluRect(dlg, 0, 0, 0, 30).bottom };
    FillRect(dc, &bar, g_accentBrush);
    RECT t = { DluRect(dlg, 17, 0, 0, 0).left, LONG(ty), col.right - 8, LONG(ty) + DluRect(dlg, 0, 0, 0, 18).bottom };
    DrawTextIn(dc, L"HF BLADE", t, g_titleFont, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT s = { t.left, t.bottom, t.right, t.bottom + DluRect(dlg, 0, 0, 0, 22).bottom };
    DrawTextIn(dc, L"Remaster-grade enhancements for\nMETAL GEAR RISING: REVENGEANCE", s, g_subFont, kMuted, DT_LEFT | DT_WORDBREAK);
}

// Display tab: the chosen screen shape next to the original 16:9 view, showing where the extra
// picture comes from (Vert+ above/below, Hor+ at the sides).
static void PaintShape(HWND dlg, HDC dc, Graphics& g)
{
    const RECT box = DluRect(dlg, kColX + 18, 160, kColX + 250, 222);
    const REAL bw = REAL(box.right - box.left), bh = REAL(box.bottom - box.top);
    const REAL aspect = g_resH ? REAL(g_resW) / REAL(g_resH) : 16.0f / 9.0f, base = 16.0f / 9.0f;
    REAL w = bw, h = bw / aspect;
    if (h > bh) { h = bh; w = bh * aspect; }
    const REAL x = box.left + (bw - w) / 2, y = box.top + (bh - h) / 2;
    // Original 16:9 view inside the screen: full width (taller screens) or full height (wider).
    REAL vw = w, vh = w / base;
    if (vh > h) { vh = h; vw = h * base; }
    const REAL vx = x + (w - vw) / 2, vy = y + (h - vh) / 2;
    SolidBrush extra(C(kAccent, 60)), view(C(kField));
    g.FillRectangle(&extra, x, y, w, h);
    g.FillRectangle(&view, vx, vy, vw, vh);
    Pen screen(C(kAccentHi), 1.5f), orig(C(kMuted), 1.0f);
    orig.SetDashStyle(DashStyleDash);
    g.DrawRectangle(&orig, vx, vy, vw, vh);
    g.DrawRectangle(&screen, x, y, w, h);
    const bool same = fabsf(aspect - base) < 0.01f;
    RECT t = { LONG(vx), LONG(vy), LONG(vx + vw), LONG(vy + vh) };
    DrawTextIn(dc, same ? L"16:9 \x2014 same shape as the original" : L"original 16:9 view", t, g_uiFont, kMuted,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT cap = DluRect(dlg, kColX + 18, 224, kColX + 250, 234);
    DrawTextIn(dc, same ? L"" : aspect < base ? L"Blue: extra view above and below (Vert+)" : L"Blue: extra view at the sides (Hor+)",
               cap, g_uiFont, kAccentHi, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// Whole window background: art column, top bar, section cards.
static void PaintBackground(HWND dlg, HDC dc)
{
    RECT client;
    GetClientRect(dlg, &client);
    FillRect(dc, &client, g_bgBrush);
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    PaintArtColumn(dlg, dc, g);

    SolidBrush fill(C(kCard));
    Pen border(C(kBorder), 1.0f);
    for (const Card& c : kCards) {
        if (c.tab != g_tab) continue;
        const RECT r = DluRect(dlg, kColX + 8, c.top, kColX + 260, c.bottom);
        GraphicsPath path;
        AddRoundRect(path, REAL(r.left) + 0.5f, REAL(r.top) + 0.5f, REAL(r.right - r.left - 1), REAL(r.bottom - r.top - 1), 6.0f);
        g.FillPath(&fill, &path);
        g.DrawPath(&border, &path);
        DrawTextIn(dc, c.title, DluRect(dlg, kColX + 18, c.top + 4, kColX + 200, c.top + 14), g_sectionFont, kAccentHi, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    if (g_tab == 1) PaintShape(dlg, dc, g);
    Pen frame(C(kBorder), 1.0f);
    g.DrawRectangle(&frame, 0.0f, 0.0f, REAL(client.right - 1), REAL(client.bottom - 1));
}

// ---------------------------------------------------------------------------
// Owner-drawn controls
// ---------------------------------------------------------------------------
static void DrawToggle(const DRAWITEMSTRUCT* di)
{
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillRect(dc, &rc, g_cardBrush);
    const bool on = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA) != 0;
    const bool hot = di->hwndItem == g_hot;
    const REAL h = REAL(rc.bottom - rc.top) * 0.78f, w = h * 1.9f;
    const REAL x = REAL(rc.left) + 1, y = REAL(rc.top) + (REAL(rc.bottom - rc.top) - h) / 2;
    {   // GDI+ must be finished before GDI text: inside WM_PRINT the DC origin is offset
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath pill;
        AddRoundRect(pill, x, y, w, h, h / 2);
        SolidBrush track(on ? C(hot ? kAccentHi : kAccent) : C(hot ? RGB(0x48, 0x4C, 0x55) : kOff));
        g.FillPath(&track, &pill);
        const REAL k = h - 4;
        SolidBrush knob(C(on ? RGB(255, 255, 255) : RGB(0xB8, 0xBC, 0xC4)));
        g.FillEllipse(&knob, on ? x + w - k - 2 : x + 2, y + 2, k, k);
        if (di->itemState & ODS_FOCUS) {
            Pen focus(C(kAccentHi, 160), 1.0f);
            GraphicsPath f;
            AddRoundRect(f, x - 1.5f, y - 1.5f, w + 3, h + 3, (h + 3) / 2);
            g.DrawPath(&focus, &f);
        }
    }
    wchar_t text[64];
    GetWindowTextW(di->hwndItem, text, 64);
    RECT tr = rc;
    tr.left = LONG(x + w + h * 0.6f);
    DrawTextIn(dc, text, tr, g_uiFont, on ? kText : kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

static void DrawButton(const DRAWITEMSTRUCT* di)
{
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillRect(dc, &rc, g_bgBrush);
    const bool hot = di->hwndItem == g_hot, down = (di->itemState & ODS_SELECTED) != 0;
    Graphics* gp = new Graphics(dc);
    Graphics& g = *gp;
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    if (di->CtlID == IDC_TAB0 || di->CtlID == IDC_TAB1) {   // tabs: active one has an accent underline
        const bool active = int(di->CtlID - IDC_TAB0) == g_tab;
        if (active) {
            SolidBrush u(C(kAccent));
            g.FillRectangle(&u, REAL(rc.left) + 4, REAL(rc.bottom) - 3, REAL(rc.right - rc.left) - 8, 2.0f);
        }
        delete gp;
        wchar_t text[32];
        GetWindowTextW(di->hwndItem, text, 32);
        DrawTextIn(dc, text, rc, g_sectionFont, active || hot ? kText : kMuted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }
    if (di->CtlID == IDC_MUTE) {   // speaker: waves when on, a cross when muted
        if (hot) {
            SolidBrush b(C(kFieldHi));
            g.FillRectangle(&b, REAL(rc.left), REAL(rc.top), REAL(rc.right - rc.left), REAL(rc.bottom - rc.top));
        }
        const REAL s = REAL(rc.bottom - rc.top) * 0.12f, cx = REAL(rc.left + rc.right) / 2 - s * 0.6f, cy = REAL(rc.top + rc.bottom) / 2;
        SolidBrush body(C(g_soundOn ? (hot ? kAccentHi : kText) : kMuted));
        PointF sp[6] = { PointF(cx - 2 * s, cy - s), PointF(cx - s, cy - s), PointF(cx + s * 0.6f, cy - 2.4f * s),
                         PointF(cx + s * 0.6f, cy + 2.4f * s), PointF(cx - s, cy + s), PointF(cx - 2 * s, cy + s) };
        g.FillPolygon(&body, sp, 6);
        Pen p(C(g_soundOn ? (hot ? kAccentHi : kText) : kMuted), 1.3f);
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        if (g_soundOn) {
            g.DrawArc(&p, cx + s * 0.2f, cy - 1.6f * s, 2.4f * s, 3.2f * s, -50, 100);
            g.DrawArc(&p, cx - s * 0.6f, cy - 2.8f * s, 4.4f * s, 5.6f * s, -50, 100);
        } else {
            g.DrawLine(&p, cx + 1.6f * s, cy - s, cx + 3.4f * s, cy + s);
            g.DrawLine(&p, cx + 1.6f * s, cy + s, cx + 3.4f * s, cy - s);
        }
        delete gp;
        return;
    }
    if (di->CtlID == IDC_XMIN || di->CtlID == IDC_XCLOSE) {   // caption buttons
        if (hot) {
            SolidBrush b(C(di->CtlID == IDC_XCLOSE ? kDanger : kFieldHi));
            g.FillRectangle(&b, REAL(rc.left), REAL(rc.top), REAL(rc.right - rc.left), REAL(rc.bottom - rc.top));
        }
        Pen p(C(hot ? RGB(255, 255, 255) : kMuted), 1.2f);
        const REAL cx = REAL(rc.left + rc.right) / 2, cy = REAL(rc.top + rc.bottom) / 2, s = REAL(rc.bottom - rc.top) * 0.18f;
        if (di->CtlID == IDC_XMIN) g.DrawLine(&p, cx - s, cy, cx + s, cy);
        else { g.DrawLine(&p, cx - s, cy - s, cx + s, cy + s); g.DrawLine(&p, cx - s, cy + s, cx + s, cy - s); }
        delete gp;
        return;
    }

    const bool primary = di->CtlID == IDC_PLAY, preview = di->CtlID == IDC_PREVIEW;
    if (preview) FillRect(dc, &rc, g_cardBrush);
    {
        GraphicsPath p;
        AddRoundRect(p, REAL(rc.left) + 0.5f, REAL(rc.top) + 0.5f, REAL(rc.right - rc.left - 1), REAL(rc.bottom - rc.top - 1), 5.0f);
        COLORREF fill = primary ? (down ? kAccentDn : hot ? kAccent : RGB(0x17, 0x86, 0xD6))
                                : (down ? RGB(0x1E, 0x20, 0x25) : hot ? kFieldHi : kField);
        SolidBrush b(C(fill));
        g.FillPath(&b, &p);
        if (!primary) { Pen border(C(hot ? kAccent : kBorder), 1.0f); g.DrawPath(&border, &p); }
        if (di->itemState & ODS_FOCUS) { Pen focus(C(RGB(255, 255, 255), 90), 1.0f); g.DrawPath(&focus, &p); }
        g.Flush(FlushIntentionSync);
    }
    delete gp;   // GDI+ must be finished before GDI text (WM_PRINT DCs have an offset origin)
    wchar_t text[64];
    GetWindowTextW(di->hwndItem, text, 64);
    DrawTextIn(dc, primary ? L"\x25B6  SAVE & PLAY" : preview ? L"\x25B6" : text, rc, g_btnFont,
               primary ? RGB(255, 255, 255) : preview ? kAccentHi : kText,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// Combo box: the closed field is painted by ComboProc; list items are drawn here.
static void DrawComboItem(const DRAWITEMSTRUCT* di)
{
    if (di->itemState & ODS_COMBOBOXEDIT) { InvalidateRect(di->hwndItem, nullptr, FALSE); return; }
    if ((int)di->itemID < 0) return;
    const bool sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(sel ? kAccent : kField);
    FillRect(di->hDC, &di->rcItem, b);
    DeleteObject(b);
    wchar_t text[64];
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    RECT r = di->rcItem;
    r.left += MulDiv(10, (int)GetDpiForWindow(di->hwndItem), 96);
    DrawTextIn(di->hDC, text, r, g_uiFont, sel ? RGB(255, 255, 255) : kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

static void PaintCombo(HWND h, HDC dc)
{
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, g_cardBrush);
    const bool hot = h == g_hot, open = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0, focus = GetFocus() == h;
    const int dpi = (int)GetDpiForWindow(h);
    {   // GDI+ first, GDI text after (see DrawToggle)
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath p;
        AddRoundRect(p, REAL(rc.left) + 0.5f, REAL(rc.top) + 0.5f, REAL(rc.right - rc.left - 1), REAL(rc.bottom - rc.top - 1), 5.0f);
        SolidBrush fill(C(hot || open ? kFieldHi : kField));
        g.FillPath(&fill, &p);
        Pen border(C(open || focus ? kAccent : hot ? RGB(0x4A, 0x4E, 0x57) : kBorder), 1.0f);
        g.DrawPath(&border, &p);
        const REAL cx = REAL(rc.right) - MulDiv(12, dpi, 96), cy = REAL(rc.bottom) / 2, s = REAL(MulDiv(4, dpi, 96));
        Pen chev(C(open ? kAccentHi : kMuted), 1.6f);
        chev.SetStartCap(LineCapRound); chev.SetEndCap(LineCapRound);
        const REAL dir = open ? -1.0f : 1.0f;
        g.DrawLine(&chev, cx - s, cy - s * 0.5f * dir, cx, cy + s * 0.5f * dir);
        g.DrawLine(&chev, cx, cy + s * 0.5f * dir, cx + s, cy - s * 0.5f * dir);
    }

    const int i = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (i >= 0) {
        wchar_t text[64];
        SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)text);
        RECT t = rc;
        t.left += MulDiv(10, dpi, 96);
        t.right -= MulDiv(24, dpi, 96);
        DrawTextIn(dc, text, t, g_uiFont, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

static void SetHot(HWND h)
{
    if (g_hot == h) return;
    HWND prev = g_hot;
    g_hot = h;
    if (prev) InvalidateRect(prev, nullptr, FALSE);
    if (h) {
        InvalidateRect(h, nullptr, FALSE);
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
    }
}

static LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintCombo(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintCombo(h, (HDC)wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        SetHot(h);
        break;
    case WM_MOUSELEAVE:
        if (g_hot == h) SetHot(nullptr);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Trackbar: thin track with an accent fill up to a round thumb.
static LRESULT CustomDrawSlider(NMCUSTOMDRAW* cd)
{
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    Graphics g(cd->hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    RECT thumb;
    SendMessageW(cd->hdr.hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    RECT client;
    GetClientRect(cd->hdr.hwndFrom, &client);
    const REAL cy = REAL(client.bottom) / 2, tx = REAL(thumb.left + thumb.right) / 2;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        FillRect(cd->hdc, &client, g_cardBrush);
        const REAL x0 = REAL(cd->rc.left), x1 = REAL(cd->rc.right);
        Pen off(C(kOff), 4.0f), on(C(kAccent), 4.0f);
        off.SetStartCap(LineCapRound); off.SetEndCap(LineCapRound);
        on.SetStartCap(LineCapRound); on.SetEndCap(LineCapRound);
        g.DrawLine(&off, x0 + 2, cy, x1 - 2, cy);
        g.DrawLine(&on, x0 + 2, cy, tx, cy);
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        const REAL r = REAL(client.bottom) * 0.30f;
        SolidBrush ring(C(kAccent)), dot(C(RGB(255, 255, 255)));
        g.FillEllipse(&ring, tx - r, cy - r, 2 * r, 2 * r);
        g.FillEllipse(&dot, tx - r * 0.45f, cy - r * 0.45f, r * 0.9f, r * 0.9f);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_SKIPDEFAULT;   // no tics
}

// Hover tracking for owner-drawn toggles and buttons.
static LRESULT CALLBACK HoverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_MOUSEMOVE) SetHot(h);
    else if (msg == WM_MOUSELEAVE && g_hot == h) SetHot(nullptr);
    else if (msg == WM_LBUTTONDBLCLK) msg = WM_LBUTTONDOWN;   // fast toggling is not a double click
    return DefSubclassProc(h, msg, wp, lp);
}

// Dark scrollbars in the combo drop-downs, rounded corners and a dark border for the window.
static void ApplyWindowTheme(HWND dlg)
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkFn = BOOL(WINAPI*)(HWND, BOOL);
    auto setMode = ux ? reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135))) : nullptr;
    auto allow = ux ? reinterpret_cast<AllowDarkFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133))) : nullptr;
    if (setMode) setMode(2);   // ForceDark (undocumented, used widely for dark scrollbars)
    const int combos[] = { IDC_FPS, IDC_MENUFPS, IDC_KEEPCUTS, IDC_KEEPBODIES, IDC_INTRO, IDC_RES };
    for (int id : combos) {
        HWND c = GetDlgItem(dlg, id);
        SetWindowSubclass(c, ComboProc, 1, 0);
        COMBOBOXINFO ci = { sizeof ci };
        if (GetComboBoxInfo(c, &ci) && ci.hwndList) {
            if (allow) allow(ci.hwndList, TRUE);
            SetWindowTheme(ci.hwndList, L"DarkMode_Explorer", nullptr);
        }
    }
    const DWORD round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(dlg, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    const COLORREF border = kBorder;
    DwmSetWindowAttribute(dlg, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
}

static bool IsToggle(int id) { return id == IDC_HFR || id == IDC_QTE || id == IDC_BLADE || id == IDC_VERTPLUS; }
static bool GetToggle(HWND dlg, int id) { return GetWindowLongPtrW(GetDlgItem(dlg, id), GWLP_USERDATA) != 0; }
static void SetToggle(HWND dlg, int id, bool on)
{
    HWND h = GetDlgItem(dlg, id);
    SetWindowLongPtrW(h, GWLP_USERDATA, on ? 1 : 0);
    InvalidateRect(h, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Intro, rendered on a layered overlay from a snapshot of the (still transparent) dialog.
//  1 = classic: one slash crosses the window and its two halves slide together along the cut.
//  2 = flurry:  a rapid series of slashes appears and stays, then one big slash opens the window.
// ---------------------------------------------------------------------------
struct Intro {
    HWND dlg = nullptr, overlay = nullptr;
    Bitmap* snap = nullptr;
    int w = 0, h = 0, margin = 0, mode = 2;
    DWORD start = 0, durMs = 0;
    int nextCut = 0;        // next flurry cut whose sound has not played yet
    bool bigPlayed = false;
};

static unsigned g_soundSeed = 1;

static REAL SoundRand() { g_soundSeed = g_soundSeed * 1664525u + 1013904223u; return REAL((g_soundSeed >> 8) & 0xFFFF) / 65535.0f; }
static REAL Lerp(REAL a, REAL b, REAL t) { return a + (b - a) * t; }
static Intro g_intro;
static DWORD IntroDuration(int mode) { return mode == 2 ? 1750 : 650; }

static REAL Clamp01(REAL v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static REAL EaseOut(REAL t) { const REAL u = 1 - t; return 1 - u * u * u; }

struct Rng {
    unsigned s;
    REAL Next() { s = s * 1664525u + 1013904223u; return REAL((s >> 8) & 0xFFFF) / 65535.0f; }
};

// Glowing blade trail from A towards B; grow = 0..1 of the length, a = opacity, scale = thickness.
static void DrawSlash(Graphics& g, PointF A, PointF B, REAL grow, REAL a, REAL scale)
{
    if (a <= 0 || grow <= 0) return;
    const PointF E(A.X + (B.X - A.X) * grow, A.Y + (B.Y - A.Y) * grow);
    struct { REAL width; COLORREF c; BYTE a; } layers[] = {
        { 22, kAccent, 45 }, { 10, RGB(120, 210, 255), 110 }, { 4, RGB(225, 245, 255), 230 }, { 1.6f, RGB(255, 255, 255), 255 } };
    for (auto& l : layers) {
        Pen p(C(l.c, BYTE(l.a * Clamp01(a))), l.width * scale);
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        g.DrawLine(&p, A, E);
    }
}

// Sparks thrown off a slash; age in seconds since the slash was born.
static void DrawSparks(Graphics& g, PointF A, PointF B, REAL age, unsigned seed, int count, REAL reach)
{
    REAL dx = B.X - A.X, dy = B.Y - A.Y;
    const REAL len = sqrtf(dx * dx + dy * dy);
    dx /= len; dy /= len;
    const REAL nx = -dy, ny = dx;
    Rng r = { seed };
    for (int i = 0; i < count; ++i) {
        const REAL pos = r.Next(), born = pos * 0.12f, life = 0.22f + r.Next() * 0.30f;
        const REAL side = r.Next() < 0.5f ? -1.0f : 1.0f, speed = (0.35f + r.Next()) * reach, along = (r.Next() - 0.3f) * reach * 0.7f;
        const bool ember = r.Next() < 0.3f;   // a few orange embers among the electric-blue sparks
        const REAL k = (age - born) / life;
        if (k <= 0 || k >= 1) continue;
        const REAL px = A.X + (B.X - A.X) * pos + (nx * side * speed + dx * along) * k;
        const REAL py = A.Y + (B.Y - A.Y) * pos + (ny * side * speed + dy * along) * k + reach * 0.45f * k * k;
        const REAL rad = (1 - k) * 2.6f + 0.6f;
        const BYTE al = BYTE(255 * (1 - k));
        SolidBrush s(ember ? Color(al, 255, BYTE(190 - 120 * k), BYTE(80 - 60 * k))
                           : Color(al, BYTE(200 - 80 * k), BYTE(240 - 60 * k), 255));
        g.FillEllipse(&s, px - rad, py - rad, 2 * rad, 2 * rad);
    }
}

// Everything the intro draws stays inside the launcher's own (rounded) rectangle.
static void SetWindowClip(Graphics& g)
{
    const Intro& in = g_intro;
    GraphicsPath p;
    AddRoundRect(p, REAL(in.margin), REAL(in.margin), REAL(in.w), REAL(in.h), REAL(in.w) / 110.0f);
    g.SetClip(&p);
}

// Draws `img` (window-sized) split along A-B; the two halves are offset by +-off along the normal
// and +-slide along the cut, with opacity a. Clipped to the window.
static void DrawSplit(Graphics& g, Bitmap* img, PointF A, PointF B, REAL slide, REAL off, REAL a)
{
    if (a <= 0) return;
    const Intro& in = g_intro;
    const REAL M = REAL(in.margin), W = REAL(in.w), H = REAL(in.h), big = 8000;
    REAL dx = B.X - A.X, dy = B.Y - A.Y;
    const REAL len = sqrtf(dx * dx + dy * dy);
    dx /= len; dy /= len;
    const REAL nx = -dy, ny = dx;   // points to the bottom-right half
    ColorMatrix cm = { 1, 0, 0, 0, 0,  0, 1, 0, 0, 0,  0, 0, 1, 0, 0,  0, 0, 0, Clamp01(a), 0,  0, 0, 0, 0, 1 };
    ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    for (int side = 0; side < 2; ++side) {
        const REAL sgn = side ? 1.0f : -1.0f;
        const REAL ox = sgn * (dx * slide + nx * off), oy = sgn * (dy * slide + ny * off);
        // Half-plane of this side, moved together with its image.
        PointF poly[4] = { PointF(A.X + ox, A.Y + oy), PointF(B.X + ox, B.Y + oy),
                           PointF(B.X + ox + sgn * nx * big, B.Y + oy + sgn * ny * big),
                           PointF(A.X + ox + sgn * nx * big, A.Y + oy + sgn * ny * big) };
        GraphicsPath half;
        half.AddPolygon(poly, 4);
        Region halfRgn(&half);
        SetWindowClip(g);
        g.IntersectClip(&halfRgn);
        g.DrawImage(img, RectF(M + ox, M + oy, W, H), 0, 0, W, H, UnitPixel, &ia);
    }
    SetWindowClip(g);
}

// Fiery trail (orange/yellow, like the key art) for the big opening cut.
static void DrawFireSlash(Graphics& g, PointF A, PointF B, REAL grow, REAL a, REAL scale)
{
    if (a <= 0 || grow <= 0) return;
    const PointF E(A.X + (B.X - A.X) * grow, A.Y + (B.Y - A.Y) * grow);
    struct { REAL width; Color c; } layers[] = {
        { 46, Color(BYTE(40 * a), 255, 110, 20) }, { 26, Color(BYTE(90 * a), 255, 150, 40) },
        { 12, Color(BYTE(170 * a), 255, 205, 90) }, { 5, Color(BYTE(235 * a), 255, 245, 200) },
        { 2, Color(BYTE(255 * a), 255, 255, 255) } };
    for (auto& l : layers) {
        Pen p(l.c, l.width * scale);
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        g.DrawLine(&p, A, E);
    }
}

// Embers: orange/yellow particles that burst out of the big cut, fall and fade.
static void DrawEmbers(Graphics& g, PointF A, PointF B, REAL age, unsigned seed, int count, REAL reach)
{
    REAL dx = B.X - A.X, dy = B.Y - A.Y;
    const REAL len = sqrtf(dx * dx + dy * dy);
    dx /= len; dy /= len;
    const REAL nx = -dy, ny = dx;
    Rng r = { seed };
    for (int i = 0; i < count; ++i) {
        const REAL pos = r.Next(), born = pos * 0.10f, life = 0.35f + r.Next() * 0.45f;
        const REAL side = r.Next() < 0.5f ? -1.0f : 1.0f, speed = (0.25f + r.Next()) * reach;
        const REAL along = (r.Next() - 0.2f) * reach * 0.9f, size = 1.0f + r.Next() * 2.4f;
        const bool streak = r.Next() < 0.45f;
        const REAL k = (age - born) / life;
        if (k <= 0 || k >= 1) continue;
        const REAL e = EaseOut(k);
        const REAL vx = nx * side * speed + dx * along, vy = ny * side * speed + dy * along;
        const REAL px = A.X + (B.X - A.X) * pos + vx * e;
        const REAL py = A.Y + (B.Y - A.Y) * pos + vy * e + reach * 0.35f * k * k;
        const BYTE al = BYTE(255 * (1 - k));
        const Color c(al, 255, BYTE(230 - 120 * k), BYTE(120 - 100 * k));
        if (streak) {   // short motion streak in the direction of travel
            Pen p(c, size * 0.8f);
            p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
            g.DrawLine(&p, px, py, px - vx * 0.06f, py - vy * 0.06f);
        } else {
            SolidBrush s(c);
            const REAL rad = size * (1 - k * 0.6f);
            g.FillEllipse(&s, px - rad, py - rad, 2 * rad, 2 * rad);
        }
    }
}

// The flurry's small cuts: deterministic, so the renderer and the sound scheduler agree.
// Each cut is a full line across the window (window-local coordinates, clipped to its edges).
static const int kFlurryCuts = 12;
static const REAL kFlurryBigAt = 820;   // ms
struct FlurryCut { REAL born, scale, pan; PointF a, b; };

static FlurryCut GetFlurryCut(int i, REAL W, REAL H)
{
    Rng r = { 424242u + 7919u * unsigned(i) };
    FlurryCut fc;
    fc.born = 40 + i * 58 + r.Next() * 20;
    const REAL cx = W * (0.15f + 0.70f * r.Next()), cy = H * (0.15f + 0.70f * r.Next());
    const REAL ang = (0.25f + 0.9f * r.Next()) * (i % 2 ? 1.0f : -1.0f) + (r.Next() < 0.2f ? 1.5708f : 0.0f);
    fc.scale = 0.45f + 0.35f * r.Next();
    fc.pan = cx / W * 2 - 1;
    // Clip the infinite line through (cx, cy) to the window (Liang-Barsky), with a small overshoot.
    const REAL dx = cosf(ang), dy = sinf(ang), pad = 4;
    REAL t0 = -1e6f, t1 = 1e6f;
    const REAL p[4] = { -dx, dx, -dy, dy }, q[4] = { cx + pad, W + pad - cx, cy + pad, H + pad - cy };
    for (int k = 0; k < 4; ++k) {
        if (fabsf(p[k]) < 1e-6f) continue;
        const REAL t = q[k] / p[k];
        if (p[k] < 0) { if (t > t0) t0 = t; } else { if (t < t1) t1 = t; }
    }
    fc.a = PointF(cx + dx * t0, cy + dy * t0);
    fc.b = PointF(cx + dx * t1, cy + dy * t1);
    if (r.Next() < 0.5f) { const PointF tmp = fc.a; fc.a = fc.b; fc.b = tmp; }   // random cut direction
    return fc;
}

static void RenderIntroFrame(Graphics& g, REAL ms)
{
    const Intro& in = g_intro;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.ResetClip();
    g.Clear(Color(0, 0, 0, 0));
    const REAL M = REAL(in.margin), W = REAL(in.w), H = REAL(in.h);
    const PointF A(M - W * 0.08f, M + H * 0.80f), B(M + W * 1.08f, M + H * 0.20f);   // the opening cut
    SetWindowClip(g);

    if (in.mode != 2) {   // classic: the window halves slide together along one cut
        const REAL t = ms / 650.0f, u = EaseOut(Clamp01((t - 0.12f) / 0.55f));
        DrawSplit(g, in.snap, A, B, (1 - u) * W * 0.18f, 0, u);
        DrawSlash(g, A, B, EaseOut(Clamp01(t / 0.20f)), 1 - Clamp01((t - 0.50f) / 0.40f), 1.0f);
        DrawSparks(g, A, B, ms / 1000.0f, 1234567u, 26, H * 0.55f);
        return;
    }

    // Flurry. A dark cover takes the cuts; then one big fiery cut splits the cover open and its
    // halves fly apart, revealing the launcher underneath.
    const REAL bigAt = kFlurryBigAt, age = ms - bigAt;
    const REAL shake = age > 60 && age < 260 ? (1 - (age - 60) / 200) * W * 0.008f : 0;
    if (shake > 0) g.TranslateTransform(sinf(age * 0.9f) * shake, cosf(age * 1.3f) * shake);

    // The cover with its cuts, rendered off-screen so both halves carry the cuts away.
    Bitmap cover(in.w, in.h, PixelFormat32bppPARGB);
    {
        Graphics c(&cover);
        c.SetSmoothingMode(SmoothingModeAntiAlias);
        c.Clear(C(kBg, BYTE(255 * Clamp01(ms / 90))));
        Pen frame(C(kBorder), 1.0f);
        c.DrawRectangle(&frame, 0.5f, 0.5f, W - 1, H - 1);
        for (int i = 0; i < kFlurryCuts; ++i) {
            const FlurryCut fc = GetFlurryCut(i, W, H);
            const REAL sAge = ms - fc.born;
            if (sAge <= 0) continue;
            const REAL flash = 1 - Clamp01((sAge - 70) / 160) * 0.35f;   // bright at the cut, then settles
            DrawSlash(c, fc.a, fc.b, EaseOut(Clamp01(sAge / 80)), flash, fc.scale);
            DrawSparks(c, fc.a, fc.b, sAge / 1000.0f, 1000u + i, 9, H * 0.30f);
        }
    }

    if (age <= 90) {   // before the big cut lands: just the cover
        g.DrawImage(&cover, M, M, W, H);
    } else {
        // The launcher is underneath; the cover halves split along the big cut and fly apart.
        g.DrawImage(in.snap, M, M, W, H);
        const REAL open = Clamp01((age - 90) / 520);
        const REAL e = open * open * (3 - 2 * open);   // smoothstep
        DrawSplit(g, &cover, A, B, e * W * 0.10f, e * H * 0.75f, 1 - Clamp01((age - 300) / 330));
    }
    if (age > 0) {
        DrawFireSlash(g, A, B, EaseOut(Clamp01(age / 110)), 1 - Clamp01((age - 330) / 420), 1.0f);
        DrawEmbers(g, A, B, age / 1000.0f, 7654321u, 70, H * 0.65f);
        DrawSparks(g, A, B, age / 1000.0f, 99u, 30, H * 0.55f);
        const REAL flash = age > 80 ? 1 - Clamp01((age - 80) / 220) : 0;   // impact: glow band + quick white pop
        if (flash > 0) {
            for (int i = 0; i < 8; ++i) {   // soft glow: stacked translucent bands, brightest at the cut
                const REAL f = REAL(i) / 7;
                Pen band(Color(BYTE(16 * flash), 255, BYTE(140 + 90 * f), BYTE(40 + 110 * f)), H * (0.50f - 0.46f * f) * (0.7f + 0.3f * flash));
                g.DrawLine(&band, A, B);
            }
            const REAL pop = 1 - Clamp01((age - 80) / 90);
            if (pop > 0) {
                SolidBrush f(Color(BYTE(150 * pop * pop), 255, 250, 235));
                g.FillRectangle(&f, M, M, W, H);
            }
        }
    }
    g.ResetTransform();
}

// Plays the cut sounds that are due at `ms` (randomised pitch, volume and sample per cut).
static void IntroSounds(REAL ms)
{
    if (!g_soundOn || Audio_SampleCount() == 0) return;
    const int n = Audio_SampleCount();
    if (g_intro.mode != 2) {
        if (!g_intro.bigPlayed) {
            g_intro.bigPlayed = true;
            Audio_Play(int(SoundRand() * n), Lerp(0.92f, 1.06f, SoundRand()), Lerp(0.75f, 0.9f, SoundRand()), 0);
        }
        return;
    }
    while (g_intro.nextCut < kFlurryCuts) {
        const FlurryCut fc = GetFlurryCut(g_intro.nextCut, REAL(g_intro.w), REAL(g_intro.h));
        if (fc.born > ms) break;
        ++g_intro.nextCut;
        Audio_Play(int(SoundRand() * n), Lerp(0.88f, 1.22f, SoundRand()), Lerp(0.35f, 0.65f, SoundRand()), fc.pan * 0.7f);
    }
    if (!g_intro.bigPlayed && ms >= kFlurryBigAt) {   // the big cut: low and heavy, layered
        g_intro.bigPlayed = true;
        const int first = int(SoundRand() * n);
        Audio_Play(first, Lerp(0.66f, 0.74f, SoundRand()), 1.0f, 0);
        Audio_Play(first + 1, Lerp(0.82f, 0.92f, SoundRand()), 0.75f, Lerp(-0.2f, 0.2f, SoundRand()));
    }
}

static void PresentIntroFrame(REAL ms)
{
    IntroSounds(ms);
    const int ow = g_intro.w + 2 * g_intro.margin, oh = g_intro.h + 2 * g_intro.margin;
    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), ow, -oh, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap target(ow, oh, ow * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&target);
        RenderIntroFrame(g, ms);
    }
    RECT wr;
    GetWindowRect(g_intro.dlg, &wr);
    POINT dst = { wr.left - g_intro.margin, wr.top - g_intro.margin }, src = { 0, 0 };
    SIZE size = { ow, oh };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_intro.overlay, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void EndIntro()
{
    KillTimer(g_intro.dlg, 2);
    SetLayeredWindowAttributes(g_intro.dlg, 0, 255, LWA_ALPHA);
    SetWindowLongPtrW(g_intro.dlg, GWL_EXSTYLE, GetWindowLongPtrW(g_intro.dlg, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    RedrawWindow(g_intro.dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (g_intro.overlay) DestroyWindow(g_intro.overlay);
    g_intro.overlay = nullptr;
    delete g_intro.snap;
    g_intro.snap = nullptr;
}

static bool SaveFramePng(REAL ms, const wchar_t* path)   // --dump-intro (development aid)
{
    const int ow = g_intro.w + 2 * g_intro.margin, oh = g_intro.h + 2 * g_intro.margin;
    Bitmap frame(ow, oh, PixelFormat32bppPARGB);
    {
        Graphics g(&frame);
        RenderIntroFrame(g, ms);
    }
    Bitmap flat(ow, oh, PixelFormat32bppRGB);
    {
        Graphics g(&flat);
        g.Clear(Color(255, 40, 44, 52));
        g.DrawImage(&frame, 0, 0, ow, oh);
    }
    CLSID png;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    return flat.Save(path, &png, nullptr) == Ok;
}

// Hides the dialog and plays the intro; mode 0 just shows the window.
static void StartIntro(HWND dlg, int mode, bool dump)
{
    if (g_intro.overlay) return;   // already playing
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    SetWindowLongPtrW(dlg, GWL_EXSTYLE, GetWindowLongPtrW(dlg, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(dlg, 0, 0, LWA_ALPHA);
    RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RECT wr;
    GetWindowRect(dlg, &wr);
    g_intro.dlg = dlg;
    g_intro.mode = mode;
    g_intro.durMs = IntroDuration(mode);
    g_intro.w = wr.right - wr.left;
    g_intro.h = wr.bottom - wr.top;
    g_intro.margin = g_intro.w / 6;
    g_intro.nextCut = 0;
    g_intro.bigPlayed = false;
    g_soundSeed = GetTickCount() | 1;

    // Snapshot of the finished dialog (still transparent on screen): let the dialog and its
    // children draw themselves into a memory DC.
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, g_intro.w, g_intro.h);
    HGDIOBJ old = SelectObject(mem, bmp);
    SendMessageW(dlg, WM_PRINT, (WPARAM)mem, PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND | PRF_NONCLIENT);
    SelectObject(mem, old);
    g_intro.snap = Bitmap::FromHBITMAP(bmp, nullptr);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    if (dump && mode != 0) {
        for (int i = 0; i < 8; ++i) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%sintro%d_%d.png", g_dir, mode, i);
            SaveFramePng(REAL(g_intro.durMs) * REAL(i + 1) / 9.0f, path);
        }
    }
    if (mode == 0 || !anim || !g_intro.snap) { EndIntro(); return; }

    g_intro.overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                      L"Static", L"", WS_POPUP, 0, 0, 0, 0, dlg, nullptr, nullptr, nullptr);
    PresentIntroFrame(0);
    ShowWindow(g_intro.overlay, SW_SHOWNOACTIVATE);
    g_intro.start = GetTickCount();
    SetTimer(dlg, 2, 10, nullptr);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static int ReadInt(const wchar_t* sec, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, g_ini);
}

static void WriteInt(const wchar_t* sec, const wchar_t* key, int v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", v);
    WritePrivateProfileStringW(sec, key, buf, g_ini);
}

static bool FileExists(const wchar_t* path)
{
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void InDir(wchar_t* out, size_t n, const wchar_t* name) { swprintf_s(out, n, L"%s%s", g_dir, name); }

// Writes every key with its default; used when the ini does not exist yet.
static void WriteDefaults()
{
    WriteInt(L"General", L"Enabled", 1);
    WriteInt(L"General", L"TargetFPS", cfg::kTargetFps);
    WriteInt(L"General", L"MenuFPS", cfg::kMenuFps);
    WriteInt(L"General", L"ToggleKey", cfg::kToggleKey);
    WriteInt(L"General", L"PresetKey", cfg::kPresetKey);
    WriteInt(L"General", L"LogStats", cfg::kLogStats);
    WriteInt(L"General", L"LogInput", cfg::kLogInput);
    WriteInt(L"General", L"HighFrameRate", cfg::kHighFrameRate);
    WriteInt(L"Fixes", L"QteFix", cfg::kQteFix);
    WriteInt(L"Fixes", L"QteEase", cfg::kQteEase);
    WriteInt(L"Fixes", L"BladeModeFix", cfg::kBladeModeFix);
    WriteInt(L"Display", L"Width", cfg::kWidth);
    WriteInt(L"Display", L"Height", cfg::kHeight);
    WriteInt(L"Display", L"VertPlusFov", cfg::kVertPlusFov);
    WriteInt(L"Debris", L"KeepLastCuts", cfg::kKeepLastCuts);
    WriteInt(L"Debris", L"KeepCorpses", cfg::kKeepCorpses);
    WriteInt(L"Debris", L"LogDebris", cfg::kLogDebris);
    WriteInt(L"Launcher", L"Intro", cfg::kIntro);
    WriteInt(L"Launcher", L"Sound", cfg::kSound);
}

static int AddItem(HWND combo, const wchar_t* text, int value)
{
    const int i = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(combo, CB_SETITEMDATA, i, value);
    return i;
}

// Selects the item whose data is `value`; adds "Custom (N)" if no preset matches.
static void SelectValue(HWND combo, int value)
{
    const int n = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i)
        if ((int)SendMessageW(combo, CB_GETITEMDATA, i, 0) == value) { SendMessageW(combo, CB_SETCURSEL, i, 0); return; }
    wchar_t buf[96];
    swprintf_s(buf, L"Custom (%d)", value);
    SendMessageW(combo, CB_SETCURSEL, AddItem(combo, buf, value), 0);
}

static int SelectedValue(HWND combo)
{
    const int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return i < 0 ? -1 : (int)SendMessageW(combo, CB_GETITEMDATA, i, 0);
}

static void UpdateEaseLabel(HWND dlg)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d%%", (int)SendDlgItemMessageW(dlg, IDC_EASE, TBM_GETPOS, 0, 0));
    SetDlgItemTextW(dlg, IDC_EASEVAL, buf);
}

static void Load(HWND dlg)
{
    HWND fps = GetDlgItem(dlg, IDC_FPS), menu = GetDlgItem(dlg, IDC_MENUFPS);
    wchar_t buf[96];
    for (int p : cfg::kPresets) { swprintf_s(buf, L"%d fps", p); AddItem(fps, buf, p); }
    AddItem(menu, L"Same as gameplay", 0);
    AddItem(menu, L"30 fps (original)", 30);
    for (int p : cfg::kPresets) { swprintf_s(buf, L"%d fps", p); AddItem(menu, buf, p); }

    HWND keep = GetDlgItem(dlg, IDC_KEEPCUTS);
    AddItem(keep, L"Off", 0);
    for (int n = 1; n <= 4; ++n) { swprintf_s(buf, n == 1 ? L"%d cut" : L"%d cuts", n); AddItem(keep, buf, n); }
    SelectValue(keep, ReadInt(L"Debris", L"KeepLastCuts", cfg::kKeepLastCuts));
    HWND bodies = GetDlgItem(dlg, IDC_KEEPBODIES);
    AddItem(bodies, L"Off", 0);
    for (int n = 1; n <= 8; ++n) { swprintf_s(buf, n == 1 ? L"%d kill" : L"%d kills", n); AddItem(bodies, buf, n); }
    SelectValue(bodies, ReadInt(L"Debris", L"KeepCorpses", cfg::kKeepCorpses));
    HWND intro = GetDlgItem(dlg, IDC_INTRO);
    AddItem(intro, L"Flurry of cuts", 2);
    AddItem(intro, L"Single cut", 1);
    AddItem(intro, L"Off", 0);
    SelectValue(intro, ReadInt(L"Launcher", L"Intro", cfg::kIntro));

    SelectValue(fps, cfg::ClampFps(ReadInt(L"General", L"TargetFPS", cfg::kTargetFps)));
    SelectValue(menu, ReadInt(L"General", L"MenuFPS", cfg::kMenuFps));
    SetToggle(dlg, IDC_HFR, ReadInt(L"General", L"HighFrameRate", cfg::kHighFrameRate) != 0);
    SetToggle(dlg, IDC_QTE, ReadInt(L"Fixes", L"QteFix", cfg::kQteFix) != 0);
    SetToggle(dlg, IDC_BLADE, ReadInt(L"Fixes", L"BladeModeFix", cfg::kBladeModeFix) != 0);
    {   // render size: presets, the user's screen, or whatever the ini holds
        const int legacyH = ReadInt(L"Display", L"Height1920", cfg::kHeight);
        g_resH = ReadInt(L"Display", L"Height", legacyH);
        g_resW = ReadInt(L"Display", L"Width", cfg::kWidth);
        HWND res = GetDlgItem(dlg, IDC_RES);
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        struct { int w, h; const wchar_t* tag; } presets[] = {
            { 1920, 1200, L"16:10" }, { 2560, 1600, L"16:10" }, { 2560, 1440, L"16:9" }, { 3840, 2160, L"16:9" },
            { 2560, 1080, L"21:9" }, { 3440, 1440, L"21:9" }, { 3840, 1600, L"21:9" }, { 5120, 1440, L"32:9" } };
        AddItem(res, L"Off (game default 1920x1080)", 0);
        bool screenListed = false;
        for (auto& pr : presets) {
            const bool mine = pr.w == sw && pr.h == sh;
            screenListed |= mine;
            swprintf_s(buf, L"%d x %d  \x00B7 %s%s", pr.w, pr.h, pr.tag, mine ? L"  \x2190 yours" : L"");
            AddItem(res, buf, (pr.w << 16) | pr.h);
        }
        if (!screenListed && sw >= 1280 && sh >= 720) {
            swprintf_s(buf, L"%d x %d  \x2190 yours", sw, sh);
            AddItem(res, buf, (sw << 16) | sh);
        }
        const int want = g_resH ? ((g_resW << 16) | g_resH) : 0;
        const int n = (int)SendMessageW(res, CB_GETCOUNT, 0, 0);
        int found = -1;
        for (int i = 0; i < n && found < 0; ++i) if ((int)SendMessageW(res, CB_GETITEMDATA, i, 0) == want) found = i;
        if (found < 0) {
            swprintf_s(buf, L"%d x %d  (custom)", g_resW, g_resH);
            found = AddItem(res, buf, want);
        }
        SendMessageW(res, CB_SETCURSEL, found, 0);
    }
    SetToggle(dlg, IDC_VERTPLUS, ReadInt(L"Display", L"VertPlusFov", cfg::kVertPlusFov) != 0);

    int ease = ReadInt(L"Fixes", L"QteEase", cfg::kQteEase);
    ease = ease < 10 ? 10 : ease > 100 ? 100 : ease;
    SendDlgItemMessageW(dlg, IDC_EASE, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendDlgItemMessageW(dlg, IDC_EASE, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(dlg, IDC_EASE, TBM_SETPOS, TRUE, ease);
    UpdateEaseLabel(dlg);

    wchar_t path[MAX_PATH], warn[256] = L"";
    InDir(path, MAX_PATH, kGameExe);
    const bool noGame = !FileExists(path);
    InDir(path, MAX_PATH, L"dinput8.dll");
    const bool noMod = !FileExists(path);
    if (noGame) wcscpy_s(warn, L"\x26A0  Game exe not found: put the launcher in the game folder.");
    else if (noMod) wcscpy_s(warn, L"\x26A0  dinput8.dll (the mod) is missing from the game folder.");
    SetDlgItemTextW(dlg, IDC_WARN, warn);
}

static void Save(HWND dlg)
{
    WriteInt(L"General", L"TargetFPS", SelectedValue(GetDlgItem(dlg, IDC_FPS)));
    WriteInt(L"General", L"MenuFPS", SelectedValue(GetDlgItem(dlg, IDC_MENUFPS)));
    WriteInt(L"General", L"HighFrameRate", GetToggle(dlg, IDC_HFR));
    WriteInt(L"Fixes", L"QteFix", GetToggle(dlg, IDC_QTE));
    WriteInt(L"Fixes", L"BladeModeFix", GetToggle(dlg, IDC_BLADE));
    WriteInt(L"Fixes", L"QteEase", (int)SendDlgItemMessageW(dlg, IDC_EASE, TBM_GETPOS, 0, 0));
    const int res = SelectedValue(GetDlgItem(dlg, IDC_RES));
    WriteInt(L"Display", L"Width", res ? (res >> 16) & 0xFFFF : 1920);
    WriteInt(L"Display", L"Height", res & 0xFFFF);
    // Older mod builds only read Height1920 (width fixed at 1920); keep it in step.
    WriteInt(L"Display", L"Height1920", res && ((res >> 16) & 0xFFFF) == 1920 ? (res & 0xFFFF) : 0);
    WriteInt(L"Display", L"VertPlusFov", GetToggle(dlg, IDC_VERTPLUS));
    WriteInt(L"Debris", L"KeepLastCuts", SelectedValue(GetDlgItem(dlg, IDC_KEEPCUTS)));
    WriteInt(L"Debris", L"KeepCorpses", SelectedValue(GetDlgItem(dlg, IDC_KEEPBODIES)));
    WriteInt(L"Launcher", L"Intro", SelectedValue(GetDlgItem(dlg, IDC_INTRO)));
}

static bool Launch(HWND dlg)
{
    if ((INT_PTR)ShellExecuteW(dlg, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL) > 32) return true;
    // Fallback: start the exe directly (steam_appid.txt lets it run while Steam is open).
    wchar_t exe[MAX_PATH];
    InDir(exe, MAX_PATH, kGameExe);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    MessageBoxW(dlg, L"Could not start the game through Steam or directly.", L"HF Blade", MB_ICONERROR);
    return false;
}

static void ShowTab(HWND dlg, int tab)
{
    g_tab = tab;
    for (int id : kTab0Ids) ShowWindow(GetDlgItem(dlg, id), tab == 0 ? SW_SHOW : SW_HIDE);
    for (int id : kTab1Ids) ShowWindow(GetDlgItem(dlg, id), tab == 1 ? SW_SHOW : SW_HIDE);
    InvalidateRect(dlg, nullptr, FALSE);
    InvalidateRect(GetDlgItem(dlg, IDC_TAB0), nullptr, FALSE);
    InvalidateRect(GetDlgItem(dlg, IDC_TAB1), nullptr, FALSE);
}

// Short description of the chosen shape under the render-size combo.
static void UpdateResInfo(HWND dlg)
{
    const int res = SelectedValue(GetDlgItem(dlg, IDC_RES));
    g_resW = res ? (res >> 16) & 0xFFFF : 1920;
    g_resH = res & 0xFFFF;
    wchar_t buf[256];
    if (!g_resH) {
        wcscpy_s(buf, L"The game's own 1920x1080 (16:9). Nothing is changed.");
    } else {
        const float a = float(g_resW) / float(g_resH), base = 16.0f / 9.0f;
        wchar_t shape[16];
        if (fabsf(a - 1.6f) < 0.01f) wcscpy_s(shape, L"16:10");
        else if (fabsf(a - base) < 0.01f) wcscpy_s(shape, L"16:9");
        else if (a > 2.2f && a < 2.5f) wcscpy_s(shape, L"21:9");
        else if (a > 3.4f && a < 3.7f) wcscpy_s(shape, L"32:9");
        else swprintf_s(shape, L"%.2f:1", a);
        if (fabsf(a - base) < 0.01f) swprintf_s(buf, L"%s: same shape as the original, just sharper.", shape);
        else if (a < base) swprintf_s(buf, L"%s, taller than 16:9: more picture above and below (Vert+).", shape);
        else swprintf_s(buf, L"%s, wider than 16:9: more picture at the sides (Hor+). The HUD is stretched for now.", shape);
    }
    SetDlgItemTextW(dlg, IDC_RESINFO, buf);
    if (g_tab == 1) InvalidateRect(dlg, nullptr, FALSE);
}

// Hover descriptions. Shown only when the pointer rests on a control.
static void AddTooltips(HWND dlg)
{
    HWND tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, dlg, nullptr, nullptr, nullptr);
    if (!tt) return;
    SetWindowTheme(tt, L"DarkMode_Explorer", nullptr);
    SendMessageW(tt, TTM_SETMAXTIPWIDTH, 0, MulDiv(320, (int)GetDpiForWindow(dlg), 96));
    SendMessageW(tt, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SendMessageW(tt, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    struct { int id; const wchar_t* text; } tips[] = {
        { IDC_FPS, L"Frame rate during gameplay. The original game is locked to 60. The mod keeps movement, QTEs and Blade Mode at real-time speed. F8 cycles the presets in game." },
        { IDC_L_FPS, L"Frame rate during gameplay. The original game is locked to 60. The mod keeps movement, QTEs and Blade Mode at real-time speed. F8 cycles the presets in game." },
        { IDC_MENUFPS, L"Frame rate for menus and the scenes the game runs in its 30 fps mode." },
        { IDC_L_MENU, L"Frame rate for menus and the scenes the game runs in its 30 fps mode." },
        { IDC_HFR, L"Turns all frame-rate patches on or off. In game, F9 switches between the mod and the original 60 fps." },
        { IDC_QTE, L"At high frame rates button-mash QTEs (e.g. Sundowner's shield) would need twice the presses. This keeps them at real-time speed." },
        { IDC_BLADE, L"Keeps Blade Mode slow motion and its synced animations at real-time speed at high frame rates." },
        { IDC_EASE, L"How fast a mash gauge drains at high fps, compared with the original. 100% = the same as 60 fps, lower = easier." },
        { IDC_L_EASE, L"How fast a mash gauge drains at high fps, compared with the original. 100% = the same as 60 fps, lower = easier." },
        { IDC_KEEPCUTS, L"Pieces from your last cuts stay on the ground instead of exploding and vanishing. Older ones are cleaned up as usual." },
        { IDC_L_PIECES, L"Pieces from your last cuts stay on the ground instead of exploding and vanishing. Older ones are cleaned up as usual." },
        { IDC_KEEPBODIES, L"Bodies of the last enemies you killed stay on the ground (cyborg soldiers and some others). Older bodies disappear as usual." },
        { IDC_L_BODIES, L"Bodies of the last enemies you killed stay on the ground (cyborg soldiers and some others). Older bodies disappear as usual." },
        { IDC_INTRO, L"The animation played when this launcher opens." },
        { IDC_L_INTRO, L"The animation played when this launcher opens." },
        { IDC_PREVIEW, L"Play the selected opening animation now." },
        { IDC_MUTE, L"Turn the launcher's sound effects on or off." },
        { IDC_RES, L"The size the game renders at when '1920x1080' is selected in its own options. Pick your screen's resolution for a sharp, full-screen picture." },
        { IDC_L_RES, L"The size the game renders at when '1920x1080' is selected in its own options. Pick your screen's resolution for a sharp, full-screen picture." },
        { IDC_VERTPLUS, L"Keeps the picture from looking zoomed in on screens that are not 16:9: taller screens get more view above and below, wider screens more at the sides. Also keeps the 3D HUD layer at the right size." },
        { IDC_SAVE, L"Save the settings to hfblade.ini." },
        { IDC_PLAY, L"Save the settings and start the game through Steam." },
    };
    for (auto& t : tips) {
        TOOLINFOW ti = { sizeof ti };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = dlg;
        ti.uId = (UINT_PTR)GetDlgItem(dlg, t.id);
        ti.lpszText = const_cast<wchar_t*>(t.text);
        SendMessageW(tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static const wchar_t kHint[] = L"In game:   F9  original 60 fps      F8  next FPS preset";
static const UINT WM_APP_INTRO = WM_APP + 1;
static bool g_dumpIntro = false;
static int  g_introMode = 2;

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mi->CtlType == ODT_COMBOBOX) {
            const RECT r = DluRect(dlg, 0, 0, 0, (int)mi->itemID == -1 ? 11 : 12);
            mi->itemHeight = UINT(r.bottom);
            return TRUE;
        }
        break;
    }
    case WM_INITDIALOG: {
        g_bgBrush = CreateSolidBrush(kBg);
        g_cardBrush = CreateSolidBrush(kCard);
        g_accentBrush = CreateSolidBrush(kAccent);
        g_titleFont = MakeFont(dlg, 22, FW_BOLD, L"Segoe UI");
        g_subFont = MakeFont(dlg, 9, FW_NORMAL, L"Segoe UI");
        g_sectionFont = MakeFont(dlg, 8, FW_BOLD, L"Segoe UI");
        g_uiFont = MakeFont(dlg, 9, FW_NORMAL, L"Segoe UI");
        g_btnFont = MakeFont(dlg, 9, FW_SEMIBOLD, L"Segoe UI");
        const int owner[] = { IDC_HFR, IDC_QTE, IDC_BLADE, IDC_VERTPLUS, IDC_PLAY, IDC_SAVE, IDC_XMIN, IDC_XCLOSE, IDC_PREVIEW, IDC_MUTE, IDC_TAB0, IDC_TAB1 };
        for (int id : owner) SetWindowSubclass(GetDlgItem(dlg, id), HoverProc, 1, 0);
        SetDlgItemTextW(dlg, IDC_HINT, kHint);
        SetWindowTextW(dlg, L"HF Blade");
        if (HICON big = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0)) {
            SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)big);
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        }
        ApplyWindowTheme(dlg);
        Load(dlg);
        UpdateResInfo(dlg);
        AddTooltips(dlg);
        ShowTab(dlg, 0);
        // Start invisible; the intro reveals the window.
        SetWindowLongPtrW(dlg, GWL_EXSTYLE, GetWindowLongPtrW(dlg, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(dlg, 0, 0, LWA_ALPHA);
        g_introMode = ReadInt(L"Launcher", L"Intro", cfg::kIntro);
        g_soundOn = ReadInt(L"Launcher", L"Sound", cfg::kSound) != 0;
        PostMessageW(dlg, WM_APP_INTRO, 0, 0);
        SetFocus(GetDlgItem(dlg, IDC_PLAY));
        return FALSE;
    }
    case WM_APP_INTRO:
        StartIntro(dlg, g_introMode, g_dumpIntro);
        return TRUE;
    case WM_NCHITTEST:   // borderless: drag the window by any empty area (child controls get their own hits)
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, HTCAPTION);
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PRINTCLIENT:
        PaintBackground(dlg, (HDC)wp);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(dlg, &ps);
        RECT rc;
        GetClientRect(dlg, &rc);
        HDC mem = CreateCompatibleDC(dc);   // double-buffered to avoid flicker
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintBackground(dlg, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(dlg, &ps);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_bgBrush;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        const int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, OPAQUE);
        const bool footer = id == IDC_HINT || id == IDC_WARN;
        SetBkColor(dc, footer ? kBg : kCard);
        SetTextColor(dc, id == IDC_WARN ? kWarn : id == IDC_HINT || id == IDC_RESNOTE ? kMuted
                         : id == IDC_EASEVAL || id == IDC_RESINFO ? kAccentHi : kText);
        SelectObject(dc, id == IDC_EASEVAL ? g_btnFont : g_uiFont);
        return (INT_PTR)(footer ? g_bgBrush : g_cardBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kText);
        SetBkColor(dc, kField);
        static HBRUSH field = CreateSolidBrush(kField);
        return (INT_PTR)field;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (di->CtlType == ODT_COMBOBOX) DrawComboItem(di);
        else if (IsToggle((int)di->CtlID)) DrawToggle(di);
        else DrawButton(di);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* h = reinterpret_cast<NMHDR*>(lp);
        if (h->idFrom == IDC_EASE && h->code == NM_CUSTOMDRAW) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, CustomDrawSlider(reinterpret_cast<NMCUSTOMDRAW*>(lp)));
            return TRUE;
        }
        break;
    }
    case WM_HSCROLL:
        UpdateEaseLabel(dlg);
        return TRUE;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(dlg, 1);
            SetDlgItemTextW(dlg, IDC_HINT, kHint);
        } else if (wp == 2) {
            const DWORD el = GetTickCount() - g_intro.start;
            if (el >= g_intro.durMs) EndIntro();
            else PresentIntroFrame(REAL(el));
        }
        return TRUE;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (IsToggle(id) && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
            SetToggle(dlg, id, !GetToggle(dlg, id));
            return TRUE;
        }
        if (code == CBN_SELCHANGE || code == CBN_DROPDOWN || code == CBN_CLOSEUP) {
            if (id == IDC_RES && code == CBN_SELCHANGE) UpdateResInfo(dlg);
            InvalidateRect((HWND)lp, nullptr, FALSE);
            return TRUE;
        }
        switch (id) {
        case IDOK:
        case IDC_PLAY:
            Save(dlg);
            if (Launch(dlg)) EndDialog(dlg, IDC_PLAY);
            return TRUE;
        case IDC_SAVE:
            Save(dlg);
            SetDlgItemTextW(dlg, IDC_HINT, L"\x2713  Saved to hfblade.ini");
            SetTimer(dlg, 1, 1800, nullptr);
            return TRUE;
        case IDC_TAB0:
        case IDC_TAB1:
            ShowTab(dlg, id - IDC_TAB0);
            return TRUE;
        case IDC_MUTE:
            g_soundOn = !g_soundOn;
            WriteInt(L"Launcher", L"Sound", g_soundOn ? 1 : 0);
            if (!g_soundOn) Audio_StopAll();
            InvalidateRect(GetDlgItem(dlg, IDC_MUTE), nullptr, FALSE);
            SetDlgItemTextW(dlg, IDC_HINT, g_soundOn ? L"\x266A  Sound on" : L"Sound off");
            SetTimer(dlg, 1, 1500, nullptr);
            return TRUE;
        case IDC_PREVIEW:
            StartIntro(dlg, SelectedValue(GetDlgItem(dlg, IDC_INTRO)), false);
            return TRUE;
        case IDC_XMIN:
            ShowWindow(dlg, SW_MINIMIZE);
            return TRUE;
        case IDC_XCLOSE:
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) slash[1] = 0;
    InDir(g_ini, MAX_PATH, L"hfblade.ini");
    if (!FileExists(g_ini)) {   // migrate settings from the pre-rename file, else write defaults
        wchar_t legacy[MAX_PATH];
        InDir(legacy, MAX_PATH, L"mgr-hfr.ini");
        if (!FileExists(legacy) || !CopyFileW(legacy, g_ini, TRUE)) WriteDefaults();
    }
    g_dumpIntro = cmd && wcsstr(cmd, L"--dump-intro") != nullptr;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    GdiplusStartup(&gdiplus, &gsi, nullptr);
    LoadArt();
    if (!g_dumpIntro) Audio_Init(g_dir);
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DlgProc, 0);
    Audio_Shutdown();
    delete g_art;
    GdiplusShutdown(gdiplus);
    return 0;
}
