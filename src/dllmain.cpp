// HF Blade: remaster-style enhancement mod for METAL GEAR RISING: REVENGEANCE (Steam, x86).
// Loaded as a dinput8.dll proxy. All patches are applied in memory; game files are untouched.
// Addresses and rationale: docs/RESEARCH.md. MIT licensed.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Target build (SHA-256 7174741f46ab4c22...ad34). Addresses assume image base 0x400000
// and are rebased at runtime if the exe is relocated.
// ---------------------------------------------------------------------------
static const uintptr_t kPreferredBase = 0x00400000;
static const DWORD     kTimeDateStamp = 0x52E76F3A;

static const uintptr_t VA_LIMITER      = 0x00F98070; // Present + Sleep-based limiter (void(void))
static const uintptr_t VA_PRESENT      = 0x00F97F90; // Present wrapper (int(void))
static const uintptr_t VA_CLAMP_SITE   = 0x00E03A07; // FLD1 ; FCOM [ESI+0x7C]  (min clamp of frame multiplier)
static const uintptr_t VA_CLAMP_RESUME = 0x00E03A0C; // FNSTSW AX
static const uintptr_t VA_RATE_MODE    = 0x01F206D0; // int: 1 = 30 fps mode, else 60 fps
static const uintptr_t VA_FRAME_MULT   = 0x01BE942C; // float: frame multiplier in 60 fps units
static const uintptr_t VA_MULT_SCALE   = 0x01BE9438; // float: ms -> multiplier scale
static const uintptr_t VA_FIXED_SITE   = 0x00E039A9; // fixed-multiplier path: FSTP ST0; FSTP ST1; FLD [ESP+0x10]
static const uintptr_t VA_FIXED_RESUME = 0x00E039B1; // FSTP [ESI+0x7C]
static const uintptr_t VA_FLAGS_060    = 0x01BEA060; // main-loop state flags that select 30 fps / fixed timing
static const uintptr_t VA_FLAGS_064    = 0x01BEA064;
static const uintptr_t VA_FLAGS_084    = 0x01BEA084;

// Display (16:10) patch sites. See docs/RESEARCH.md section 11.
static const uintptr_t VA_RES_TABLE_H  = 0x016EBF1C; // .rdata float 1080.0: mode-table entry 0 height
static const uintptr_t VA_RES_IDX5_H   = 0x00A30EB9; // MOV EAX,0x438 (index 5 = "1920x1080" height)
static const uintptr_t VA_CAM_ASPECT   = 0x016C3078; // .rdata float 16/9 used by the engine camera
static const uintptr_t VA_LETTERBOX[]  = { 0x00A3A8CE, 0x00A3A868, 0x00F9B9FC, 0x00F9B9A9, 0x00F9D5C6, 0x00F9D56F };
static const uint8_t   kLetterboxOrig[][2] = { {0x2B,0xC7}, {0x2B,0xC7}, {0x2B,0xC6}, {0x2B,0xC6}, {0x2B,0xC6}, {0x2B,0xC6} };
static const uint8_t   kXorEaxEax[]    = { 0x31, 0xC0 };
static const uint8_t   kResTableHOrig[] = { 0x00, 0x00, 0x87, 0x44 };          // 1080.0f
static const uint8_t   kResIdx5Orig[]  = { 0xB8, 0x38, 0x04, 0x00, 0x00 };     // MOV EAX,1080
static const uintptr_t VA_RES_TABLE_W  = 0x016EBF20; // .rdata float 1920.0: mode-table entry 0 width
static const uintptr_t VA_RES_IDX5_W   = 0x00A30EAA; // MOV EAX,0x780 (index 5 = "1920x1080" width)
static const uint8_t   kResTableWOrig[] = { 0x00, 0x00, 0xF0, 0x44 };          // 1920.0f
static const uint8_t   kResIdx5WOrig[] = { 0xB8, 0x80, 0x07, 0x00, 0x00 };     // MOV EAX,1920
// The two perspective calls of the 3D UI layer (its eye distance assumes a 1080-tall screen).
static const uintptr_t VA_PROJ_UI_CALLS[] = { 0x00CACB8D, 0x00CACC2D };
static const uint8_t   kCamAspectOrig[] = { 0x39, 0x8E, 0xE3, 0x3F };          // 1.7777778f
// Perspective builder (cdecl: out, vertical fov rad, aspect, near, far, 0, 0) and its 9 call sites.
static const uintptr_t VA_PROJ         = 0x00DDCCC0;
static const uintptr_t VA_PROJ_CALLS[] = { 0x00985F6C, 0x00A28128, 0x00A460F6, 0x00CACB8D, 0x00CACC2D,
                                           0x00D1E43E, 0x00D1E47C, 0x00DE5B18, 0x00F43CFB };

// QTE fixes. See docs/RESEARCH.md section 12.
// Mash-speed gauges: "FSUB dword [0x0163CCE0]" (0.01 per update) -> FSUB [g_qteDecay] (0.01 * mult).
static const uintptr_t VA_QTE_FSUB[] = { 0x005AC120, 0x005AC5B3, 0x005AEEA5, 0x00840AC9, 0x008410FA };
static const uintptr_t VA_QTE_DECAY_CONST = 0x0163CCE0;
// Integer gauge / timeout counters: 6-byte DEC/INC [ESI+disp32] -> CALL cave; NOP.
static const uintptr_t VA_QTE_DEC948  = 0x006127B7; // DEC dword [ESI+0x948]
static const uintptr_t VA_QTE_INC94C[] = { 0x00612782, 0x00611BA4 }; // INC dword [ESI+0x94C]
static const uint8_t   kDec948Orig[] = { 0xFF, 0x8E, 0x48, 0x09, 0x00, 0x00 };
static const uint8_t   kInc94COrig[] = { 0xFF, 0x86, 0x4C, 0x09, 0x00, 0x00 };
// Sundowner (Em0310) shield tug-of-war: press frame g += dt*[esi+0x938], else g -= dt*[esi+0x93C].
// A press is one frame, so at 120 fps each press adds half as much. Press: /mult. Decay: *ease.
static const uintptr_t VA_TUG_PRESS = 0x00584C3D;   // FMUL dword [ESI+0x938]
static const uintptr_t VA_TUG_DECAY = 0x00584C4B;   // FMUL dword [ESI+0x93C]
static const uint8_t   kTugPressOrig[] = { 0xD8, 0x8E, 0x38, 0x09, 0x00, 0x00 };
static const uint8_t   kTugDecayOrig[] = { 0xD8, 0x8E, 0x3C, 0x09, 0x00, 0x00 };

// Blade Mode fixes. See docs/RESEARCH.md section 13.
// "X / getRate()" sites meant to cancel world slow-mo; getRate() includes the frame multiplier,
// so the division also cancelled it. Redirect the CALL to a stub returning getRate()/mult.
static const uintptr_t VA_GET_RATE = 0x00E049B0;   // thiscall, no stack args, returns float in ST0
static const uintptr_t VA_RATE_DIV_CALLS[] = {
    0x005CC9B8, 0x005CD009, 0x005CD063, 0x005CD1DE, 0x005CD35E, 0x005CD4DF, 0x005CD64E, 0x005CD7CE, 0x005CDDE8,
    0x005CDF77, 0x005CE193, 0x005CE67E, 0x005CE7FE, 0x005CE97D, 0x005CEBFD, 0x008CE295, 0x00AE44C7 };
// Slow-mo ramp lerps "inter += (target - inter) * k" applied per frame: FMUL dword [ESI+0x3430].
static const uintptr_t VA_LERP_WORLD = 0x00BC285A, VA_LERP_WORLD_RESUME = 0x00BC2860;
static const uintptr_t VA_LERP_PLAYER = 0x00BC287C, VA_LERP_PLAYER_RESUME = 0x00BC2882;
static const uint8_t   kLerpOrig[] = { 0xD8, 0x8E, 0x30, 0x34, 0x00, 0x00 };
// Blade Mode energy drain: every Blade Mode frame the player subtracts "coef() * [ESI+0x338C]" from the
// fuel cells (0xbc3000) without any time step, so the energy bar empties twice as fast at 120 fps.
// Raiden 0xbf256e, plus the same code for the two DLC characters (Sam, Blade Wolf).
static const uintptr_t VA_BLADE_DRAIN[] = { 0x00BF256E, 0x00877147, 0x008B8457 }; // FMUL dword [ESI+0x338C]
static const uint8_t   kBladeDrainOrig[] = { 0xD8, 0x8E, 0x8C, 0x33, 0x00, 0x00 };

