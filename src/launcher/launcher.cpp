// HF Blade launcher: edits hfblade.ini (next to this exe) and starts the game through Steam.
// Native Win32 window with a custom dark theme (GDI+ for anti-aliased shapes) and a "cut" intro.
// Layout: sidebar with six pages (Play, Gameplay, Display, Photo Mode, Launcher, Log), content area
// with section cards, and a bottom bar (status, Save, Save & Play). Controls are positioned in code
// (BuildLayout), so the dialog template in launcher.rc only lists them.
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
#include <string>
#include <vector>
#include "resource.h"
#include "audio.h"
#include "../config.h"

using namespace Gdiplus;

static wchar_t g_dir[MAX_PATH];   // folder of this exe, with trailing backslash
static wchar_t g_ini[MAX_PATH];
static const wchar_t kGameExe[] = L"METAL GEAR RISING REVENGEANCE.exe";
static const wchar_t kSteamUrl[] = L"steam://rungameid/235460";
static const wchar_t kVersion[] = L"Settings launcher";

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
static const COLORREF kBg      = RGB(0x0E, 0x0F, 0x12);
static const COLORREF kSide    = RGB(0x11, 0x13, 0x17);   // sidebar
static const COLORREF kCard    = RGB(0x18, 0x1A, 0x1F);
static const COLORREF kField   = RGB(0x22, 0x25, 0x2B);
static const COLORREF kFieldHi = RGB(0x2A, 0x2D, 0x34);
static const COLORREF kBorder  = RGB(0x2B, 0x2E, 0x35);
static const COLORREF kText    = RGB(0xE8, 0xE9, 0xEC);
static const COLORREF kMuted   = RGB(0x8B, 0x90, 0x9A);
static const COLORREF kDim     = RGB(0x76, 0x7B, 0x86);   // descriptions
static const COLORREF kAccent  = RGB(0x1C, 0x9C, 0xEC);   // MGR electric blue
static const COLORREF kAccentHi = RGB(0x5C, 0xCB, 0xFF);
static const COLORREF kAccentDn = RGB(0x12, 0x6E, 0xB0);   // pressed
static const COLORREF kDanger  = RGB(0xC4, 0x2B, 0x1C);   // close-button hover
static const COLORREF kWarn    = RGB(0xF0, 0xB4, 0x29);
static const COLORREF kOff     = RGB(0x3A, 0x3D, 0x45);

static Color C(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// ---------------------------------------------------------------------------
// Layout. Design size in pixels at 100%; everything is multiplied by g_scale (DPI, shrunk to fit
// small screens) through S().
// ---------------------------------------------------------------------------
static const int kWinW = 1000, kWinH = 740, kSideW = 220, kPadX = 24, kBarH = 56;

enum { PG_PLAY, PG_GAME, PG_DISP, PG_PHOTO, PG_LAUNCH, PG_LOG, PG_COUNT };
static const wchar_t* const kPageNames[PG_COUNT] = { L"Play", L"Gameplay", L"Display", L"Photo Mode", L"Launcher", L"Log" };

static float g_scale = 1.0f;
static int S(int v) { return (int)floorf(float(v) * g_scale + 0.5f); }

struct PCard { int page; RECT r; const wchar_t* title; };
struct PText { int page; RECT r; const wchar_t* text; };
static std::vector<PCard> g_cards;
static std::vector<PText> g_descs;
static std::vector<PText> g_lines;               // 1 px row separators (text unused)
static std::vector<int>   g_pageIds[PG_COUNT];   // controls shown on each page
static RECT g_banner, g_summary, g_shapeBox, g_shapeCap, g_hintRect, g_logCard, g_logTitle;

static int g_page = PG_PLAY;
static int g_resW = 1920, g_resH = 1200;   // selected render size (H = 0: game default)
static int g_photoKey = cfg::kPhotoKey;
static bool g_dirty = false;
static HWND g_dlg;

static HBRUSH g_bgBrush, g_cardBrush, g_accentBrush, g_sideBrush, g_fieldBrush;
static HFONT  g_titleFont, g_bigTitleFont, g_tagFont, g_subFont, g_sectionFont, g_uiFont, g_btnFont, g_descFont,
              g_navFont, g_valueFont, g_monoFont, g_playFont;
static HWND   g_hot;          // owner-drawn control under the mouse
static Bitmap* g_art;         // optional key art
static Bitmap* g_bannerBmp;   // banner (art cropped to fill + shading), rendered once per size
static bool    g_soundOn = true; // cut sound effects ([Launcher] Sound)

static void AddRoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    const REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static HFONT MakeFont(int pt, int weight, const wchar_t* face)
{
    return CreateFontW(-(int)floorf(float(pt) * 96.0f * g_scale / 72.0f + 0.5f), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
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

// GDI text after GDI+ drawing on the same DC: finish the GDI+ batch first.
static void Txt(Graphics& g, HDC dc, const wchar_t* s, RECT rc, HFONT font, COLORREF color, UINT flags)
{
    g.Flush(FlushIntentionSync);
    DrawTextIn(dc, s, rc, font, color, flags);
}

// Banner crop focus: the image row (fraction of its height) placed at a given banner row.
static float g_artFocusY = 0.555f, g_artFocusAt = 0.52f;   // user cover art: Raiden's eye

// Built-in banner: Tunahan's own Raiden drawing (src/launcher/banner.jpg, RCDATA 2), tinted for the
// dark theme. Used when no key-art file sits next to the exe.
static Bitmap* LoadEmbeddedBanner()
{
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));   // RT_RCDATA
    if (!r) return nullptr;
    const DWORD size = SizeofResource(mod, r);
    const void* data = LockResource(LoadResource(mod, r));
    if (!data || !size) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return nullptr;
    memcpy(GlobalLock(mem), data, size);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) { GlobalFree(mem); return nullptr; }
    Bitmap* src = Bitmap::FromStream(stream);
    Bitmap* copy = nullptr;
    if (src && src->GetLastStatus() == Ok) {   // copy so the stream can be released
        copy = new Bitmap(src->GetWidth(), src->GetHeight(), PixelFormat32bppARGB);
        Graphics g(copy);
        g.DrawImage(src, 0, 0, src->GetWidth(), src->GetHeight());
    }
    delete src;
    stream->Release();
    return copy;
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
    if ((g_art = LoadEmbeddedBanner()) != nullptr) { g_artFocusY = 0.33f; g_artFocusAt = 0.42f; }   // drawing: masked face
}

