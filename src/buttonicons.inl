// ---------------------------------------------------------------------------
// PlayStation button icons ([Display] ButtonIcons=1). See docs/RESEARCH.md section 23.
// Included into dllmain.cpp after the patching helpers; uses Log/At/WriteJmp/g_delta.
//
// The game creates its 2D textures through one loader (0xf97120), which calls
// D3DXCreateTextureFromFileInMemoryEx (call at 0xf97182) through the import thunk 0x1436f9e (JMP [0x15fd378]).
// The thunk is redirected here. When the source is the button atlas (512x512 DXT5, recognised by a
// hash of its data: the PC layout used by ui_core*.dtt and the DLC 3 layout), the texture is created
// uncompressed instead, and the Xbox icons' rectangles are overwritten with HF Blade's own
// PlayStation icons (src/buttonicons.h, drawn by tools/make_button_icons.py). Every other texture
// passes through untouched. The game's files are not changed.
// ---------------------------------------------------------------------------
#include <d3d9.h>
#include "buttonicons.h"

static const uintptr_t VA_TEX_THUNK = 0x01436F9E;   // FF 25 <iat>: jmp [D3DXCreateTextureFromFileInMemoryEx]
static const uintptr_t VA_TEX_IAT   = 0x015FD378;

typedef HRESULT (WINAPI* TexFromMemExFn)(IDirect3DDevice9*, const void*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
                                         D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);
typedef HRESULT (WINAPI* FilterTexFn)(IDirect3DBaseTexture9*, const PALETTEENTRY*, UINT, DWORD);
static FilterTexFn g_btnFilter = nullptr;
static int g_btnLogged = 0;

static uint32_t Fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static void BtnPaint(IDirect3DTexture9* tex, const BtnIconRect* rects, size_t n)
{
    // The colour channels matter too: the QTE highlight tints the icon's colour (not only its alpha), so
    // transparent pixels must be black as in the game's own icons, or the whole rectangle lights up.
    // Grey = 255 * sqrt(alpha), close to the game's soft edges (alpha 55 -> ~140, 128 -> ~165).
    static uint32_t lut[256];
    if (!lut[255])
        for (int v = 0; v < 256; ++v) {
            const uint32_t g = uint32_t(sqrtf(v / 255.0f) * 255.0f + 0.5f);
            lut[v] = (uint32_t(v) << 24) | (g << 16) | (g << 8) | g;
        }
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) { Log("Button icons: could not lock the atlas."); return; }
    for (size_t i = 0; i < n; ++i) {
        const BtnIconRect& r = rects[i];
        const uint8_t* a = kBtnAlpha + r.offset;
        for (int y = 0; y < r.h; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + (r.y + y) * lr.Pitch) + r.x;
            for (int x = 0; x < r.w; ++x) row[x] = lut[a[y * r.w + x]];
        }
    }
    tex->UnlockRect(0);
    if (tex->GetLevelCount() > 1) {
        if (!g_btnFilter) {
            HMODULE dx = GetModuleHandleA("d3dx9_43.dll");
            if (dx) g_btnFilter = reinterpret_cast<FilterTexFn>(GetProcAddress(dx, "D3DXFilterTexture"));
        }
        if (g_btnFilter) g_btnFilter(tex, nullptr, 0, 0xFFFFFFFFu);   // D3DX_DEFAULT: rebuild the mip levels
    }
}

static HRESULT WINAPI ButtonTexHook(IDirect3DDevice9* dev, const void* src, UINT size, UINT w, UINT h, UINT mips,
                                    DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipFilter,
                                    D3DCOLOR key, void* info, PALETTEENTRY* pal, IDirect3DTexture9** out)
{
    // The import is resolved by the time the game loads textures; read it then.
    const TexFromMemExFn orig = *At<TexFromMemExFn>(VA_TEX_IAT);
    const uint8_t* b = static_cast<const uint8_t*>(src);
    const BtnIconRect* rects = nullptr;
    size_t n = 0;
    const char* which = "";
    if (b && size >= 0x40080 && memcmp(b, "DDS ", 4) == 0 && *reinterpret_cast<const uint32_t*>(b + 12) == 512 &&
        *reinterpret_cast<const uint32_t*>(b + 16) == 512 && memcmp(b + 84, "DXT5", 4) == 0 && pool == D3DPOOL_MANAGED) {
        const uint32_t hash = Fnv1a(b, 0x40080);
        if (hash == kBtnAtlasHashPc) { rects = kBtnRectsPc; n = _countof(kBtnRectsPc); which = "PC"; }
        else if (hash == kBtnAtlasHashDlc3) { rects = kBtnRectsDlc3; n = _countof(kBtnRectsDlc3); which = "DLC 3"; }
    }
    if (!rects) return orig(dev, src, size, w, h, mips, usage, fmt, pool, filter, mipFilter, key, info, pal, out);

    HRESULT hr = orig(dev, src, size, w, h, mips, usage, D3DFMT_A8R8G8B8, pool, filter, mipFilter, key, info, pal, out);
    if (FAILED(hr) || !out || !*out) {
        Log("Button icons: could not create the %s atlas uncompressed (0x%08lX); Xbox icons kept.", which, hr);
        return orig(dev, src, size, w, h, mips, usage, fmt, pool, filter, mipFilter, key, info, pal, out);
    }
    BtnPaint(*out, rects, n);
    if (g_btnLogged++ < 8) Log("Button icons: PlayStation icons applied to the %s button atlas.", which);
    return hr;
}

static void InstallButtonIcons(const char* ini)
{
    if (GetPrivateProfileIntA("Display", "ButtonIcons", cfg::kButtonIcons, ini) != 1) return;
    const uint8_t* p = At<uint8_t>(VA_TEX_THUNK);
    if (p[0] != 0xFF || p[1] != 0x25 || *reinterpret_cast<const uint32_t*>(p + 2) != VA_TEX_IAT + g_delta) {
        Log("Signature mismatch at TexThunk (0x%08X). Button icons not available.", (unsigned)(VA_TEX_THUNK + g_delta));
        return;
    }
    WriteJmp(VA_TEX_THUNK, &ButtonTexHook, 6);
    Log("Button icons: PlayStation.");
}