static const uint8_t kLimiterOrig[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
static const uint8_t kClampOrig[]   = { 0xD9, 0xE8, 0xD8, 0x56, 0x7C };
static const uint8_t kFixedOrig[]   = { 0xDD, 0xD8, 0xDD, 0xD9, 0xD9, 0x44, 0x24, 0x10 };
// Present wrapper starts with "MOV EAX, [0x01F206D4]". The exe is ASLR-enabled, so the
// absolute operand is relocated at load time and is checked as (0x01F206D4 + delta).
static const uint8_t   kPresentOpcode  = 0xA1;
static const uintptr_t kPresentOperand = 0x01F206D4;

// ---------------------------------------------------------------------------
// Config / state
// ---------------------------------------------------------------------------
static uintptr_t g_delta = 0;
static int       g_targetFps = 120;
static int       g_menuFps = 120;
static int       g_toggleKey = VK_F9;
static int       g_presetKey = cfg::kPresetKey;
static bool      g_menuFollows = true;   // MenuFPS=0: menus use TargetFPS
static char      g_ini[MAX_PATH];
static int       g_logStats = 1;
static bool      g_hfrOn = true;
static char      g_dir[MAX_PATH];

extern "C" {
    float     g_minMult = 1.0f;     // read by the naked clamp hook
    float     g_fixedScale = 1.0f;  // read by the naked fixed-path hook
    uintptr_t g_clampResume = 0;
    uintptr_t g_fixedResume = 0;
    float     g_qteDecay = 0.01f;   // 0.01 per 60 fps frame, scaled by the frame multiplier (and ease)
    int32_t   g_ticks60 = 1;        // whole 60 Hz ticks elapsed this frame (0/1 at 120 fps)
    int32_t   g_ticksDecay = 1;     // same, scaled by g_qteEase (mash-gauge decrement)
    float     g_easeNow = 1.0f;     // g_qteEase while HFR is on, else 1
}
static float g_qteEase = 1.0f;      // [Fixes] QteEase / 100: fraction of the original gauge decay kept in HFR

// Called once per frame. The game computes the multiplier at the start of the next frame;
// using the latest value is accurate to within one frame.
static void UpdateFrameScalars()
{
    static double acc = 0.0, accDecay = 0.0;
    const float mult = *reinterpret_cast<float*>(0x01BE942C + g_delta);
    const float ease = g_hfrOn ? g_qteEase : 1.0f;
    g_easeNow = ease;
    g_qteDecay = 0.01f * mult * ease;
    acc += mult;
    const int ticks = int(acc);
    acc -= ticks;
    g_ticks60 = ticks;
    accDecay += double(mult) * ease;
    const int decayTicks = int(accDecay);
    accDecay -= decayTicks;
    g_ticksDecay = decayTicks;
}

extern "C" {
    uintptr_t g_multPtr = 0;        // relocated address of the game's frame multiplier
    uintptr_t g_getRateAddr = 0;
    uintptr_t g_lerpWorldResume = 0, g_lerpPlayerResume = 0;
}

// Same contract as getRate (ECX = handle, result in ST0, EAX clobbered) but returns rate / mult.
extern "C" __declspec(naked) void GetRateNoMult()
{
    __asm {
        call  dword ptr [g_getRateAddr]
        mov   eax, dword ptr [g_multPtr]
        cmp   dword ptr [eax], 0        // multiplier not initialised yet (first frame): leave as is
        je    done
        fdiv  dword ptr [eax]
    done:
        ret
    }
}

extern "C" __declspec(naked) void LerpWorldHook()
{
    __asm {
        fmul  dword ptr [esi + 0x3430]
        push  eax
        mov   eax, dword ptr [g_multPtr]
        fmul  dword ptr [eax]
        pop   eax
        jmp   dword ptr [g_lerpWorldResume]
    }
}

extern "C" __declspec(naked) void LerpPlayerHook()
{
    __asm {
        fmul  dword ptr [esi + 0x3430]
        push  eax
        mov   eax, dword ptr [g_multPtr]
        fmul  dword ptr [eax]
        pop   eax
        jmp   dword ptr [g_lerpPlayerResume]
    }
}

// Replaces "FMUL dword [ESI+0x338C]" (CALL + NOP). Scales the per-frame drain by min(mult, 1):
// unchanged at 60 fps and below, real-time above 60 fps.
extern "C" __declspec(naked) void BladeDrainHook()
{
    __asm {
        fmul    dword ptr [esi + 0x338C]
        push    eax
        mov     eax, dword ptr [g_multPtr]
        fld     dword ptr [eax]         // st0 = mult, st1 = drain
        fld1                            // st0 = 1, st1 = mult
        fcomi   st, st(1)
        fcmovnb st, st(1)               // 1 >= mult -> use mult
        fstp    st(1)                   // st0 = min(mult, 1), st1 = drain
        fmulp   st(1), st
        pop     eax
        ret
    }
}

extern "C" __declspec(naked) void CaveDec948()
{
    __asm {
        push  eax
        mov   eax, dword ptr [g_ticksDecay]
        sub   dword ptr [esi + 0x948], eax
        pop   eax
        ret
    }
}

extern "C" __declspec(naked) void CaveInc94C()
{
    __asm {
        push  eax
        mov   eax, dword ptr [g_ticks60]
        add   dword ptr [esi + 0x94C], eax
        pop   eax
        ret
    }
}

// Aspect-correct FOV. The engine keeps the vertical FOV, which is already right for wider screens
// (Hor+), but a taller screen such as 16:10 loses width (Hor-). For projections whose aspect is our
// patched one:
//  - world cameras: if aspect < 16:9, widen the vertical FOV so the horizontal FOV equals 16:9's;
//  - the 3D UI layer: its eye distance assumes a 1080-tall screen, so scale tan(fov/2) by H/1080.
extern "C" {
    uintptr_t g_projAddr = 0;
    float     g_fovAspect = 0.0f;   // patched camera aspect (0 = hook inactive)
    float     g_fovK = 1.0f;        // world cameras: (16/9) / aspect when narrower than 16:9, else 1
    float     g_fovKUi = 1.0f;      // 3D UI layer: height / 1080
    uintptr_t g_projUiRet[2] = {};  // relocated return addresses of the UI calls
}

extern "C" void __cdecl AdjustFov(float* fovAspect, uintptr_t ret)
{
    if (g_fovAspect == 0.0f || fabsf(fovAspect[1] - g_fovAspect) >= 1e-3f) return;
    const float k = (ret == g_projUiRet[0] || ret == g_projUiRet[1]) ? g_fovKUi : g_fovK;
    if (k != 1.0f) fovAspect[0] = 2.0f * atanf(tanf(fovAspect[0] * 0.5f) * k);
}

// Replaces "CALL perspective"; on entry [ESP]=ret, [ESP+4]=out, [ESP+8]=fov, [ESP+12]=aspect.
extern "C" __declspec(naked) void FovStub()
{
    __asm {
        push  eax
        push  ecx
        push  edx
        mov   eax, dword ptr [esp + 12]   // return address (identifies the call site)
        push  eax
        lea   eax, [esp + 24]             // &fov
        push  eax
        call  AdjustFov
        add   esp, 8
        pop   edx
        pop   ecx
        pop   eax
        jmp   dword ptr [g_projAddr]
    }
}

extern "C" {
    uintptr_t g_tugPressResume = 0, g_tugDecayResume = 0;
}

extern "C" __declspec(naked) void TugPressHook()
{
    __asm {
        fmul  dword ptr [esi + 0x938]
        push  eax
        mov   eax, dword ptr [g_multPtr]
        cmp   dword ptr [eax], 0
        je    skip
        fdiv  dword ptr [eax]
    skip:
        pop   eax
        jmp   dword ptr [g_tugPressResume]
    }
}

extern "C" __declspec(naked) void TugDecayHook()
{
    __asm {
        fmul  dword ptr [esi + 0x93C]
        fmul  dword ptr [g_easeNow]
        jmp   dword ptr [g_tugDecayResume]
    }
}

static void ApplyMode()
{
    // HFR on : variable path may go down to 60/TargetFPS; the fixed (30 fps) path keeps its
    //          per-second rate at MenuFPS by scaling its constant 1.0 by 30/MenuFPS.
    // HFR off: original game behaviour (60/30 fps, clamp 1.0, fixed multiplier 1.0).
    g_minMult    = g_hfrOn ? 60.0f / float(g_targetFps) : 1.0f;
    g_fixedScale = g_hfrOn ? 30.0f / float(g_menuFps)   : 1.0f;
}

template <typename T> static T* At(uintptr_t va) { return reinterpret_cast<T*>(va + g_delta); }

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static SRWLOCK g_logLock = SRWLOCK_INIT;   // hooks run on several game threads

static void Log(const char* fmt, ...)
{
    AcquireSRWLockExclusive(&g_logLock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_logLock); } } unlock;
    char path[MAX_PATH];
    _snprintf_s(path, _TRUNCATE, "%s%s", g_dir, cfg::kLogName);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Patching helpers