// ---------------------------------------------------------------------------
// Texts shown as inline descriptions and as hover tooltips (same source).
// ---------------------------------------------------------------------------
struct Tip { int id, label; const wchar_t* text; };
static const Tip kTips[] = {
    { IDC_FPS, IDC_L_FPS, L"Frame rate during gameplay. The original game is locked to 60. The mod keeps movement, QTEs and Blade Mode at real-time speed. F8 cycles the presets in game." },
    { IDC_MENUFPS, IDC_L_MENU, L"Frame rate for menus and the scenes the game runs in its 30 fps mode." },
    { IDC_HFR, 0, L"Turns all frame-rate patches on or off. In game, F9 switches between the mod and the original 60 fps." },
    { IDC_QTE, 0, L"At high frame rates button-mash QTEs (e.g. Sundowner's shield) would need twice the presses. This keeps them at real-time speed." },
    { IDC_WEAPONSW, 0, L"In game, D-pad down switches the unique (heavy attack) weapon to the next one you own, without opening the weapon menu." },
    { IDC_BLADE, 0, L"Keeps Blade Mode slow motion and its synced animations at real-time speed at high frame rates." },
    { IDC_EASE, IDC_L_EASE, L"How fast a mash gauge drains at high fps, compared with the original. 100% = the same as 60 fps, lower = easier." },
    { IDC_KEEPCUTS, IDC_L_PIECES, L"Pieces from your last cuts stay on the ground instead of exploding and vanishing. Older ones are cleaned up as usual." },
    { IDC_KEEPBODIES, IDC_L_BODIES, L"Bodies of the last enemies you killed stay on the ground (cyborg soldiers and some others). Older bodies disappear as usual." },
    { IDC_INTRO, IDC_L_INTRO, L"The animation played when this launcher opens." },
    { IDC_PREVIEW, 0, L"Play the selected opening animation now." },
    { IDC_MUTE, 0, L"Turn the launcher's sound effects on or off." },
    { IDC_SOUND, 0, L"Play the cut sound effects when the launcher opens and in the preview. The speaker button in the top bar does the same." },
    { IDC_RES, IDC_L_RES, L"The size the game renders at when '1920x1080' is selected in its own options. Pick your screen's resolution for a sharp, full-screen picture." },
    { IDC_VERTPLUS, 0, L"Keeps the picture from looking zoomed in on screens that are not 16:9: taller screens get more view above and below, wider screens more at the sides. Also keeps the 3D HUD layer at the right size." },
    { IDC_PHOTO, 0, L"In game, F10 freezes everything, hides the HUD and lets you fly the camera to take screenshots (PrtSc with ReShade, F12 with Steam). F10 again resumes the game where it stopped." },
    { IDC_SAVE, 0, L"Save the settings to hfblade.ini." },
    { IDC_PLAY, 0, L"Save the settings and start the game through Steam." },
    { IDC_PLAYBIG, 0, L"Save the settings and start the game through Steam." },
};

static const wchar_t* DescFor(int id)
{
    for (const Tip& t : kTips) if (t.id == id) return t.text;
    return L"";
}

// ---------------------------------------------------------------------------
// Layout builder: places every control and records cards, descriptions and separators for painting.
// ---------------------------------------------------------------------------
static int g_cy, g_cardTop, g_cardPage, g_ctlH;
static const wchar_t* g_cardTitle;
static bool g_firstRow;

static int CX() { return S(kSideW + kPadX); }
static int CW() { return S(kWinW - kSideW - 2 * kPadX); }
static int IX() { return CX() + S(20); }
static int IW() { return CW() - S(40); }