// ---------------------------------------------------------------------------
static bool Matches(const char* name, uintptr_t va, const uint8_t* bytes, size_t n)
{
    const uint8_t* p = At<uint8_t>(va);
    if (memcmp(p, bytes, n) == 0) return true;
    char got[64] = {};
    for (size_t i = 0; i < n && i < 16; ++i) _snprintf_s(got + i * 3, sizeof got - i * 3, _TRUNCATE, "%02X ", p[i]);
    Log("Signature mismatch at %s (0x%08X): %s", name, (unsigned)(va + g_delta), got);
    return false;
}

static bool PresentMatches()
{
    const uint8_t* p = At<uint8_t>(VA_PRESENT);
    const uint32_t operand = *reinterpret_cast<const uint32_t*>(p + 1);
    if (p[0] == kPresentOpcode && operand == kPresentOperand + g_delta) return true;
    Log("Signature mismatch at Present (0x%08X): %02X %08X", (unsigned)(VA_PRESENT + g_delta), p[0], operand);
    return false;
}

static void WriteJmp(uintptr_t va, const void* target, size_t len)
{
    uint8_t* p = At<uint8_t>(va);
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (reinterpret_cast<uintptr_t>(p) + 5));
    for (size_t i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

static void WriteBytes(uintptr_t va, const void* data, size_t len)
{
    uint8_t* p = At<uint8_t>(va);
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, data, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

// ---------------------------------------------------------------------------
// Display: render the in-game "1920x1080" option at Width x Height (16:10, ultrawide, ...) without
// letterboxing. Independent of the frame-rate patches; all-or-nothing on its own signatures.
// ---------------------------------------------------------------------------
static void InstallDisplay(const char* ini)
{
    // [Display] Width/Height; older configs only have Height1920 (width 1920).
    const int legacyH = GetPrivateProfileIntA("Display", "Height1920", 0, ini);
    const int height = GetPrivateProfileIntA("Display", "Height", legacyH, ini);
    const int width = GetPrivateProfileIntA("Display", "Width", 1920, ini);
    if (height == 0) { Log("Display: unchanged (Height=0)."); return; }
    if (height < 720 || height > 4320 || width < 1280 || width > 7680) {
        Log("Display: %dx%d out of range, ignored.", width, height);
        return;
    }

    bool ok = Matches("ResTableH", VA_RES_TABLE_H, kResTableHOrig, sizeof kResTableHOrig);
    ok &= Matches("ResIdx5H", VA_RES_IDX5_H, kResIdx5Orig, sizeof kResIdx5Orig);
    ok &= Matches("ResTableW", VA_RES_TABLE_W, kResTableWOrig, sizeof kResTableWOrig);
    ok &= Matches("ResIdx5W", VA_RES_IDX5_W, kResIdx5WOrig, sizeof kResIdx5WOrig);
    ok &= Matches("CamAspect", VA_CAM_ASPECT, kCamAspectOrig, sizeof kCamAspectOrig);
    for (size_t i = 0; i < _countof(VA_LETTERBOX); ++i)
        ok &= Matches("Letterbox", VA_LETTERBOX[i], kLetterboxOrig[i], 2);
    if (!ok) { Log("Display: signatures do not match. No display patches applied."); return; }

    const float heightF = float(height), widthF = float(width);
    const float aspect = widthF / heightF;
    uint8_t movEax[5] = { 0xB8 }, movEaxW[5] = { 0xB8 };
    *reinterpret_cast<int32_t*>(movEax + 1) = height;
    *reinterpret_cast<int32_t*>(movEaxW + 1) = width;

    WriteBytes(VA_RES_TABLE_H, &heightF, sizeof heightF);
    WriteBytes(VA_RES_IDX5_H, movEax, sizeof movEax);
    WriteBytes(VA_RES_TABLE_W, &widthF, sizeof widthF);
    WriteBytes(VA_RES_IDX5_W, movEaxW, sizeof movEaxW);
    WriteBytes(VA_CAM_ASPECT, &aspect, sizeof aspect);
    for (size_t i = 0; i < _countof(VA_LETTERBOX); ++i)
        WriteBytes(VA_LETTERBOX[i], kXorEaxEax, sizeof kXorEaxEax);
    Log("Display: 1920x1080 option -> %dx%d, camera aspect %.4f, letterbox off.", width, height, aspect);
    if (GetPrivateProfileIntA("Display", "VertPlusFov", 1, ini)) {
        g_fovAspect = aspect;
        g_fovK = aspect < 16.0f / 9.0f ? (16.0f / 9.0f) / aspect : 1.0f;
        g_fovKUi = heightF / 1080.0f;
    }
}

// ---------------------------------------------------------------------------
// Hook 1: frame-multiplier lower clamp (1.0 -> 60 / TargetFPS)
// Replaces "FLD1; FCOM [ESI+0x7C]" with "FLD [g_minMult]; FCOM [ESI+0x7C]".
// Stack depth and flags are identical, so the original compare/branch logic continues unchanged.
// ---------------------------------------------------------------------------
extern "C" __declspec(naked) void ClampHook()
{
    __asm {
        fld   dword ptr [g_minMult]
        fcom  dword ptr [esi + 0x7C]
        jmp   dword ptr [g_clampResume]
    }
}

// ---------------------------------------------------------------------------
// Hook 1b: fixed-multiplier path (variable timing disabled, game runs its 30 fps mode)
// Original: FSTP ST0; FSTP ST1; FLD [ESP+0x10] (the constant 1.0 passed by the main loop)
// New:      same, then FMUL [g_fixedScale]. Reached by JMP, so ESP is unchanged.
// ---------------------------------------------------------------------------
extern "C" __declspec(naked) void FixedHook()
{
    __asm {
        fstp  st(0)
        fstp  st(1)
        fld   dword ptr [esp + 0x10]
        fmul  dword ptr [g_fixedScale]
        jmp   dword ptr [g_fixedResume]
    }
}

// ---------------------------------------------------------------------------
// Hook 2: frame limiter replacement
// Same contract as the original: present, then wait for the frame budget.
// Uses QPC with absolute deadlines (no drift) and spins for the final ~1.5 ms.
// ---------------------------------------------------------------------------
typedef int (__cdecl* PresentFn)();
static PresentFn g_present = nullptr;
static LARGE_INTEGER g_freq;
static int64_t g_deadline = 0;

static int64_t Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

static void Pace(int fps)
{
    const int64_t period = g_freq.QuadPart / fps;
    int64_t now = Now();
    if (g_deadline == 0 || now - g_deadline > period) {
        // first frame, or we fell behind by more than a frame: resync instead of bursting
        g_deadline = now + period;
        return;
    }
    const int64_t spinWindow = g_freq.QuadPart * 15 / 10000; // 1.5 ms
    while (g_deadline - now > spinWindow) {
        Sleep(1);                                            // game already called timeBeginPeriod(1)
        now = Now();
    }
    while (Now() < g_deadline) _mm_pause();
    g_deadline += period;
}

static void Stats()
{
    static int frames = 0;
    static int64_t windowStart = 0;
    if (!g_logStats) return;
    int64_t now = Now();
    if (windowStart == 0) windowStart = now;
    if (++frames < 600) return;
    double secs = double(now - windowStart) / double(g_freq.QuadPart);
    Log("fps=%.1f hfr=%d mode=%d mult=%.3f scale=%.5f minMult=%.3f",
        frames / secs, g_hfrOn, *At<int>(VA_RATE_MODE), *At<float>(VA_FRAME_MULT), *At<float>(VA_MULT_SCALE), g_minMult);
    frames = 0;
    windowStart = now;
}

// Diagnostic ([General] LogInput=1): once per second while buttons are pressed, compares the
// game's pad-0 trigger (rising-edge) bits with rising edges computed here from the hold mask.
// If both match the presses made, the input layer is not losing presses.
static int g_logInput = 0;
static void TraceInput()
{
    static uint32_t lastHold = 0;
    static int trig = 0, edges = 0, frames = 0;
    static int64_t windowStart = 0;
    if (!g_logInput) return;
    const uint32_t hold = *At<uint32_t>(0x01B7B910), trigger = *At<uint32_t>(0x01B7B914);
    trig += __popcnt(trigger);
    edges += __popcnt(hold & ~lastHold);
    lastHold = hold;
    ++frames;
    const int64_t now = Now();
    if (windowStart == 0) windowStart = now;
    if (now - windowStart < g_freq.QuadPart) return;
    if (trig || edges) Log("input: %d frames, game triggers=%d, own edges=%d, hold=%08X", frames, trig, edges, hold);
    trig = edges = frames = 0;
    windowStart = now;
}

// Logs whenever the game's timing state changes, to identify menus / QTEs / cutscenes.
static void TraceState()
{
    static uint32_t last = 0xFFFFFFFF;
    const uint32_t f060 = *At<uint32_t>(VA_FLAGS_060), f064 = *At<uint32_t>(VA_FLAGS_064), f084 = *At<uint32_t>(VA_FLAGS_084);
    const bool fixed = (f064 & 0x4000) && (f060 & 0x40000000);
    const uint32_t key = (uint32_t)*At<int>(VA_RATE_MODE) | (fixed ? 2u : 0u) | ((f084 & 0x200000) ? 4u : 0u);
    if (key == last) return;
    last = key;
    Log("state: mode=%d timing=%s f060=%08X f064=%08X f084=%08X",
        *At<int>(VA_RATE_MODE), fixed ? "fixed" : "variable", f060, f064, f084);
}

static void PollToggle()
{
    static bool wasDown = false;
    if (!g_toggleKey) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_toggleKey) & 0x8000);
    if (down && !wasDown) {
        g_hfrOn = !g_hfrOn;
        ApplyMode();
        Log("toggle: HFR %s", g_hfrOn ? "ON" : "OFF (original 60/30 fps)");
    }
    wasDown = down;
}

// PresetKey (default F8): cycle TargetFPS through cfg::kPresets and save it to the ini.
static void PollPreset()
{
    static bool wasDown = false;
    if (!g_presetKey) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_presetKey) & 0x8000);
    if (down && !wasDown) {
        g_targetFps = cfg::NextPreset(g_targetFps);
        if (g_menuFollows) g_menuFps = g_targetFps;
        ApplyMode();
        char v[16];
        _snprintf_s(v, _TRUNCATE, "%d", g_targetFps);
        WritePrivateProfileStringA("General", "TargetFPS", v, g_ini);
        MessageBeep(MB_OK);
        Log("fps preset -> %d (menus %d, minMult %.3f)", g_targetFps, g_menuFps, g_minMult);
    }
    wasDown = down;
}

static void PhotoTick(float dt);   // photomode.inl

extern "C" void __cdecl LimiterHook()
{
    g_present();
    PollToggle();
    PollPreset();
    {   // photo mode input/state, with real-time dt
        static int64_t last = 0;
        const int64_t now = Now();
        const float dt = last ? float(double(now - last) / double(g_freq.QuadPart)) : 0.0f;
        last = now;
        PhotoTick(dt);
    }
    TraceState();
    TraceInput();
    UpdateFrameScalars();
    const bool mode30 = *At<int>(VA_RATE_MODE) == 1;
    const int fps = g_hfrOn ? (mode30 ? g_menuFps : g_targetFps) : (mode30 ? 30 : 60);
    Pace(fps);
    Stats();
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
static void WriteCall(uintptr_t va, const void* target, size_t len)
{
    uint8_t buf[16];
    uint8_t* p = At<uint8_t>(va);
    buf[0] = 0xE8;
    *reinterpret_cast<int32_t*>(buf + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (reinterpret_cast<uintptr_t>(p) + 5));
    for (size_t i = 5; i < len; ++i) buf[i] = 0x90;
    WriteBytes(va, buf, len);
}

// Vert+ FOV hook on every perspective-builder call (only acts on our patched aspect).
static void InstallVertPlus()
{
    if (g_fovAspect == 0.0f || (g_fovK == 1.0f && g_fovKUi == 1.0f)) return;
    for (uintptr_t va : VA_PROJ_CALLS) {
        const uint8_t* p = At<uint8_t>(va);
        const uintptr_t target = va + g_delta + 5 + *reinterpret_cast<const int32_t*>(p + 1);
        if (p[0] != 0xE8 || target != VA_PROJ + g_delta) {
            Log("Signature mismatch at ProjCall (0x%08X). Vert+ FOV not applied.", (unsigned)(va + g_delta));
            g_fovAspect = 0.0f;
            return;
        }
    }
    g_projAddr = VA_PROJ + g_delta;
    for (int i = 0; i < 2; ++i) g_projUiRet[i] = VA_PROJ_UI_CALLS[i] + g_delta + 5;
    for (uintptr_t va : VA_PROJ_CALLS) WriteCall(va, &FovStub, 5);
    Log("Aspect FOV applied: %u projection calls, world x%.3f, 3D UI x%.3f (tan).",
        (unsigned)_countof(VA_PROJ_CALLS), g_fovK, g_fovKUi);
}

// Scales frame-counted button-mash QTE logic by the frame multiplier.
static void InstallQte(const char* ini)
{
    if (!GetPrivateProfileIntA("Fixes", "QteFix", 1, ini)) { Log("QTE fix disabled (QteFix=0)."); return; }
    int ease = GetPrivateProfileIntA("Fixes", "QteEase", 80, ini);
    if (ease < 10) ease = 10;
    if (ease > 100) ease = 100;
    g_qteEase = ease / 100.0f;

    bool ok = true;
    const uint32_t constAddr = uint32_t(VA_QTE_DECAY_CONST + g_delta);
    for (uintptr_t va : VA_QTE_FSUB) {
        const uint8_t* p = At<uint8_t>(va);
        if (!(p[0] == 0xD8 && p[1] == 0x25 && *reinterpret_cast<const uint32_t*>(p + 2) == constAddr)) {
            Log("Signature mismatch at QteFsub (0x%08X): %02X %02X %08X", (unsigned)(va + g_delta), p[0], p[1],
                *reinterpret_cast<const uint32_t*>(p + 2));
            ok = false;
        }
    }
    ok &= Matches("QteDec948", VA_QTE_DEC948, kDec948Orig, sizeof kDec948Orig);
    for (uintptr_t va : VA_QTE_INC94C) ok &= Matches("QteInc94C", va, kInc94COrig, sizeof kInc94COrig);
    if (!ok) { Log("QTE fix: signatures do not match. Not applied."); return; }

    const uint32_t decayAddr = uint32_t(reinterpret_cast<uintptr_t>(&g_qteDecay));
    for (uintptr_t va : VA_QTE_FSUB) WriteBytes(va + 2, &decayAddr, sizeof decayAddr);
    WriteCall(VA_QTE_DEC948, &CaveDec948, sizeof kDec948Orig);
    for (uintptr_t va : VA_QTE_INC94C) WriteCall(va, &CaveInc94C, sizeof kInc94COrig);
    g_multPtr = VA_FRAME_MULT + g_delta;
    if (Matches("TugPress", VA_TUG_PRESS, kTugPressOrig, sizeof kTugPressOrig) &&
        Matches("TugDecay", VA_TUG_DECAY, kTugDecayOrig, sizeof kTugDecayOrig)) {
        g_tugPressResume = VA_TUG_PRESS + g_delta + sizeof kTugPressOrig;
        g_tugDecayResume = VA_TUG_DECAY + g_delta + sizeof kTugDecayOrig;
        WriteJmp(VA_TUG_PRESS, &TugPressHook, sizeof kTugPressOrig);
        WriteJmp(VA_TUG_DECAY, &TugDecayHook, sizeof kTugDecayOrig);
        Log("QTE fix: Sundowner shield tug-of-war patched (press /mult, decay x ease).");
    } else Log("QTE fix: Sundowner tug-of-war signature mismatch, skipped.");
    Log("QTE fix applied: %u gauge decays, %u frame counters, ease %d%% of original decay at high fps.",
        (unsigned)_countof(VA_QTE_FSUB), (unsigned)(1 + _countof(VA_QTE_INC94C)), ease);
}

// Makes Blade Mode / synced "unslowed" animations and the slow-mo ramp frame-rate independent.
static void InstallBlade(const char* ini)
{
    if (!GetPrivateProfileIntA("Fixes", "BladeModeFix", 1, ini)) { Log("Blade Mode fix disabled (BladeModeFix=0)."); return; }

    bool ok = true;
    for (uintptr_t va : VA_RATE_DIV_CALLS) {
        const uint8_t* p = At<uint8_t>(va);
        const uintptr_t target = va + g_delta + 5 + *reinterpret_cast<const int32_t*>(p + 1);
        if (p[0] != 0xE8 || target != VA_GET_RATE + g_delta) {
            Log("Signature mismatch at RateDiv (0x%08X): %02X -> 0x%08X", (unsigned)(va + g_delta), p[0], (unsigned)target);
            ok = false;
        }
    }
    ok &= Matches("LerpWorld", VA_LERP_WORLD, kLerpOrig, sizeof kLerpOrig);
    ok &= Matches("LerpPlayer", VA_LERP_PLAYER, kLerpOrig, sizeof kLerpOrig);
    if (!ok) { Log("Blade Mode fix: signatures do not match. Not applied."); return; }

    g_multPtr = VA_FRAME_MULT + g_delta;
    g_getRateAddr = VA_GET_RATE + g_delta;
    g_lerpWorldResume = VA_LERP_WORLD_RESUME + g_delta;
    g_lerpPlayerResume = VA_LERP_PLAYER_RESUME + g_delta;
    for (uintptr_t va : VA_RATE_DIV_CALLS) WriteCall(va, &GetRateNoMult, 5);
    WriteJmp(VA_LERP_WORLD, &LerpWorldHook, sizeof kLerpOrig);
    WriteJmp(VA_LERP_PLAYER, &LerpPlayerHook, sizeof kLerpOrig);
    Log("Blade Mode fix applied: %u rate-division sites, 2 slow-mo ramps.", (unsigned)_countof(VA_RATE_DIV_CALLS));

    unsigned drains = 0;
    for (uintptr_t va : VA_BLADE_DRAIN)
        if (Matches("BladeDrain", va, kBladeDrainOrig, sizeof kBladeDrainOrig)) {
            WriteCall(va, &BladeDrainHook, sizeof kBladeDrainOrig);
            ++drains;
        }
    Log("Blade Mode energy drain: %u of %u sites scaled to real time.", drains, (unsigned)_countof(VA_BLADE_DRAIN));
}

// ---------------------------------------------------------------------------
// Persistent debris: keep the pieces of the last N cuts. See docs/RESEARCH.md section 14.
// Pieces normally vanish through (1) their own lifetime (BehaviorDebrisBase update), (2) the
// per-group explosion of DebrisExplodeManager and (3) the DebrisManager reducer.
// A "cut event" = new pieces appearing within 0.3 s of each other. Stored piece pointers are only
// compared, never dereferenced, so pieces deleted by other paths (level change) are harmless.
// ---------------------------------------------------------------------------
static const uintptr_t VA_DEBRIS_PIECE   = 0x005DDA84; // CMP [ESI+0x928],EBX; JNE 0x5DDE02 (ST0 = 1.0 live)
static const uintptr_t VA_DEBRIS_CONT    = 0x005DDA90; // lifetime block
static const uintptr_t VA_DEBRIS_SKIP    = 0x005DDE02; // epilogue: pop esi; fstp st0; pop ebx; ret
static const uintptr_t VA_DEBRIS_GROUP   = 0x00942DCA; // LEA EAX,[ESP+0x10]; PUSH EAX (before group explode)
static const uintptr_t VA_DEBRIS_GRP_CONT = 0x00942DCF;
static const uintptr_t VA_DEBRIS_GRP_SKIP = 0x00942DEF; // next slot
static const uintptr_t VA_DEBRIS_REDUCE  = 0x005D9560; // force-fade one piece (thiscall, plain RET)
static const uintptr_t VA_DEBRIS_RED_CONT = 0x005D956A;
static const uint8_t kDebrisPieceOrig[]  = { 0x39, 0x9E, 0x28, 0x09, 0x00, 0x00, 0x0F, 0x85, 0x72, 0x03, 0x00, 0x00 };
static const uint8_t kDebrisGroupOrig[]  = { 0x8D, 0x44, 0x24, 0x10, 0x50 };
static const uint8_t kDebrisReduceOrig[] = { 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x80, 0x08, 0x00, 0x00, 0x00 };

struct DebrisEvent {
    int      id;
    int      n;
    void*    pieces[128];
    int      ng;
    uint32_t groups[8];
};
static DebrisEvent g_events[8];     // ring; g_evCount used, newest at g_evNewest
static int     g_keepCuts = 0, g_evCount = 0, g_evNewest = -1, g_evNextId = 1, g_logDebris = 0;
static int64_t g_lastNewPiece = 0;
static void*   g_released[512];     // pointers of evicted pieces: never treat them as new again
static int     g_releasedPos = 0;

static bool Contains(void* const* arr, int n, const void* p)
{
    for (int i = 0; i < n; ++i) if (arr[i] == p) return true;
    return false;
}

static void EvictOldest()
{
    const int oldest = (g_evNewest - (g_evCount - 1) + _countof(g_events)) % _countof(g_events);
    DebrisEvent& e = g_events[oldest];
    for (int i = 0; i < e.n; ++i) { g_released[g_releasedPos] = e.pieces[i]; g_releasedPos = (g_releasedPos + 1) % _countof(g_released); }
    if (g_logDebris) Log("debris: release cut #%d (%d pieces, %d groups)", e.id, e.n, e.ng);
    --g_evCount;
}

// Piece updates run on several game worker threads, so all event state is behind one lock.
static SRWLOCK g_debrisLock = SRWLOCK_INIT;
struct DebrisGuard {
    DebrisGuard() { AcquireSRWLockExclusive(&g_debrisLock); }
    ~DebrisGuard() { ReleaseSRWLockExclusive(&g_debrisLock); }
};

static void AddGroup(DebrisEvent& e, uint32_t group)
{
    if (group == 0 || e.ng >= (int)_countof(e.groups)) return;
    for (int i = 0; i < e.ng; ++i) if (e.groups[i] == group) return;
    e.groups[e.ng++] = group;
    if (g_logDebris) Log("debris: cut #%d group 0x%08X", e.id, group);
}

static void NoteThread()
{
    static DWORD seen[16];
    static int nSeen = 0;
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < nSeen; ++i) if (seen[i] == tid) return;
    if (nSeen < (int)_countof(seen)) seen[nSeen++] = tid;
    Log("debris: piece update on thread %lu (%d thread(s) so far)", tid, nSeen);
}