static void PlaceCtl(HWND dlg, int page, int id, int x, int y, int w, int h)
{
    SetWindowPos(GetDlgItem(dlg, id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (page >= 0) g_pageIds[page].push_back(id);
}

static int MeasureDesc(HDC dc, const wchar_t* text, int width)
{
    RECT r = { 0, 0, width, 0 };
    HGDIOBJ old = SelectObject(dc, g_descFont);
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

static void BeginCard(int page, const wchar_t* title)
{
    g_cardPage = page;
    g_cardTitle = title;
    g_cardTop = g_cy;
    g_cy += S(32);
    g_firstRow = true;
}

static void EndCard()
{
    const RECT r = { CX(), g_cardTop, CX() + CW(), g_cy };
    g_cards.push_back({ g_cardPage, r, g_cardTitle });
    g_cy = r.bottom + S(10);
}

static void RowStart()
{
    if (!g_firstRow) {
        const RECT r = { IX(), g_cy, IX() + IW(), g_cy + 1 };
        g_lines.push_back({ g_cardPage, r, nullptr });
        g_cy += S(8);
    }
    g_firstRow = false;
}

static void RowDesc(HDC dc, const wchar_t* text)
{
    if (!text[0]) return;
    const int h = MeasureDesc(dc, text, IW());
    const RECT r = { IX(), g_cy, IX() + IW(), g_cy + h };
    g_descs.push_back({ g_cardPage, r, text });
    g_cy += h + S(8);
}

static void RowCombo(HWND dlg, HDC dc, int labelId, int ctlId, int ctlW, int reserveRight)
{
    RowStart();
    PlaceCtl(dlg, g_cardPage, labelId, IX(), g_cy, S(320), g_ctlH);
    PlaceCtl(dlg, g_cardPage, ctlId, IX() + IW() - reserveRight - ctlW, g_cy, ctlW, S(260));
    g_cy += g_ctlH + S(4);
    RowDesc(dc, DescFor(ctlId));
}

static void RowToggle(HWND dlg, HDC dc, int id)
{
    RowStart();
    PlaceCtl(dlg, g_cardPage, id, IX(), g_cy, IW(), g_ctlH);
    g_cy += g_ctlH + S(4);
    RowDesc(dc, DescFor(id));
}

static void BuildLayout(HWND dlg)
{
    HDC dc = GetDC(dlg);
    const int W = S(kWinW), H = S(kWinH), barTop = H - S(kBarH);
    g_ctlH = S(24);
    g_cards.clear(); g_descs.clear(); g_lines.clear();
    for (auto& v : g_pageIds) v.clear();

    // Chrome: sidebar rows, caption buttons, bottom bar.
    for (int i = 0; i < PG_COUNT; ++i) PlaceCtl(dlg, -1, IDC_NAV0 + i, 0, S(150) + i * S(46), S(kSideW) - 1, S(42));
    PlaceCtl(dlg, -1, IDC_MUTE, W - S(120), 0, S(40), S(30));
    PlaceCtl(dlg, -1, IDC_XMIN, W - S(80), 0, S(40), S(30));
    PlaceCtl(dlg, -1, IDC_XCLOSE, W - S(40), 0, S(40), S(30));
    PlaceCtl(dlg, -1, IDC_HINT, CX(), barTop + S(9), S(380), S(20));
    PlaceCtl(dlg, -1, IDC_WARN, CX(), barTop + S(29), S(380), S(20));
    PlaceCtl(dlg, -1, IDC_PLAY, W - S(kPadX) - S(160), barTop + S(10), S(160), S(36));
    PlaceCtl(dlg, -1, IDC_SAVE, W - S(kPadX) - S(160) - S(10) - S(110), barTop + S(10), S(110), S(36));

    // Play: banner, summary of the main settings, big Save & Play.
    const int top = S(36);
    g_banner = { CX(), top, CX() + CW(), top + S(410) };
    g_summary = { CX(), g_banner.bottom + S(14), CX() + CW(), g_banner.bottom + S(14) + S(104) };
    PlaceCtl(dlg, PG_PLAY, IDC_PLAYBIG, CX(), g_summary.bottom + S(16), CW(), S(60));
    g_hintRect = { CX(), g_summary.bottom + S(16) + S(60) + S(14), CX() + CW(), g_summary.bottom + S(16) + S(60) + S(34) };

    // Gameplay.
    g_cy = top;
    BeginCard(PG_GAME, L"FRAME RATE");
    RowCombo(dlg, dc, IDC_L_FPS, IDC_FPS, S(220), 0);
    RowCombo(dlg, dc, IDC_L_MENU, IDC_MENUFPS, S(220), 0);
    RowToggle(dlg, dc, IDC_HFR);
    EndCard();
    BeginCard(PG_GAME, L"FIXES");
    RowToggle(dlg, dc, IDC_QTE);
    RowToggle(dlg, dc, IDC_BLADE);
    RowStart();
    PlaceCtl(dlg, PG_GAME, IDC_L_EASE, IX(), g_cy, S(200), g_ctlH);
    PlaceCtl(dlg, PG_GAME, IDC_EASE, IX() + IW() - S(274), g_cy, S(230), g_ctlH);
    PlaceCtl(dlg, PG_GAME, IDC_EASEVAL, IX() + IW() - S(44), g_cy, S(44), g_ctlH);
    g_cy += g_ctlH + S(4);
    RowDesc(dc, DescFor(IDC_EASE));
    RowToggle(dlg, dc, IDC_WEAPONSW);
    EndCard();
    BeginCard(PG_GAME, L"AFTERMATH");
    RowCombo(dlg, dc, IDC_L_PIECES, IDC_KEEPCUTS, S(220), 0);
    RowCombo(dlg, dc, IDC_L_BODIES, IDC_KEEPBODIES, S(220), 0);
    EndCard();

    // Display.
    g_cy = top;
    BeginCard(PG_DISP, L"RESOLUTION");
    RowCombo(dlg, dc, IDC_L_RES, IDC_RES, S(300), 0);
    RowToggle(dlg, dc, IDC_VERTPLUS);
    RowStart();
    PlaceCtl(dlg, PG_DISP, IDC_RESINFO, IX(), g_cy, IW(), S(20));
    g_cy += S(24);
    PlaceCtl(dlg, PG_DISP, IDC_RESNOTE, IX(), g_cy, IW(), S(36));
    g_cy += S(44);
    EndCard();
    BeginCard(PG_DISP, L"SHAPE");
    g_shapeBox = { IX(), g_cy, IX() + IW(), g_cy + S(150) };
    g_cy += S(154);
    g_shapeCap = { IX(), g_cy, IX() + IW(), g_cy + S(20) };
    g_cy += S(30);
    EndCard();

    // Photo Mode.
    g_cy = top;
    BeginCard(PG_PHOTO, L"PHOTO MODE");
    RowToggle(dlg, dc, IDC_PHOTO);
    EndCard();
    BeginCard(PG_PHOTO, L"CONTROLS");
    RowStart();
    {
        const int colW[4] = { S(150), S(190), S(150), S(160) }, colX[4] = { 0, S(170), S(366), S(526) };
        const int ids[4] = { IDC_PHOTOPAD, IDC_PHOTOPAD2, IDC_PHOTOKB, IDC_PHOTOKB2 };
        for (int i = 0; i < 4; ++i) PlaceCtl(dlg, PG_PHOTO, ids[i], IX() + colX[i], g_cy, colW[i], S(184));
        g_cy += S(196);
    }
    EndCard();

    // Launcher.
    g_cy = top;
    BeginCard(PG_LAUNCH, L"STARTUP");
    const int introTop = g_cy;   // first row of the card: no separator above it
    RowCombo(dlg, dc, IDC_L_INTRO, IDC_INTRO, S(190), S(48));
    PlaceCtl(dlg, PG_LAUNCH, IDC_PREVIEW, IX() + IW() - S(40), introTop, S(40), g_ctlH);
    EndCard();
    BeginCard(PG_LAUNCH, L"SOUND");
    RowToggle(dlg, dc, IDC_SOUND);
    EndCard();

    // Log.
    PlaceCtl(dlg, PG_LOG, IDC_LOGFOLDER, CX() + CW() - S(130), top, S(130), S(30));
    PlaceCtl(dlg, PG_LOG, IDC_LOGREFRESH, CX() + CW() - S(130) - S(10) - S(100), top, S(100), S(30));
    g_logTitle = { CX(), top, CX() + S(400), top + S(30) };
    g_logCard = { CX(), top + S(42), CX() + CW(), barTop - S(16) };
    PlaceCtl(dlg, PG_LOG, IDC_LOGVIEW, g_logCard.left + S(14), g_logCard.top + S(12), g_logCard.right - g_logCard.left - S(28),
             g_logCard.bottom - g_logCard.top - S(24));
    ReleaseDC(dlg, dc);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
static void DrawNavIcon(Graphics& g, int idx, REAL x, REAL y, REAL s, COLORREF col)
{
    const REAL u = s / 20.0f;
    Pen p(C(col), 1.5f);
    p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound); p.SetLineJoin(LineJoinRound);
    SolidBrush dot(C(col)), side(C(kSide));
    switch (idx) {
    case PG_PLAY: {
        PointF t[3] = { PointF(x + 6 * u, y + 3 * u), PointF(x + 6 * u, y + 17 * u), PointF(x + 17 * u, y + 10 * u) };
        g.DrawPolygon(&p, t, 3);
        break;
    }
    case PG_GAME: {   // gamepad
        GraphicsPath path;
        AddRoundRect(path, x + 1 * u, y + 5 * u, 18 * u, 11 * u, 5 * u);
        g.DrawPath(&p, &path);
        g.DrawLine(&p, x + 6 * u, y + 8 * u, x + 6 * u, y + 13 * u);
        g.DrawLine(&p, x + 3.5f * u, y + 10.5f * u, x + 8.5f * u, y + 10.5f * u);
        g.FillEllipse(&dot, x + 12 * u, y + 8 * u, 2.2f * u, 2.2f * u);
        g.FillEllipse(&dot, x + 14.8f * u, y + 11 * u, 2.2f * u, 2.2f * u);
        break;
    }
    case PG_DISP: {   // monitor
        GraphicsPath path;
        AddRoundRect(path, x + 1 * u, y + 3 * u, 18 * u, 12 * u, 2 * u);
        g.DrawPath(&p, &path);
        g.DrawLine(&p, x + 7 * u, y + 18 * u, x + 13 * u, y + 18 * u);
        g.DrawLine(&p, x + 10 * u, y + 15 * u, x + 10 * u, y + 18 * u);
        break;
    }
    case PG_PHOTO: {   // camera
        GraphicsPath path;
        AddRoundRect(path, x + 1 * u, y + 6 * u, 18 * u, 11 * u, 2.5f * u);
        g.DrawPath(&p, &path);
        g.DrawEllipse(&p, x + 6.8f * u, y + 8.3f * u, 6.4f * u, 6.4f * u);
        PointF b[4] = { PointF(x + 6 * u, y + 6 * u), PointF(x + 7.8f * u, y + 3.4f * u), PointF(x + 12.2f * u, y + 3.4f * u), PointF(x + 14 * u, y + 6 * u) };
        g.DrawLines(&p, b, 4);
        break;
    }
    case PG_LAUNCH: {   // sliders
        const REAL ys[3] = { 5 * u, 10 * u, 15 * u }, xs[3] = { 7 * u, 13 * u, 8.5f * u };
        for (int i = 0; i < 3; ++i) {
            g.DrawLine(&p, x + 2 * u, y + ys[i], x + 18 * u, y + ys[i]);
            g.FillEllipse(&side, x + xs[i] - 2.2f * u, y + ys[i] - 2.2f * u, 4.4f * u, 4.4f * u);
            g.DrawEllipse(&p, x + xs[i] - 2.2f * u, y + ys[i] - 2.2f * u, 4.4f * u, 4.4f * u);
        }
        break;
    }
    case PG_LOG: {   // document
        GraphicsPath path;
        AddRoundRect(path, x + 4 * u, y + 2 * u, 12 * u, 16 * u, 1.8f * u);
        g.DrawPath(&p, &path);
        for (int i = 0; i < 3; ++i) g.DrawLine(&p, x + 7 * u, y + (7 + 3 * i) * u, x + 13 * u, y + (7 + 3 * i) * u);
        break;
    }
    }
}

// Play page banner: key art cropped to fill (or a dark gradient), darkened towards the bottom.
static void EnsureBannerBitmap()
{
    const int w = g_banner.right - g_banner.left, h = g_banner.bottom - g_banner.top;
    if (g_bannerBmp && (int)g_bannerBmp->GetWidth() == w && (int)g_bannerBmp->GetHeight() == h) return;
    delete g_bannerBmp;
    g_bannerBmp = new Bitmap(w, h, PixelFormat32bppPARGB);
    Graphics g(g_bannerBmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    const REAL W = REAL(w), H = REAL(h);
    if (g_art) {
        const REAL iw = REAL(g_art->GetWidth()), ih = REAL(g_art->GetHeight()), aspect = W / H;
        REAL sw = iw, sh = ih, sx = 0, sy = 0;
        if (iw / ih > aspect) { sw = ih * aspect; sx = (iw - sw) / 2; }
        else {
            // Taller-than-banner art: keep Raiden's face in view (g_artFocusY of the image at g_artFocusAt of
            // the banner), below his hair and above the title text.
            sh = iw / aspect;
            sy = ih * g_artFocusY - sh * g_artFocusAt;
            if (sy < 0) sy = 0;
            if (sy > ih - sh) sy = ih - sh;
        }
        g.DrawImage(g_art, RectF(0, 0, W, H), sx, sy, sw, sh, UnitPixel);
    } else {
        LinearGradientBrush bg(PointF(0, 0), PointF(W, H), C(RGB(0x12, 0x2A, 0x45)), C(RGB(0x0A, 0x0C, 0x10)));
        g.FillRectangle(&bg, 0.0f, 0.0f, W, H);
        SolidBrush red(C(kAccent, 120)), dim(C(kAccent, 40));
        for (int i = 0; i < 4; ++i) {
            const REAL x = W * (0.30f + 0.17f * i), s = W * 0.05f;
            PointF p[4] = { PointF(x, H), PointF(x + s, H), PointF(x + s + W * 0.22f, 0), PointF(x + W * 0.22f, 0) };
            g.FillPolygon(i == 2 ? &red : &dim, p, 4);
        }
    }
    const REAL f0 = H * 0.62f;   // start darkening below the face
    LinearGradientBrush fade(PointF(0, f0), PointF(0, H), C(RGB(8, 9, 12), 0), C(RGB(8, 9, 12), 238));
    g.FillRectangle(&fade, RectF(0, f0, W, H - f0 + 1));
    LinearGradientBrush fadeL(PointF(0, 0), PointF(W * 0.55f, 0), C(RGB(8, 9, 12), 110), C(RGB(8, 9, 12), 0));
    g.FillRectangle(&fadeL, RectF(0, 0, W * 0.55f, H));
}

static void PaintBanner(HDC dc, Graphics& g)
{
    EnsureBannerBitmap();
    const REAL x = REAL(g_banner.left), y = REAL(g_banner.top), w = REAL(g_banner.right - g_banner.left), h = REAL(g_banner.bottom - g_banner.top);
    GraphicsPath path;
    AddRoundRect(path, x + 0.5f, y + 0.5f, w - 1, h - 1, 8.0f);
    g.SetClip(&path);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.DrawImage(g_bannerBmp, x, y, w, h);
    g.ResetClip();
    Pen border(C(kBorder), 1.0f);
    g.DrawPath(&border, &path);
    SolidBrush bar(C(kAccent));
    g.FillRectangle(&bar, x + S(24), y + h - S(104), REAL(S(4)), REAL(S(84)));
    RECT t = { g_banner.left + S(40), g_banner.bottom - S(110), g_banner.right - S(20), g_banner.bottom - S(54) };
    Txt(g, dc, L"HF BLADE", t, g_bigTitleFont, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT s = { t.left, g_banner.bottom - S(56), t.right, g_banner.bottom - S(22) };
    DrawTextIn(dc, L"Remaster-grade enhancements for METAL GEAR RISING: REVENGEANCE", s, g_tagFont, RGB(0xC9, 0xCD, 0xD4), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

static const wchar_t* ComboText(HWND dlg, int id, wchar_t* out, int n)
{
    HWND c = GetDlgItem(dlg, id);
    const int i = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
    out[0] = 0;
    if (i >= 0 && SendMessageW(c, CB_GETLBTEXTLEN, i, 0) < n) SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)out);
    return out;
}

static bool GetToggle(HWND dlg, int id) { return GetWindowLongPtrW(GetDlgItem(dlg, id), GWLP_USERDATA) != 0; }

static void PaintSummary(HWND dlg, HDC dc, Graphics& g)
{
    const RECT r = g_summary;
    GraphicsPath path;
    AddRoundRect(path, REAL(r.left) + 0.5f, REAL(r.top) + 0.5f, REAL(r.right - r.left - 1), REAL(r.bottom - r.top - 1), 6.0f);
    SolidBrush fill(C(kCard));
    Pen border(C(kBorder), 1.0f);
    g.FillPath(&fill, &path);
    g.DrawPath(&border, &path);
    RECT title = { r.left + S(20), r.top + S(10), r.left + S(300), r.top + S(28) };
    Txt(g, dc, L"CURRENT SETTINGS", title, g_sectionFont, kAccentHi, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    wchar_t fps[64], size[64], key[32];
    ComboText(dlg, IDC_FPS, fps, 64);
    if (g_resH) swprintf_s(size, L"%d x %d", g_resW, g_resH); else wcscpy_s(size, L"Game default");
    const bool weapon = GetToggle(dlg, IDC_WEAPONSW), photo = GetToggle(dlg, IDC_PHOTO);
    if (g_photoKey >= 0x70 && g_photoKey <= 0x87) swprintf_s(key, L"F%d", g_photoKey - 0x6F); else swprintf_s(key, L"Key 0x%02X", g_photoKey);
    const wchar_t* caps[4] = { L"GAMEPLAY FPS", L"RENDER SIZE", L"QUICK WEAPON SWITCH", L"PHOTO MODE KEY" };
    const wchar_t* vals[4] = { fps, size, weapon ? L"On" : L"Off", photo ? key : L"Off" };
    const COLORREF cols[4] = { kText, kText, weapon ? kText : kMuted, photo ? kText : kMuted };
    const int tw = (r.right - r.left - S(40)) / 4;
    for (int i = 0; i < 4; ++i) {
        const int x = r.left + S(20) + i * tw;
        if (i) {
            Pen sep(C(kBorder), 1.0f);
            g.DrawLine(&sep, x - S(10), r.top + S(42), x - S(10), r.bottom - S(16));
        }
        RECT c = { x, r.top + S(40), x + tw - S(16), r.top + S(58) };
        Txt(g, dc, caps[i], c, g_sectionFont, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT v = { x, r.top + S(60), x + tw - S(16), r.top + S(96) };
        DrawTextIn(dc, vals[i], v, g_valueFont, cols[i], DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

// Display page: the chosen screen shape next to the original 16:9 view, showing where the extra
// picture comes from (Vert+ above/below, Hor+ at the sides).
static void PaintShape(HDC dc, Graphics& g)
{
    const RECT box = g_shapeBox;
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
    Txt(g, dc, same ? L"16:9 \x2014 same shape as the original" : L"original 16:9 view", t, g_uiFont, kMuted,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DrawTextIn(dc, same ? L"" : aspect < base ? L"Blue: extra view above and below (Vert+)" : L"Blue: extra view at the sides (Hor+)",
               g_shapeCap, g_uiFont, kAccentHi, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

static void FillRound(Graphics& g, const RECT& r, COLORREF fill, COLORREF border, REAL radius)
{
    GraphicsPath path;
    AddRoundRect(path, REAL(r.left) + 0.5f, REAL(r.top) + 0.5f, REAL(r.right - r.left - 1), REAL(r.bottom - r.top - 1), radius);
    SolidBrush b(C(fill));
    Pen p(C(border), 1.0f);
    g.FillPath(&b, &path);
    g.DrawPath(&p, &path);
}

// Whole window background: sidebar, page content (cards, descriptions, banner, ...), bottom bar.
static void PaintBackground(HWND dlg, HDC dc)
{
    RECT client;
    GetClientRect(dlg, &client);
    FillRect(dc, &client, g_bgBrush);
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    // Sidebar.
    const int sideW = S(kSideW);
    SolidBrush side(C(kSide));
    g.FillRectangle(&side, 0, 0, sideW, client.bottom);
    Pen line(C(kBorder), 1.0f);
    g.DrawLine(&line, sideW - 1, 0, sideW - 1, client.bottom);
    SolidBrush bar(C(kAccent));
    g.FillRectangle(&bar, S(24), S(32), S(4), S(84));
    RECT t = { S(38), S(26), sideW - S(10), S(62) };
    Txt(g, dc, L"HF BLADE", t, g_titleFont, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT sub = { S(38), S(64), sideW - S(10), S(92) };
    DrawTextIn(dc, L"METAL GEAR RISING:\nREVENGEANCE", sub, g_descFont, kMuted, DT_LEFT | DT_WORDBREAK);
    RECT ver = { S(38), S(96), sideW - S(10), S(116) };
    DrawTextIn(dc, kVersion, ver, g_descFont, kDim, DT_LEFT | DT_SINGLELINE);
    RECT note = { S(24), client.bottom - S(84), sideW - S(20), client.bottom - S(16) };
    DrawTextIn(dc, L"Patches the game in memory; game files are never modified.", note, g_descFont, kDim, DT_LEFT | DT_BOTTOM | DT_WORDBREAK);

    // Bottom bar separator.
    g.DrawLine(&line, sideW, client.bottom - S(kBarH), client.right, client.bottom - S(kBarH));

    // Page content.
    for (const PCard& c : g_cards) {
        if (c.page != g_page) continue;
        FillRound(g, c.r, kCard, kBorder, 6.0f);
        RECT tr = { c.r.left + S(20), c.r.top + S(10), c.r.left + S(400), c.r.top + S(28) };
        Txt(g, dc, c.title, tr, g_sectionFont, kAccentHi, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    for (const PText& l : g_lines)
        if (l.page == g_page) g.DrawLine(&line, l.r.left, l.r.top, l.r.right, l.r.top);
    g.Flush(FlushIntentionSync);
    for (const PText& d : g_descs)
        if (d.page == g_page) DrawTextIn(dc, d.text, d.r, g_descFont, kDim, DT_LEFT | DT_WORDBREAK);

    if (g_page == PG_PLAY) {
        PaintBanner(dc, g);
        PaintSummary(dlg, dc, g);
        const RECT h = g_hintRect;
        DrawTextIn(dc, L"In game:   F9  original 60 fps      F8  next FPS preset", h, g_uiFont, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    } else if (g_page == PG_DISP) {
        PaintShape(dc, g);
    } else if (g_page == PG_LOG) {
        FillRound(g, g_logCard, kField, kBorder, 6.0f);
        Txt(g, dc, L"hfblade.log  \x00B7  last 200 lines", g_logTitle, g_sectionFont, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
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
    const REAL x = REAL(rc.right) - w - 1, y = REAL(rc.top) + (REAL(rc.bottom - rc.top) - h) / 2;
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
    tr.right = LONG(x) - S(12);
    DrawTextIn(dc, text, tr, g_uiFont, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void DrawButton(const DRAWITEMSTRUCT* di)
{
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    const bool nav = di->CtlID >= IDC_NAV0 && di->CtlID < IDC_NAV0 + PG_COUNT;
    FillRect(dc, &rc, nav ? g_sideBrush : g_bgBrush);
    const bool hot = di->hwndItem == g_hot, down = (di->itemState & ODS_SELECTED) != 0;
    Graphics* gp = new Graphics(dc);
    Graphics& g = *gp;
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    if (nav) {   // sidebar row: icon + label; active row has a tinted background and an accent bar on the left edge
        const int idx = int(di->CtlID - IDC_NAV0);
        const bool active = idx == g_page;
        if (active || hot) {
            SolidBrush bgb(active ? C(kAccent, 30) : C(RGB(255, 255, 255), 12));
            g.FillRectangle(&bgb, REAL(rc.left), REAL(rc.top), REAL(rc.right - rc.left), REAL(rc.bottom - rc.top));
        }
        if (active) {
            SolidBrush b(C(kAccent));
            g.FillRectangle(&b, REAL(rc.left), REAL(rc.top), REAL(S(3)), REAL(rc.bottom - rc.top));
        }
        const REAL is = REAL(S(20));
        DrawNavIcon(g, idx, REAL(rc.left + S(26)), REAL(rc.top) + (REAL(rc.bottom - rc.top) - is) / 2, is, active ? kAccentHi : hot ? kText : kMuted);
        delete gp;
        RECT tr = rc;
        tr.left += S(62);
        DrawTextIn(dc, kPageNames[idx], tr, g_navFont, active || hot ? kText : kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
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

    const bool primary = di->CtlID == IDC_PLAY || di->CtlID == IDC_PLAYBIG, preview = di->CtlID == IDC_PREVIEW;
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
    DrawTextIn(dc, primary ? L"\x25B6  SAVE & PLAY" : preview ? L"\x25B6" : text, rc,
               di->CtlID == IDC_PLAYBIG ? g_playFont : g_btnFont,
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
    wchar_t text[96];
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    RECT r = di->rcItem;
    r.left += S(10);
    DrawTextIn(di->hDC, text, r, g_uiFont, sel ? RGB(255, 255, 255) : kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

static void PaintCombo(HWND h, HDC dc)
{
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, g_cardBrush);
    const bool hot = h == g_hot, open = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0, focus = GetFocus() == h;
    {   // GDI+ first, GDI text after (see DrawToggle)
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath p;
        AddRoundRect(p, REAL(rc.left) + 0.5f, REAL(rc.top) + 0.5f, REAL(rc.right - rc.left - 1), REAL(rc.bottom - rc.top - 1), 5.0f);
        SolidBrush fill(C(hot || open ? kFieldHi : kField));
        g.FillPath(&fill, &p);
        Pen border(C(open || focus ? kAccent : hot ? RGB(0x4A, 0x4E, 0x57) : kBorder), 1.0f);
        g.DrawPath(&border, &p);
        const REAL cx = REAL(rc.right) - S(14), cy = REAL(rc.bottom) / 2, s = REAL(S(4));
        Pen chev(C(open ? kAccentHi : kMuted), 1.6f);
        chev.SetStartCap(LineCapRound); chev.SetEndCap(LineCapRound);
        const REAL dir = open ? -1.0f : 1.0f;
        g.DrawLine(&chev, cx - s, cy - s * 0.5f * dir, cx, cy + s * 0.5f * dir);
        g.DrawLine(&chev, cx, cy + s * 0.5f * dir, cx + s, cy - s * 0.5f * dir);
    }

    const int i = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (i >= 0) {
        wchar_t text[96];
        SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)text);
        RECT t = rc;
        t.left += S(12);
        t.right -= S(28);
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

// Dark scrollbars in the combo drop-downs and the log, rounded corners and a dark border for the window.
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
    HWND log = GetDlgItem(dlg, IDC_LOGVIEW);
    if (allow) allow(log, TRUE);
    SetWindowTheme(log, L"DarkMode_Explorer", nullptr);
    const DWORD round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(dlg, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    const COLORREF border = kBorder;
    DwmSetWindowAttribute(dlg, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
}

static bool IsToggle(int id) { return id == IDC_HFR || id == IDC_QTE || id == IDC_BLADE || id == IDC_VERTPLUS || id == IDC_PHOTO || id == IDC_WEAPONSW || id == IDC_SOUND; }
static bool IsCombo(int id) { return id == IDC_FPS || id == IDC_MENUFPS || id == IDC_KEEPCUTS || id == IDC_KEEPBODIES || id == IDC_INTRO || id == IDC_RES; }
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
    WriteInt(L"PhotoMode", L"Enabled", cfg::kPhotoMode);
    WriteInt(L"PhotoMode", L"Key", cfg::kPhotoKey);
    WriteInt(L"PhotoMode", L"SpeedCmPerSec", cfg::kPhotoSpeed);
    WriteInt(L"WeaponSwitch", L"Enabled", cfg::kWeaponSwitch);
    WriteInt(L"WeaponSwitch", L"PadButton", cfg::kWeaponSwitchPad);
    WriteInt(L"WeaponSwitch", L"Key", cfg::kWeaponSwitchKey);
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
    SetToggle(dlg, IDC_WEAPONSW, ReadInt(L"WeaponSwitch", L"Enabled", cfg::kWeaponSwitch) != 0);
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
    SetToggle(dlg, IDC_PHOTO, ReadInt(L"PhotoMode", L"Enabled", cfg::kPhotoMode) != 0);
    g_photoKey = ReadInt(L"PhotoMode", L"Key", cfg::kPhotoKey);

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
    WriteInt(L"WeaponSwitch", L"Enabled", GetToggle(dlg, IDC_WEAPONSW));
    WriteInt(L"Fixes", L"QteEase", (int)SendDlgItemMessageW(dlg, IDC_EASE, TBM_GETPOS, 0, 0));
    const int res = SelectedValue(GetDlgItem(dlg, IDC_RES));
    WriteInt(L"Display", L"Width", res ? (res >> 16) & 0xFFFF : 1920);
    WriteInt(L"Display", L"Height", res & 0xFFFF);
    // Older mod builds only read Height1920 (width fixed at 1920); keep it in step.
    WriteInt(L"Display", L"Height1920", res && ((res >> 16) & 0xFFFF) == 1920 ? (res & 0xFFFF) : 0);
    WriteInt(L"Display", L"VertPlusFov", GetToggle(dlg, IDC_VERTPLUS));
    WriteInt(L"PhotoMode", L"Enabled", GetToggle(dlg, IDC_PHOTO));
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

// Log page: the last ~200 lines of hfblade.log (the game may be writing it, so open it shared).
static void LoadLog(HWND dlg)
{
    HWND ed = GetDlgItem(dlg, IDC_LOGVIEW);
    wchar_t path[MAX_PATH];
    InDir(path, MAX_PATH, L"hfblade.log");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { SetWindowTextW(ed, L"No log yet - start the game once."); return; }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    const LONGLONG maxBytes = 256 * 1024;
    const LONGLONG start = size.QuadPart > maxBytes ? size.QuadPart - maxBytes : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = start;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::string buf(size_t(size.QuadPart - start), '\0');
    DWORD got = 0;
    if (!buf.empty()) ReadFile(f, &buf[0], (DWORD)buf.size(), &got, nullptr);
    CloseHandle(f);
    buf.resize(got);

    size_t b = 0, e = buf.size();
    if (start > 0) { const size_t nl = buf.find('\n'); b = nl == std::string::npos ? buf.size() : nl + 1; }   // drop a cut-off first line
    while (e > b && (buf[e - 1] == '\n' || buf[e - 1] == '\r')) --e;
    size_t s = e;
    for (int lines = 1; s > b; --s)
        if (buf[s - 1] == '\n' && lines++ == 200) break;
    const std::string part = buf.substr(s, e - s);
    if (part.empty()) { SetWindowTextW(ed, L"The log is empty."); return; }

    std::wstring wide;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, part.data(), (int)part.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(cp, 0, part.data(), (int)part.size(), nullptr, 0); }
    wide.resize(size_t(n > 0 ? n : 0));
    if (n > 0) MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, part.data(), (int)part.size(), &wide[0], n);
    std::wstring out;
    out.reserve(wide.size() + wide.size() / 20);
    for (wchar_t ch : wide) {
        if (ch == L'\r') continue;
        if (ch == L'\n') out += L'\r';
        out += ch;
    }
    SendMessageW(ed, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    SetWindowTextW(ed, out.c_str());
    SendMessageW(ed, EM_SETSEL, (WPARAM)out.size(), (LPARAM)out.size());
    SendMessageW(ed, EM_SCROLLCARET, 0, 0);
    SendMessageW(ed, WM_VSCROLL, SB_BOTTOM, 0);
}

static void UpdateStatus(HWND dlg)
{
    SetDlgItemTextW(dlg, IDC_HINT, g_dirty ? L"\x25CF  Unsaved changes" : L"\x2713  Settings saved");
}

static void MarkDirty(HWND dlg)
{
    if (g_dirty) return;
    g_dirty = true;
    UpdateStatus(dlg);
}

static void ShowPage(HWND dlg, int page)
{
    g_page = page;
    for (int p = 0; p < PG_COUNT; ++p)
        for (int id : g_pageIds[p]) ShowWindow(GetDlgItem(dlg, id), p == page ? SW_SHOW : SW_HIDE);
    if (page == PG_LOG) LoadLog(dlg);
    RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
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
    if (g_page == PG_DISP) InvalidateRect(dlg, &g_shapeBox, FALSE), InvalidateRect(dlg, &g_shapeCap, FALSE);
}

// Hover descriptions. Shown only when the pointer rests on a control.
static void AddTooltips(HWND dlg)
{
    HWND tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, dlg, nullptr, nullptr, nullptr);
    if (!tt) return;
    SetWindowTheme(tt, L"DarkMode_Explorer", nullptr);
    SendMessageW(tt, TTM_SETMAXTIPWIDTH, 0, S(320));
    SendMessageW(tt, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SendMessageW(tt, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    auto add = [&](int id, const wchar_t* text) {
        TOOLINFOW ti = { sizeof ti };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = dlg;
        ti.uId = (UINT_PTR)GetDlgItem(dlg, id);
        ti.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    };
    for (const Tip& t : kTips) {
        add(t.id, t.text);
        if (t.label) add(t.label, t.text);
    }
}

static const UINT WM_APP_INTRO = WM_APP + 1;
static bool g_dumpIntro = false;
static int  g_introMode = 2;

static void ApplySound(HWND dlg, bool on)
{
    g_soundOn = on;
    WriteInt(L"Launcher", L"Sound", on ? 1 : 0);
    if (!on) Audio_StopAll();
    SetToggle(dlg, IDC_SOUND, on);
    InvalidateRect(GetDlgItem(dlg, IDC_MUTE), nullptr, FALSE);
    SetDlgItemTextW(dlg, IDC_HINT, on ? L"\x266A  Sound on" : L"Sound off");
    SetTimer(dlg, 1, 1500, nullptr);
}

// Window size: the design size scaled by DPI, shrunk if the work area is too small; centred.
static void SizeWindow(HWND dlg)
{
    float sc = float(GetDpiForWindow(dlg)) / 96.0f;
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    const float fit = fminf(float(wa.right - wa.left - 24) / float(kWinW), float(wa.bottom - wa.top - 24) / float(kWinH));
    if (fit < sc) sc = fit;
    g_scale = sc < 0.5f ? 0.5f : sc;
    const int w = S(kWinW), h = S(kWinH);
    SetWindowPos(dlg, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mi->CtlType == ODT_COMBOBOX) {
            mi->itemHeight = UINT((int)mi->itemID == -1 ? S(20) : S(26));
            return TRUE;
        }
        break;
    }
    case WM_INITDIALOG: {
        g_dlg = dlg;
        SizeWindow(dlg);
        g_bgBrush = CreateSolidBrush(kBg);
        g_cardBrush = CreateSolidBrush(kCard);
        g_accentBrush = CreateSolidBrush(kAccent);
        g_sideBrush = CreateSolidBrush(kSide);
        g_fieldBrush = CreateSolidBrush(kField);
        g_titleFont = MakeFont(22, FW_BOLD, L"Segoe UI");
        g_bigTitleFont = MakeFont(30, FW_BOLD, L"Segoe UI");
        g_tagFont = MakeFont(11, FW_NORMAL, L"Segoe UI");
        g_subFont = MakeFont(9, FW_NORMAL, L"Segoe UI");
        g_sectionFont = MakeFont(8, FW_BOLD, L"Segoe UI");
        g_uiFont = MakeFont(10, FW_NORMAL, L"Segoe UI");
        g_btnFont = MakeFont(10, FW_SEMIBOLD, L"Segoe UI");
        g_descFont = MakeFont(8, FW_NORMAL, L"Segoe UI");
        g_navFont = MakeFont(11, FW_SEMIBOLD, L"Segoe UI");
        g_valueFont = MakeFont(18, FW_SEMIBOLD, L"Segoe UI");
        g_playFont = MakeFont(14, FW_BOLD, L"Segoe UI");
        g_monoFont = MakeFont(9, FW_NORMAL, L"Consolas");
        const int owner[] = { IDC_HFR, IDC_QTE, IDC_BLADE, IDC_VERTPLUS, IDC_PLAY, IDC_PLAYBIG, IDC_SAVE, IDC_XMIN, IDC_XCLOSE, IDC_PREVIEW, IDC_MUTE,
                              IDC_PHOTO, IDC_WEAPONSW, IDC_SOUND, IDC_LOGREFRESH, IDC_LOGFOLDER };
        for (int id : owner) SetWindowSubclass(GetDlgItem(dlg, id), HoverProc, 1, 0);
        for (int i = 0; i < PG_COUNT; ++i) SetWindowSubclass(GetDlgItem(dlg, IDC_NAV0 + i), HoverProc, 1, 0);
        const int statics[] = { IDC_L_FPS, IDC_L_MENU, IDC_L_EASE, IDC_L_PIECES, IDC_L_BODIES, IDC_L_INTRO, IDC_L_RES, IDC_RESINFO, IDC_RESNOTE,
                                IDC_PHOTOPAD, IDC_PHOTOPAD2, IDC_PHOTOKB, IDC_PHOTOKB2, IDC_HINT, IDC_WARN };
        for (int id : statics) SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)g_uiFont, FALSE);
        SendDlgItemMessageW(dlg, IDC_EASEVAL, WM_SETFONT, (WPARAM)g_btnFont, FALSE);
        SendDlgItemMessageW(dlg, IDC_LOGVIEW, WM_SETFONT, (WPARAM)g_monoFont, FALSE);
        const int combos[] = { IDC_FPS, IDC_MENUFPS, IDC_KEEPCUTS, IDC_KEEPBODIES, IDC_INTRO, IDC_RES };
        for (int id : combos) {
            SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)g_uiFont, FALSE);
            SendDlgItemMessageW(dlg, id, CB_SETITEMHEIGHT, (WPARAM)-1, S(20));
            SendDlgItemMessageW(dlg, id, CB_SETITEMHEIGHT, 0, S(26));
        }
        SetWindowTextW(dlg, L"HF Blade");
        if (HICON big = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0)) {
            SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)big);
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        }
        BuildLayout(dlg);
        ApplyWindowTheme(dlg);
        Load(dlg);
        UpdateResInfo(dlg);
        AddTooltips(dlg);
        g_soundOn = ReadInt(L"Launcher", L"Sound", cfg::kSound) != 0;
        SetToggle(dlg, IDC_SOUND, g_soundOn);
        UpdateStatus(dlg);
        ShowPage(dlg, PG_PLAY);
        // Start invisible; the intro reveals the window.
        SetWindowLongPtrW(dlg, GWL_EXSTYLE, GetWindowLongPtrW(dlg, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(dlg, 0, 0, LWA_ALPHA);
        g_introMode = ReadInt(L"Launcher", L"Intro", cfg::kIntro);
        PostMessageW(dlg, WM_APP_INTRO, 0, 0);
        SetFocus(GetDlgItem(dlg, IDC_PLAYBIG));
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
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        const int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_LOGVIEW) {
            SetBkColor(dc, kField);
            SetTextColor(dc, kMuted);
            return (INT_PTR)g_fieldBrush;
        }
        SetBkMode(dc, OPAQUE);
        const bool footer = id == IDC_HINT || id == IDC_WARN;
        SetBkColor(dc, footer ? kBg : kCard);
        SetTextColor(dc, id == IDC_WARN ? kWarn
                         : id == IDC_HINT ? (g_dirty ? kWarn : kMuted)
                         : id == IDC_RESNOTE || id == IDC_PHOTOPAD2 || id == IDC_PHOTOKB2 ? kMuted
                         : id == IDC_EASEVAL || id == IDC_RESINFO ? kAccentHi : kText);
        SelectObject(dc, id == IDC_EASEVAL ? g_btnFont : g_uiFont);
        return (INT_PTR)(footer ? g_bgBrush : g_cardBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kText);
        SetBkColor(dc, kField);
        return (INT_PTR)g_fieldBrush;
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
        MarkDirty(dlg);
        return TRUE;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(dlg, 1);
            UpdateStatus(dlg);
        } else if (wp == 2) {
            const DWORD el = GetTickCount() - g_intro.start;
            if (el >= g_intro.durMs) EndIntro();
            else PresentIntroFrame(REAL(el));
        }
        return TRUE;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (IsToggle(id) && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
            if (id == IDC_SOUND) ApplySound(dlg, !g_soundOn);
            else { SetToggle(dlg, id, !GetToggle(dlg, id)); MarkDirty(dlg); }
            return TRUE;
        }
        if (IsCombo(id) && (code == CBN_SELCHANGE || code == CBN_DROPDOWN || code == CBN_CLOSEUP)) {
            if (code == CBN_SELCHANGE) {
                if (id == IDC_RES) UpdateResInfo(dlg);
                MarkDirty(dlg);
            }
            InvalidateRect((HWND)lp, nullptr, FALSE);
            return TRUE;
        }
        if (id >= IDC_NAV0 && id < IDC_NAV0 + PG_COUNT) {
            if (code == BN_CLICKED) ShowPage(dlg, id - IDC_NAV0);
            return TRUE;
        }
        if (code != BN_CLICKED && code != BN_DOUBLECLICKED) break;
        switch (id) {
        case IDOK:
        case IDC_PLAY:
        case IDC_PLAYBIG:
            Save(dlg);
            g_dirty = false;
            UpdateStatus(dlg);
            if (Launch(dlg)) EndDialog(dlg, IDC_PLAY);
            return TRUE;
        case IDC_SAVE:
            Save(dlg);
            g_dirty = false;
            UpdateStatus(dlg);
            return TRUE;
        case IDC_MUTE:
            ApplySound(dlg, !g_soundOn);
            return TRUE;
        case IDC_PREVIEW:
            StartIntro(dlg, SelectedValue(GetDlgItem(dlg, IDC_INTRO)), false);
            return TRUE;
        case IDC_LOGREFRESH:
            LoadLog(dlg);
            return TRUE;
        case IDC_LOGFOLDER:
            ShellExecuteW(dlg, L"open", g_dir, nullptr, nullptr, SW_SHOWNORMAL);
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
    delete g_bannerBmp;
    GdiplusShutdown(gdiplus);
    return 0;
}