// Zandatsu: on entering a cut-and-grab, the player state registers the target's debris group with
// DebrisExplodeManager 0x941450 (thiscall, [esp+4] = group). In vanilla those pieces are deleted when
// the grab ends (via the enemy's vanish -> 0x940450); kept, they would hang in the air. Such groups
// are excluded from both persistent features.
static const uintptr_t VA_ZAN_REGISTER = 0x00941450;   // PUSH ECX/EBX/EBP/ESI/EDI; LEA EBP,[ECX+0x50]
static const uint8_t   kZanRegisterOrig[] = { 0x51, 0x53, 0x55, 0x56, 0x57, 0x8D, 0x69, 0x50 };
static uint32_t g_zanGroups[16];
static int      g_zanPos = 0;
extern "C" { uintptr_t g_zanResume = 0; }

static bool IsZanGroupLocked(uint32_t group)
{
    if (group == 0 || group == 0xFFFFFFFFu) return false;
    for (uint32_t z : g_zanGroups) if (z == group) return true;
    return false;
}

static void DropPieceLocked(const void* piece)
{
    for (int k = 0; k < g_evCount; ++k) {
        DebrisEvent& e = g_events[(g_evNewest - k + _countof(g_events)) % _countof(g_events)];
        for (int i = 0; i < e.n; ++i)
            if (e.pieces[i] == piece) { e.pieces[i] = e.pieces[--e.n]; return; }
    }
}

extern "C" void __cdecl DebrisNoteZandatsu(uint32_t group)
{
    if (group == 0 || group == 0xFFFFFFFFu) return;
    DebrisGuard guard;
    if (!IsZanGroupLocked(group)) { g_zanGroups[g_zanPos] = group; g_zanPos = (g_zanPos + 1) % (int)_countof(g_zanGroups); }
    for (int k = 0; k < g_evCount; ++k) {   // stop protecting that group
        DebrisEvent& e = g_events[(g_evNewest - k + _countof(g_events)) % _countof(g_events)];
        for (int i = 0; i < e.ng; ++i)
            if (e.groups[i] == group) { e.groups[i] = e.groups[--e.ng]; break; }
    }
    if (g_logDebris) Log("debris: zandatsu group 0x%08X excluded", group);
}

extern "C" bool __cdecl DebrisIsZandatsuGroup(uint32_t group)
{
    DebrisGuard guard;
    return IsZanGroupLocked(group);
}

// Entry detour of 0x941450 (thiscall: ECX = manager, [ESP+4] = group, [ESP+8] = collision group).
extern "C" __declspec(naked) void ZanRegisterHook()
{
    __asm {
        push  ecx
        push  edx
        mov   eax, dword ptr [esp + 12]
        push  eax
        call  DebrisNoteZandatsu
        add   esp, 4
        pop   edx
        pop   ecx
        push  ecx
        push  ebx
        push  ebp
        push  esi
        push  edi
        lea   ebp, [ecx + 0x50]
        jmp   dword ptr [g_zanResume]
    }
}

// Called for every live piece each frame. Returns 1 if the piece belongs to one of the last N cuts.
extern "C" int __cdecl DebrisOnPiece(uint8_t* piece)
{
    DebrisGuard guard;
    if (g_logDebris) NoteThread();
    const uint32_t group = *reinterpret_cast<uint32_t*>(piece + 0x83C);
    if (IsZanGroupLocked(group)) { DropPieceLocked(piece); return 0; }   // zandatsu pieces vanish as usual
    for (int k = 0; k < g_evCount; ++k) {
        DebrisEvent& e = g_events[(g_evNewest - k + _countof(g_events)) % _countof(g_events)];
        if (Contains(e.pieces, e.n, piece)) { AddGroup(e, group); return 1; }   // group may be assigned later
    }
    // A new piece: age (seconds) just started and we have not released it before.
    if (*reinterpret_cast<float*>(piece + 0x884) >= 0.05f) return 0;
    if (Contains(g_released, _countof(g_released), piece)) return 0;

    const int64_t now = Now();
    if (!(g_evCount > 0 && now - g_lastNewPiece < g_freq.QuadPart * 3 / 10)) {
        if (g_evCount == g_keepCuts) EvictOldest();
        g_evNewest = (g_evNewest + 1) % _countof(g_events);
        DebrisEvent& fresh = g_events[g_evNewest];
        fresh.id = g_evNextId++;
        fresh.n = fresh.ng = 0;
        ++g_evCount;
        if (g_logDebris) Log("debris: new cut #%d", fresh.id);
    }
    g_lastNewPiece = now;
    DebrisEvent& e = g_events[g_evNewest];
    if (e.n == _countof(e.pieces)) return 0;
    e.pieces[e.n++] = piece;
    AddGroup(e, group);
    return 1;
}

// Returns 1 if this DebrisExplodeManager slot's group belongs to one of the last N cuts.
extern "C" int __cdecl DebrisGroupProtected(uint8_t* slot)
{
    const uint32_t group = *reinterpret_cast<uint32_t*>(slot + 0x14);
    if (group == 0) return 0;
    DebrisGuard guard;
    for (int k = 0; k < g_evCount; ++k) {
        const DebrisEvent& e = g_events[(g_evNewest - k + _countof(g_events)) % _countof(g_events)];
        for (int i = 0; i < e.ng; ++i) if (e.groups[i] == group) return 1;
    }
    return 0;
}

extern "C" int __cdecl DebrisIsProtected(void* piece)
{
    DebrisGuard guard;
    for (int k = 0; k < g_evCount; ++k) {
        const DebrisEvent& e = g_events[(g_evNewest - k + _countof(g_events)) % _countof(g_events)];
        if (Contains(e.pieces, e.n, piece)) return 1;
    }
    return 0;
}

// Enemy body-part debris (BehaviorDebrisActor / EmActor / Em0010DebrisActor) has its own update:
// age +0x884 grows and the piece fades at [+0x8F0]. Protected pieces get their age held at 0.
static const uintptr_t VA_DEBRIS_PRE_FN = 0x00A8B7F0;  // common call at the top of each update
static const uintptr_t VA_DEBRIS_ACTOR_CALLS[] = { 0x005DDFBE, 0x005DE4EE, 0x005E662E };

extern "C" void __cdecl DebrisOnActor(uint8_t* piece)
{
    if (DebrisOnPiece(piece)) *reinterpret_cast<float*>(piece + 0x884) = 0.0f;
}

extern "C" {
    uintptr_t g_debrisCont = 0, g_debrisSkip = 0, g_grpCont = 0, g_grpSkip = 0, g_redCont = 0;
    uintptr_t g_debrisPreFn = 0;
}

// Replaces "CALL 0xA8B7F0" at the top of an actor-debris update (ECX = ESI = piece).
extern "C" __declspec(naked) void DebrisActorHook()
{
    __asm {
        call  dword ptr [g_debrisPreFn]
        push  eax
        push  esi
        call  DebrisOnActor
        add   esp, 4
        pop   eax
        ret
    }
}

// Replaces "CMP [ESI+0x928],EBX; JNE skip". ESI = piece, EBX = 0, ST0 = 1.0 (left untouched).
extern "C" __declspec(naked) void DebrisPieceHook()
{
    __asm {
        push  ecx
        push  edx
        push  esi
        call  DebrisOnPiece
        add   esp, 4
        pop   edx
        pop   ecx
        test  eax, eax
        jz    orig
        // Kept: the update (physics, transform) still runs; only the lifetime clock is held at 0,
        // so it never fades, is deleted or (explosive pieces) explodes.
        mov   dword ptr [esi + 0x524], 0xBF800000   // priority -1: the reducer skips it
        mov   dword ptr [esi + 0x884], 0            // age
    orig:
        cmp   dword ptr [esi + 0x928], ebx
        jne   frozen
        jmp   dword ptr [g_debrisCont]
    frozen:
        jmp   dword ptr [g_debrisSkip]
    }
}

// Replaces "LEA EAX,[ESP+0x10]; PUSH EAX" before a group explodes. ESI = slot + 0xC.
extern "C" __declspec(naked) void DebrisGroupHook()
{
    __asm {
        push  ecx
        push  edx
        lea   eax, [esi - 0xC]
        push  eax
        call  DebrisGroupProtected
        add   esp, 4
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        lea   eax, [esp + 0x10]
        push  eax
        jmp   dword ptr [g_grpCont]
    skip:
        jmp   dword ptr [g_grpSkip]
    }
}

// Replaces "PUSH ESI; MOV ESI,ECX; CMP [ESI+0x880],0" at the start of the force-fade (ECX = piece).
extern "C" __declspec(naked) void DebrisReduceHook()
{
    __asm {
        push  ecx
        push  edx
        push  ecx
        call  DebrisIsProtected
        add   esp, 4
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        push  esi
        mov   esi, ecx
        cmp   dword ptr [esi + 0x880], 0
        jmp   dword ptr [g_redCont]
    skip:
        ret
    }
}

// "Kill all enemies (and let their pieces finish)" objectives: the EmSet update 0xca7b20 sets a group's
// done flag G+0x654 only when no slot still owns a debris group (counted at [esp+0x14]). Kept pieces and
// kept bodies never release theirs, so those objectives would never complete. 0xca7bff "JNE skip" is
// removed so the group completes as soon as all its waves are done (vanilla waits a few seconds for
// the pieces to explode first).
static const uintptr_t VA_EMSET_DEBRIS_GATE = 0x00CA7BFB;   // CMP [ESP+0x14],EBX; JNE +0x0F
static const uint8_t   kEmSetGateOrig[] = { 0x39, 0x5C, 0x24, 0x14, 0x75, 0x0F };
static void UnblockDebrisObjectives()
{
    static bool done = false;
    if (done) return;
    done = true;
    // Zandatsu exclusion (shared by kept pieces and kept bodies).
    if (Matches("ZanRegister", VA_ZAN_REGISTER, kZanRegisterOrig, sizeof kZanRegisterOrig)) {
        g_zanResume = VA_ZAN_REGISTER + g_delta + sizeof kZanRegisterOrig;
        WriteJmp(VA_ZAN_REGISTER, &ZanRegisterHook, sizeof kZanRegisterOrig);
        Log("Debris: zandatsu targets are excluded from kept pieces/bodies.");
    } else Log("Debris: zandatsu register not matched; grabbed pieces may hang in the air.");
    if (!Matches("EmSetDebrisGate", VA_EMSET_DEBRIS_GATE, kEmSetGateOrig, sizeof kEmSetGateOrig)) {
        Log("Debris: objective gate not matched; 'finish debris' objectives may wait for kept pieces.");
        return;
    }
    const uint8_t nops[2] = { 0x90, 0x90 };
    WriteBytes(VA_EMSET_DEBRIS_GATE + 4, nops, sizeof nops);
    Log("Debris: kill-all objectives no longer wait for kept pieces/bodies.");
}

static void InstallDebris(const char* ini)
{
    g_keepCuts = GetPrivateProfileIntA("Debris", "KeepLastCuts", cfg::kKeepLastCuts, ini);
    if (g_keepCuts <= 0) { Log("Debris: unchanged (KeepLastCuts=0)."); return; }
    if (g_keepCuts > (int)_countof(g_events)) g_keepCuts = (int)_countof(g_events);
    g_logDebris = GetPrivateProfileIntA("Debris", "LogDebris", cfg::kLogDebris, ini);

    bool ok = Matches("DebrisPiece", VA_DEBRIS_PIECE, kDebrisPieceOrig, sizeof kDebrisPieceOrig);
    ok &= Matches("DebrisGroup", VA_DEBRIS_GROUP, kDebrisGroupOrig, sizeof kDebrisGroupOrig);
    ok &= Matches("DebrisReduce", VA_DEBRIS_REDUCE, kDebrisReduceOrig, sizeof kDebrisReduceOrig);
    for (uintptr_t va : VA_DEBRIS_ACTOR_CALLS) {
        const uint8_t* p = At<uint8_t>(va);
        if (p[0] != 0xE8 || va + g_delta + 5 + *reinterpret_cast<const int32_t*>(p + 1) != VA_DEBRIS_PRE_FN + g_delta) {
            Log("Signature mismatch at DebrisActor (0x%08X).", (unsigned)(va + g_delta));
            ok = false;
        }
    }
    if (!ok) { Log("Debris: signatures do not match. Not applied."); return; }

    QueryPerformanceFrequency(&g_freq);
    g_debrisCont = VA_DEBRIS_CONT + g_delta;
    g_debrisSkip = VA_DEBRIS_SKIP + g_delta;
    g_grpCont = VA_DEBRIS_GRP_CONT + g_delta;
    g_grpSkip = VA_DEBRIS_GRP_SKIP + g_delta;
    g_redCont = VA_DEBRIS_RED_CONT + g_delta;
    WriteJmp(VA_DEBRIS_PIECE, &DebrisPieceHook, sizeof kDebrisPieceOrig);
    WriteJmp(VA_DEBRIS_GROUP, &DebrisGroupHook, sizeof kDebrisGroupOrig);
    WriteJmp(VA_DEBRIS_REDUCE, &DebrisReduceHook, sizeof kDebrisReduceOrig);
    g_debrisPreFn = VA_DEBRIS_PRE_FN + g_delta;
    for (uintptr_t va : VA_DEBRIS_ACTOR_CALLS) WriteCall(va, &DebrisActorHook, 5);
    Log("Debris: keeping the pieces of the last %d cut(s) (objects + 3 enemy-part types).", g_keepCuts);
    UnblockDebrisObjectives();
}


// ---------------------------------------------------------------------------
// Persistent corpses: keep the bodies of the last N killed enemies. See docs/RESEARCH.md section 15.
// After the death animation each enemy type calls its "vanish" function (explosion effect + sound +
// hide), then deletes the object 180 frames later. We skip vanish for the last N bodies and rewind
// the death state step so the same code (and our check) runs again next frame. When a body is pushed
// out of the ring, its next vanish call goes through and it disappears as usual.
// Stored pointers are only compared, never dereferenced.
// ---------------------------------------------------------------------------
static const uintptr_t VA_VANISH_0010 = 0x00B346D0; // Em0010 (cyborg soldier): thiscall(int), RET 4
static const uintptr_t VA_VANISH_0030 = 0x00B67710; // Em0030: thiscall(), RET
struct CorpseSite { uintptr_t va; int kind; int mode; };
// kind 0 = Em0010, 1 = Em0030. mode: 1 = step := 2, 2 = step := 0, 3 = step -= 1 (an INC follows),
// 4 = step -= EBX (an ADD [step],EBX follows).
static const CorpseSite kCorpseSites[] = {
    { 0x00B3F668, 0, 1 },
    { 0x00B3F8C7, 0, 1 },
    { 0x00B3F9B8, 0, 2 },
    { 0x00B35ABA, 0, 3 },
    { 0x00B6D1EF, 1, 3 },
    { 0x00B6D3EE, 1, 4 },
    { 0x00B6DBA6, 1, 3 },
};

static void*   g_corpses[8];
static int     g_keepCorpses = 0, g_corpseCount = 0, g_corpseNewest = -1, g_logCorpses = 0;
static void*   g_corpseReleased[32];
static int     g_corpseRelCount = 0;
static SRWLOCK g_corpseLock = SRWLOCK_INIT;

extern "C" {
    uintptr_t g_vanish0010 = 0, g_vanish0030 = 0;
}

// Returns 1 to skip the vanish call (body is kept), 0 to let it run.
extern "C" int __cdecl CorpseKeep(uint8_t* obj, int site, int ebxValue)
{
    const CorpseSite& cs = kCorpseSites[site];
    if (cs.kind == 0 && (*reinterpret_cast<uint32_t*>(obj + 0xDD4) & 0x100)) return 0;  // special death
    if (cs.kind == 1 && *reinterpret_cast<uint32_t*>(obj + 0xA50) == 0) return 0;        // instant vanish
    if (DebrisIsZandatsuGroup(*reinterpret_cast<uint32_t*>(obj + 0x83C))) return 0;      // zandatsu target

    AcquireSRWLockExclusive(&g_corpseLock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_corpseLock); } } unlock;

    for (int i = 0; i < g_corpseRelCount; ++i)
        if (g_corpseReleased[i] == obj) {           // pushed out earlier: vanish now, forget it
            g_corpseReleased[i] = g_corpseReleased[--g_corpseRelCount];
            return 0;
        }
    bool kept = false;
    for (int k = 0; k < g_corpseCount && !kept; ++k)
        kept = g_corpses[(g_corpseNewest - k + _countof(g_corpses)) % _countof(g_corpses)] == obj;
    if (!kept) {
        if (g_corpseCount == g_keepCorpses) {       // release the oldest
            void* old = g_corpses[(g_corpseNewest - (g_corpseCount - 1) + _countof(g_corpses)) % _countof(g_corpses)];
            --g_corpseCount;
            if (g_corpseRelCount == (int)_countof(g_corpseReleased)) --g_corpseRelCount;
            g_corpseReleased[g_corpseRelCount++] = old;
            if (g_logCorpses) Log("corpse: release %p", old);
        }
        g_corpseNewest = (g_corpseNewest + 1) % _countof(g_corpses);
        g_corpses[g_corpseNewest] = obj;
        ++g_corpseCount;
        if (g_logCorpses) Log("corpse: keep %p (%s, site %d)", obj, cs.kind == 0 ? "Em0010" : "Em0030", site);
    }
    int32_t& step = *reinterpret_cast<int32_t*>(obj + 0x61C);
    switch (cs.mode) {
    case 1: step = 2; break;
    case 2: step = 0; break;
    case 3: step -= 1; break;
    case 4: step -= ebxValue; break;
    }
    *reinterpret_cast<int32_t*>(obj + 0x6BC) = 1;     // EmSet: not counted as alive (waves continue)
    return 1;
}

extern "C" __declspec(naked) void CorpseStub0()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  0
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0010]
    skip:
        ret   4
    }
}

extern "C" __declspec(naked) void CorpseStub1()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  1
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0010]
    skip:
        ret   4
    }
}

extern "C" __declspec(naked) void CorpseStub2()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  2
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0010]
    skip:
        ret   4
    }
}

extern "C" __declspec(naked) void CorpseStub3()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  3
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0010]
    skip:
        ret   4
    }
}

extern "C" __declspec(naked) void CorpseStub4()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  4
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0030]
    skip:
        ret
    }
}

extern "C" __declspec(naked) void CorpseStub5()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  5
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0030]
    skip:
        ret
    }
}

extern "C" __declspec(naked) void CorpseStub6()
{
    __asm {
        push  ecx
        push  edx
        push  ebx
        push  6
        push  esi
        call  CorpseKeep
        add   esp, 12
        pop   edx
        pop   ecx
        test  eax, eax
        jnz   skip
        jmp   dword ptr [g_vanish0030]
    skip:
        ret
    }
}

static void (*const kCorpseStubs[])() = { CorpseStub0, CorpseStub1, CorpseStub2, CorpseStub3, CorpseStub4, CorpseStub5, CorpseStub6 };

static void InstallCorpses(const char* ini)
{
    g_keepCorpses = GetPrivateProfileIntA("Debris", "KeepCorpses", cfg::kKeepCorpses, ini);
    if (g_keepCorpses <= 0) { Log("Corpses: unchanged (KeepCorpses=0)."); return; }
    if (g_keepCorpses > (int)_countof(g_corpses)) g_keepCorpses = (int)_countof(g_corpses);
    g_logCorpses = GetPrivateProfileIntA("Debris", "LogDebris", cfg::kLogDebris, ini);
    for (const CorpseSite& cs : kCorpseSites) {
        const uint8_t* p = At<uint8_t>(cs.va);
        const uintptr_t want = (cs.kind == 0 ? VA_VANISH_0010 : VA_VANISH_0030) + g_delta;
        if (p[0] != 0xE8 || cs.va + g_delta + 5 + *reinterpret_cast<const int32_t*>(p + 1) != want) {
            Log("Signature mismatch at Corpse call (0x%08X). Corpses not applied.", (unsigned)(cs.va + g_delta));
            return;
        }
    }
    g_vanish0010 = VA_VANISH_0010 + g_delta;
    g_vanish0030 = VA_VANISH_0030 + g_delta;
    for (size_t i = 0; i < _countof(kCorpseSites); ++i) WriteCall(kCorpseSites[i].va, reinterpret_cast<const void*>(kCorpseStubs[i]), 5);
    Log("Corpses: keeping the last %d enemy bodies (Em0010, Em0030; %u vanish calls).", g_keepCorpses, (unsigned)_countof(kCorpseSites));
    UnblockDebrisObjectives();
}

#include "photomode.inl"

static void Install()
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    g_delta = base - kPreferredBase;

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt  = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kTimeDateStamp) {
        Log("Unsupported exe (TimeDateStamp 0x%08lX). No patches applied.", nt->FileHeader.TimeDateStamp);
        return;
    }

    char* ini = g_ini;
    _snprintf_s(g_ini, _TRUNCATE, "%s%s", g_dir, cfg::kIniName);
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {   // migrate settings from the old name
        char legacy[MAX_PATH];
        _snprintf_s(legacy, _TRUNCATE, "%s%s", g_dir, cfg::kLegacyIniName);
        if (CopyFileA(legacy, g_ini, TRUE)) Log("Settings migrated from %s to %s.", cfg::kLegacyIniName, cfg::kIniName);
    }
    if (!GetPrivateProfileIntA("General", "Enabled", 1, ini)) { Log("Disabled in %s.", cfg::kIniName); return; }

    InstallDisplay(ini);
    InstallVertPlus();
    InstallDebris(ini);
    InstallCorpses(ini);
    InstallPhotoMode(ini);

    bool ok = Matches("Limiter", VA_LIMITER, kLimiterOrig, sizeof kLimiterOrig);
    ok &= Matches("Clamp", VA_CLAMP_SITE, kClampOrig, sizeof kClampOrig);
    ok &= Matches("Fixed", VA_FIXED_SITE, kFixedOrig, sizeof kFixedOrig);
    ok &= PresentMatches();
    if (!ok) {
        Log("Frame-rate signatures do not match (base=0x%08X). No frame-rate patches applied.", (unsigned)base);
        return;
    }
    if (!GetPrivateProfileIntA("General", "HighFrameRate", 1, ini)) { Log("Frame-rate patches disabled (HighFrameRate=0)."); return; }
    g_targetFps = cfg::ClampFps(GetPrivateProfileIntA("General", "TargetFPS", cfg::kTargetFps, ini));
    g_menuFps = GetPrivateProfileIntA("General", "MenuFPS", cfg::kMenuFps, ini);
    g_menuFollows = g_menuFps == 0;
    if (g_menuFollows) g_menuFps = g_targetFps;
    if (g_menuFps < cfg::kMinMenuFps) g_menuFps = cfg::kMinMenuFps;
    if (g_menuFps > cfg::kMaxFps) g_menuFps = cfg::kMaxFps;
    g_toggleKey = GetPrivateProfileIntA("General", "ToggleKey", cfg::kToggleKey, ini);
    g_presetKey = GetPrivateProfileIntA("General", "PresetKey", cfg::kPresetKey, ini);
    g_logStats = GetPrivateProfileIntA("General", "LogStats", 1, ini);
    g_logInput = GetPrivateProfileIntA("General", "LogInput", 0, ini);

    QueryPerformanceFrequency(&g_freq);
    g_present = reinterpret_cast<PresentFn>(VA_PRESENT + g_delta);
    g_clampResume = VA_CLAMP_RESUME + g_delta;
    g_fixedResume = VA_FIXED_RESUME + g_delta;
    ApplyMode();

    WriteJmp(VA_CLAMP_SITE, &ClampHook, sizeof kClampOrig);
    WriteJmp(VA_FIXED_SITE, &FixedHook, sizeof kFixedOrig);
    WriteJmp(VA_LIMITER, &LimiterHook, sizeof kLimiterOrig);
    Log("Patched: TargetFPS=%d MenuFPS=%d minMult=%.3f fixedScale=%.3f ToggleKey=0x%02X PresetKey=0x%02X base=0x%08X",
        g_targetFps, g_menuFps, g_minMult, g_fixedScale, g_toggleKey, g_presetKey, (unsigned)base);

    // Depends on LimiterHook updating g_qteDecay / g_ticks60 every frame.
    InstallQte(ini);
    InstallBlade(ini);
}

// ---------------------------------------------------------------------------
// dinput8 proxy
// ---------------------------------------------------------------------------
typedef HRESULT (WINAPI* DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, void* outer)
{
    static DirectInput8CreateFn real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        strcpy_s(path + n, MAX_PATH - n, "\\dinput8.dll");
        HMODULE h = LoadLibraryA(path);
        if (h) real = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(h, "DirectInput8Create"));
        if (!real) { Log("Could not load system dinput8.dll"); return E_FAIL; }
    }
    return real(inst, ver, riid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        DWORD n = GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        while (n > 0 && g_dir[n - 1] != '\\') --n;
        g_dir[n] = '\0';
        Install();
    }
    return TRUE;
}
