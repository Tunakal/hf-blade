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
// Frame-time delta in the multiplier update 0xE03970: "FLD ST0; FSUB [ESI+0x80]" = now - last, both float
// milliseconds since startup (0xDF81D0). A float holds ~7 digits, so after an hour of play the step is
// 0.25 ms and after four hours 1 ms: fine at 60 fps, but at 240-360 fps (2.8-4.2 ms frames) the
// multiplier jitters by 10-35 %. Replaced with a double-precision QPC delta.
static const uintptr_t VA_FRAME_DELTA  = 0x00E03999;
static const uintptr_t VA_FRAME_DELTA_RESUME = 0x00E039A1; // FST [ESI+0x8C]
static const uint8_t   kFrameDeltaOrig[] = { 0xD9, 0xC0, 0xD8, 0xA6, 0x80, 0x00, 0x00, 0x00 };
// Camera turn speed. 0xDB9880 (shared by 8 camera states) turns the right-stick value into a per-frame
// rotation "x*|x|*K*arg" and only its upper limit is scaled by the multiplier, so the camera turned
// faster the higher the frame rate. Both products (pitch, yaw): FMUL [ESP+0x4C]; FSTP [ESP].
static const uintptr_t VA_CAM_PITCH    = 0x00DB99CB, VA_CAM_PITCH_RESUME = 0x00DB99D2;
static const uintptr_t VA_CAM_YAW      = 0x00DB9A3D, VA_CAM_YAW_RESUME   = 0x00DB9A44;
static const uint8_t   kCamTurnOrig[]  = { 0xD8, 0x4C, 0x24, 0x4C, 0xD9, 0x1C, 0x24 };
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

// Precise frame delta for 0xE03970 (see VA_FRAME_DELTA). Falls back to the game's own float delta on the
// first call and for implausible values.
extern "C" {
    float     g_fdNow = 0.0f;
    uintptr_t g_frameDeltaResume = 0;
}
extern "C" double __cdecl PreciseFrameDelta(float nowMs, float lastMs)
{
    static LARGE_INTEGER freq = {}, last = {};
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const double fallback = double(nowMs) - double(lastMs);
    if (!last.QuadPart) { last = c; return fallback; }
    const double ms = double(c.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart);
    last = c;
    if (!(ms > 0.0) || ms > 1000.0) return fallback;
    return ms;
}

// Replaces "FLD ST0; FSUB [ESI+0x80]". In: ST0 = now (float ms). Out: ST0 = delta, ST1 = now.
extern "C" __declspec(naked) void FrameDeltaHook()
{
    __asm {
        pushfd                                  // the caller tests flags (CMP at 0xE03994) after this
        pushad
        sub   esp, 8
        fst   dword ptr [esp]                   // nowMs
        fld   dword ptr [esi + 0x80]
        fstp  dword ptr [esp + 4]               // lastMs
        fstp  dword ptr [g_fdNow]               // FPU stack now empty, as the C ABI expects
        call  PreciseFrameDelta                 // ST0 = delta
        add   esp, 8
        fld   dword ptr [g_fdNow]               // ST0 = now, ST1 = delta
        fxch  st(1)                             // ST0 = delta, ST1 = now
        popad
        popfd
        jmp   dword ptr [g_frameDeltaResume]
    }
}

extern "C" {
    float     g_camScale = 1.0f;        // min(mult, 1) while HFR is on, else 1
    uintptr_t g_camPitchResume = 0, g_camYawResume = 0;
}
extern "C" __declspec(naked) void CamPitchHook()
{
    __asm {
        fmul  dword ptr [esp + 0x4C]
        fmul  dword ptr [g_camScale]
        fstp  dword ptr [esp]
        jmp   dword ptr [g_camPitchResume]
    }
}
extern "C" __declspec(naked) void CamYawHook()
{
    __asm {
        fmul  dword ptr [esp + 0x4C]
        fmul  dword ptr [g_camScale]
        fstp  dword ptr [esp]
        jmp   dword ptr [g_camYawResume]
    }
}

// Called once per frame. The game computes the multiplier at the start of the next frame;
// using the latest value is accurate to within one frame.
static void UpdateFrameScalars()
{
    static double acc = 0.0, accDecay = 0.0;
    const float mult = *reinterpret_cast<float*>(0x01BE942C + g_delta);
    const float ease = g_hfrOn ? g_qteEase : 1.0f;
    g_easeNow = ease;
    g_camScale = (g_hfrOn && mult > 0.0f && mult < 1.0f) ? mult : 1.0f;
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
    // Sleep(1) can overshoot by ~1 ms, so only sleep while more than 2 ms remain, then spin.
    // At 240-360 fps (2.8-4.2 ms frames) this is mostly spinning.
    const int64_t spinWindow = g_freq.QuadPart * 2 / 1000;   // 2 ms
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
static void WeaponSwitchTick();    // weaponswitch.inl
static void CheatTick();           // cheats.inl
static void TracePads();           // after the .inl includes

// Title screen ("press start"): its alpha movie (movie_ui/Title_Alpha.usm, Raiden's eye glow) only plays
// correctly at the original frame rate. cTitleStart's update (vtable slot 5) marks the frame unless its
// state [+0x38] is 5 (main menu; 3 = "press start", 4/6 = fades); while marked, the limiter runs at
// TitleFPS (default 30, as in the original game).
static const uintptr_t VA_TITLE_START_UPDATE_SLOT = 0x01656CA0;   // cTitleStart vtable +0x14
static const uintptr_t VA_TITLE_START_UPDATE      = 0x009A7220;
extern "C" {
    uint32_t          g_frameNo = 0;
    volatile uint32_t g_titleFrame = 0x80000000u;
    uintptr_t         g_titleOrig = 0;
}
static int g_titleFps = 30;

extern "C" __declspec(naked) void TitleStartHook()
{
    __asm {
        cmp   dword ptr [ecx + 0x38], 5       // state 5 = main menu: keep MenuFPS there
        je    orig
        mov   eax, dword ptr [g_frameNo]
        mov   dword ptr [g_titleFrame], eax
    orig:
        jmp   dword ptr [g_titleOrig]
    }
}

static void InstallTitleFps(const char* ini)
{
    g_titleFps = GetPrivateProfileIntA("General", "TitleFPS", 0, ini);
    if (g_titleFps <= 0) { Log("Title screen: TitleFPS=0, title runs at MenuFPS."); return; }
    if (g_titleFps < 30) g_titleFps = 30;
    if (g_titleFps > cfg::kMaxFps) g_titleFps = cfg::kMaxFps;
    uint32_t* slot = At<uint32_t>(VA_TITLE_START_UPDATE_SLOT);
    if (*slot != uint32_t(VA_TITLE_START_UPDATE + g_delta)) {
        Log("Title screen: cTitleStart vtable slot mismatch (0x%08X), TitleFPS not applied.", *slot);
        g_titleFps = 0;
        return;
    }
    g_titleOrig = VA_TITLE_START_UPDATE + g_delta;
    const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&TitleStartHook));
    WriteBytes(VA_TITLE_START_UPDATE_SLOT, &hook, sizeof hook);
    Log("Title screen: runs at %d fps (TitleFPS).", g_titleFps);
}

// Menu auto-repeat: the input devices count frames while a button or stick direction is held
// (pad 0xddaac0, keyboard 0xdda1xx), against a delay [0x1dd05bc] and an interval [0x1dd05b8]. Above
// the original rate a short tap outlasts the delay and one press moves 2-3 steps. The counters now
// advance only on the original-rate tick: 30 Hz in the 30 fps (menu) mode, else 60 Hz.
static const uintptr_t VA_REPEAT_PAD     = 0x00DDAE35;   // INC [ECX+0x2C]; MOV EAX,[ECX+0x2C]
static const uintptr_t VA_REPEAT_PAD_SKIP = 0x00DDAE5B;  // epilogue (repeat mask stays = trigger)
static const uintptr_t VA_REPEAT_KB      = 0x00DDA23F;   // INC [EAX+0x2C]; MOV ECX,[EAX+0x2C]
static const uintptr_t VA_REPEAT_KB_SKIP = 0x00DDA267;   // POP EDI; POP ESI; RET (EBX not pushed yet)
static const uint8_t kRepeatPadOrig[] = { 0xFF, 0x41, 0x2C, 0x8B, 0x41, 0x2C };
static const uint8_t kRepeatKbOrig[]  = { 0xFF, 0x40, 0x2C, 0x8B, 0x48, 0x2C };
extern "C" {
    volatile LONG g_repeatTick = 1;
    uintptr_t g_repPadCont = 0, g_repPadSkip = 0, g_repKbCont = 0, g_repKbSkip = 0;
}
extern "C" __declspec(naked) void RepeatPadHook()
{
    __asm {
        cmp   dword ptr [g_repeatTick], 0
        je    skip
        inc   dword ptr [ecx + 0x2C]
        mov   eax, dword ptr [ecx + 0x2C]
        jmp   dword ptr [g_repPadCont]
    skip:
        jmp   dword ptr [g_repPadSkip]
    }
}
extern "C" __declspec(naked) void RepeatKbHook()
{
    __asm {
        cmp   dword ptr [g_repeatTick], 0
        je    skip
        inc   dword ptr [eax + 0x2C]
        mov   ecx, dword ptr [eax + 0x2C]
        jmp   dword ptr [g_repKbCont]
    skip:
        jmp   dword ptr [g_repKbSkip]
    }
}
static void UpdateRepeatTick(bool mode30)
{
    static int64_t last = 0;
    static double acc = 0;
    const int64_t now = Now();
    const double dt = last ? double(now - last) / double(g_freq.QuadPart) : 0.0;
    last = now;
    if (!g_hfrOn) { g_repeatTick = 1; acc = 0; return; }
    const double period = mode30 ? 1.0 / 30.0 : 1.0 / 60.0;
    acc += dt;
    if (acc >= period) {
        g_repeatTick = 1;
        acc -= period;
        if (acc > period) acc = period;   // below the original rate: one tick per frame, as in vanilla
    } else g_repeatTick = 0;
}
static void InstallInputRepeat()
{
    if (!Matches("RepeatPad", VA_REPEAT_PAD, kRepeatPadOrig, sizeof kRepeatPadOrig) ||
        !Matches("RepeatKb", VA_REPEAT_KB, kRepeatKbOrig, sizeof kRepeatKbOrig)) {
        Log("Input repeat: signatures do not match. Not applied.");
        return;
    }
    g_repPadCont = VA_REPEAT_PAD + g_delta + sizeof kRepeatPadOrig;
    g_repPadSkip = VA_REPEAT_PAD_SKIP + g_delta;
    g_repKbCont  = VA_REPEAT_KB + g_delta + sizeof kRepeatKbOrig;
    g_repKbSkip  = VA_REPEAT_KB_SKIP + g_delta;
    WriteJmp(VA_REPEAT_PAD, &RepeatPadHook, sizeof kRepeatPadOrig);
    WriteJmp(VA_REPEAT_KB, &RepeatKbHook, sizeof kRepeatKbOrig);
    Log("Input repeat: menu auto-repeat runs at the original rate.");
}

extern "C" void __cdecl LimiterHook()
{
    ++g_frameNo;
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
    WeaponSwitchTick();
    CheatTick();
    TracePads();
    TraceState();
    TraceInput();
    UpdateFrameScalars();
    const bool mode30 = *At<int>(VA_RATE_MODE) == 1;
    UpdateRepeatTick(mode30);
    int fps = g_hfrOn ? (mode30 ? g_menuFps : g_targetFps) : (mode30 ? 30 : 60);
    {   // title screen at TitleFPS; the fixed (30 fps) path is rescaled to match while it is up
        static bool wasTitle = false;
        const bool title = g_hfrOn && g_titleFps > 0 && (g_frameNo - g_titleFrame) < 4;
        if (title) fps = mode30 ? g_titleFps : (g_titleFps < 60 ? 60 : g_titleFps);
        if (title != wasTitle) {
            if (title) g_fixedScale = 30.0f / float(mode30 ? g_titleFps : g_menuFps);
            else ApplyMode();
            Log("Title screen %s (%d fps).", title ? "entered" : "left", fps);
            wasTitle = title;
        }
    }
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
// Effect timing. See docs/RESEARCH.md section 17.
// The effect system (EspListThread) updates every effect once per frame. Most effect code scales by
// the per-effect time step that 0xedfc20 writes into the update context (+0x1C rate, +0x24 mult),
// but some particle types count update calls, so above 60 fps they die early: blood on the floor
// lasted 1-2 s at 120 fps instead of several seconds. Fix: above 60 fps the effect updates run at
// most once per 60 Hz tick, with the time step of all the frames since the last update.
// ---------------------------------------------------------------------------
static const uintptr_t VA_ESP_KICK = 0x00F43A20, VA_ESP_KICK_RESUME = 0x00F43A26;  // EspListThread per-frame start
static const uint8_t   kEspKickOrig[] = { 0x83, 0xEC, 0x0C, 0x53, 0x33, 0xDB };  // SUB ESP,0xC; PUSH EBX; XOR EBX,EBX
// Per-effect update dispatch "MOV r,[vtable+8]; MOV ECX,ESI; CALL r" (3 job loops).
static const uintptr_t VA_ESP_CALL_A = 0x00F41412, VA_ESP_CALL_B = 0x00F41470, VA_ESP_CALL_C = 0x00F41513;
static const uint8_t   kEspCallEdxOrig[] = { 0x8B, 0x42, 0x08, 0x8B, 0xCE };  // MOV EAX,[EDX+8]; MOV ECX,ESI
static const uint8_t   kEspCallEaxOrig[] = { 0x8B, 0x50, 0x08, 0x8B, 0xCE };  // MOV EDX,[EAX+8]; MOV ECX,ESI
// End of the step-context fill: FSTP [ESI+0x24]; MOV ECX,[EDI+0x84]
static const uintptr_t VA_ESP_CTX = 0x00EDFCA5, VA_ESP_CTX_RESUME = 0x00EDFCAE;
static const uint8_t   kEspCtxOrig[] = { 0xD9, 0x5E, 0x24, 0x8B, 0x8F, 0x84, 0x00, 0x00, 0x00 };

extern "C" {
    volatile LONG g_espSkip = 0;      // 1 = skip effect updates this frame
    float     g_espFactor = 1.0f;     // time-step multiplier on update frames
    uintptr_t g_espKickResume = 0, g_espResA = 0, g_espResB = 0, g_espResC = 0, g_espCtxResume = 0;
}
static float g_espAcc = 0.0f;

// Main thread, once per frame, before the effect jobs start.
extern "C" void __cdecl EspFrameTick()
{
    const float m = *reinterpret_cast<const float*>(g_multPtr);
    // Only in gameplay. In the 30 fps mode (menus, title screen) leave every effect update alone:
    // skipping there broke the title screen's eye glow.
    if (!(m > 0.0f) || *At<int>(VA_RATE_MODE) == 1) { g_espSkip = 0; g_espFactor = 1.0f; g_espAcc = 0.0f; return; }
    g_espAcc += m;
    if (m >= 0.95f || g_espAcc >= 0.999f) {
        g_espFactor = g_espAcc / m;
        if (g_espFactor > 8.0f) g_espFactor = 8.0f;   // 360 fps updates every 6th frame
        g_espAcc = 0.0f;
        g_espSkip = 0;
    } else {
        g_espFactor = 1.0f;
        g_espSkip = 1;
    }
}

extern "C" __declspec(naked) void EspNoop() { __asm { ret } }

extern "C" __declspec(naked) void EspKickHook()
{
    __asm {
        pushad
        pushfd
        call  EspFrameTick
        popfd
        popad
        sub   esp, 0x0C
        push  ebx
        xor   ebx, ebx
        jmp   dword ptr [g_espKickResume]
    }
}

extern "C" __declspec(naked) void EspCallHookA()   // vtable in EDX, target in EAX
{
    __asm {
        cmp   dword ptr [g_espSkip], 0
        je    run
        test  byte ptr [esi + 0x6C], 0x10   // timeline-driven effect: never skip
        jz    skip
    run:
        mov   eax, dword ptr [edx + 8]
        mov   ecx, esi
        jmp   dword ptr [g_espResA]
    skip:
        mov   eax, offset EspNoop
        mov   ecx, esi
        jmp   dword ptr [g_espResA]
    }
}

extern "C" __declspec(naked) void EspCallHookB()
{
    __asm {
        cmp   dword ptr [g_espSkip], 0
        je    run
        test  byte ptr [esi + 0x6C], 0x10   // timeline-driven effect: never skip
        jz    skip
    run:
        mov   eax, dword ptr [edx + 8]
        mov   ecx, esi
        jmp   dword ptr [g_espResB]
    skip:
        mov   eax, offset EspNoop
        mov   ecx, esi
        jmp   dword ptr [g_espResB]
    }
}

extern "C" __declspec(naked) void EspCallHookC()   // vtable in EAX, target in EDX
{
    __asm {
        cmp   dword ptr [g_espSkip], 0
        je    run
        test  byte ptr [esi + 0x6C], 0x10   // timeline-driven effect: never skip
        jz    skip
    run:
        mov   edx, dword ptr [eax + 8]
        mov   ecx, esi
        jmp   dword ptr [g_espResC]
    skip:
        mov   edx, offset EspNoop
        mov   ecx, esi
        jmp   dword ptr [g_espResC]
    }
}

extern "C" __declspec(naked) void EspCtxHook()     // ST0 = frame mult for ctx+0x24; ESI = ctx, EDI = effect
{
    __asm {
        test  byte ptr [edi + 0x6C], 0x10   // timeline-driven effect: leave its step alone
        jnz   plain
        fmul  dword ptr [g_espFactor]
        fstp  dword ptr [esi + 0x24]
        fld   dword ptr [esi + 0x1C]
        fmul  dword ptr [g_espFactor]
        fstp  dword ptr [esi + 0x1C]
        mov   ecx, dword ptr [edi + 0x84]
        jmp   dword ptr [g_espCtxResume]
    plain:
        fstp  dword ptr [esi + 0x24]
        mov   ecx, dword ptr [edi + 0x84]
        jmp   dword ptr [g_espCtxResume]
    }
}

static void InstallEffects(const char* ini)
{
    if (!GetPrivateProfileIntA("Fixes", "EffectTimingFix", 1, ini)) { Log("Effect timing fix disabled (EffectTimingFix=0)."); return; }
    bool ok = Matches("EspKick", VA_ESP_KICK, kEspKickOrig, sizeof kEspKickOrig);
    ok &= Matches("EspCallA", VA_ESP_CALL_A, kEspCallEdxOrig, sizeof kEspCallEdxOrig);
    ok &= Matches("EspCallB", VA_ESP_CALL_B, kEspCallEdxOrig, sizeof kEspCallEdxOrig);
    ok &= Matches("EspCallC", VA_ESP_CALL_C, kEspCallEaxOrig, sizeof kEspCallEaxOrig);
    ok &= Matches("EspCtx", VA_ESP_CTX, kEspCtxOrig, sizeof kEspCtxOrig);
    if (!ok) { Log("Effect timing fix: signatures do not match. Not applied."); return; }
    g_multPtr = VA_FRAME_MULT + g_delta;
    g_espKickResume = VA_ESP_KICK_RESUME + g_delta;
    g_espResA = VA_ESP_CALL_A + 5 + g_delta;
    g_espResB = VA_ESP_CALL_B + 5 + g_delta;
    g_espResC = VA_ESP_CALL_C + 5 + g_delta;
    g_espCtxResume = VA_ESP_CTX_RESUME + g_delta;
    // Order: the context hook first (harmless at factor 1), the skips, then the tick that drives them.
    WriteJmp(VA_ESP_CTX, &EspCtxHook, sizeof kEspCtxOrig);
    WriteJmp(VA_ESP_CALL_A, &EspCallHookA, sizeof kEspCallEdxOrig);
    WriteJmp(VA_ESP_CALL_B, &EspCallHookB, sizeof kEspCallEdxOrig);
    WriteJmp(VA_ESP_CALL_C, &EspCallHookC, sizeof kEspCallEaxOrig);
    WriteJmp(VA_ESP_KICK, &EspKickHook, sizeof kEspKickOrig);
    Log("Effect timing fix applied: effects update at <= 60 Hz with the accumulated time step.");
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

static int g_keepGekko = 0;   // [Debris] KeepGekko: 0 = GEKKOs (Em0030) explode and vanish as in vanilla

// Returns 1 to skip the vanish call (body is kept), 0 to let it run.
extern "C" int __cdecl CorpseKeep(uint8_t* obj, int site, int ebxValue)
{
    const CorpseSite& cs = kCorpseSites[site];
    if (cs.kind == 1 && !g_keepGekko) return 0;                                          // GEKKO: explode
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
    Log("Corpses: keeping the last %d enemy bodies (Em0010, Em0030 GEKKO only with KeepGekko=1; %u vanish calls).", g_keepCorpses, (unsigned)_countof(kCorpseSites));
    UnblockDebrisObjectives();
}

// GEKKO (Em0030, "em0030_gekko") is large: cut into many pieces it costs frame time, and the
// pieces block the way. Unless [Debris] KeepGekko=1, its vanish (end of the death animation, where
// it explodes) first drops its debris group from the kept cuts, as for a zandatsu target, so its
// pieces explode with it as in vanilla. Entry detour of 0xb67710: FLDZ; PUSH ESI; SUB ESP,8.
static const uint8_t kVanish0030Orig[] = { 0xD9, 0xEE, 0x56, 0x83, 0xEC, 0x08 };
extern "C" {
    uintptr_t g_vanish0030Resume = 0;
    void __cdecl GekkoVanish(uint8_t* obj) { DebrisNoteZandatsu(*reinterpret_cast<uint32_t*>(obj + 0x83C)); }
}
extern "C" __declspec(naked) void GekkoVanishHook()
{
    __asm {
        push  ecx
        push  edx
        push  ecx
        call  GekkoVanish
        add   esp, 4
        pop   edx
        pop   ecx
        fldz
        push  esi
        sub   esp, 8
        jmp   dword ptr [g_vanish0030Resume]
    }
}

static void InstallGekko(const char* ini)
{
    g_keepGekko = GetPrivateProfileIntA("Debris", "KeepGekko", cfg::kKeepGekko, ini);
    if (g_keepGekko || g_keepCuts <= 0) return;   // corpses alone: CorpseKeep already lets GEKKOs vanish
    if (!Matches("Vanish0030", VA_VANISH_0030, kVanish0030Orig, sizeof kVanish0030Orig)) {
        Log("GEKKO: signature mismatch; their pieces are kept like others.");
        return;
    }
    g_vanish0030Resume = VA_VANISH_0030 + g_delta + sizeof kVanish0030Orig;
    WriteJmp(VA_VANISH_0030, &GekkoVanishHook, sizeof kVanish0030Orig);
    Log("GEKKO: bodies and pieces explode as in vanilla (KeepGekko=0).");
}

#include "photomode.inl"
#include "weaponswitch.inl"
#include "cheats.inl"
#include "buttonicons.inl"

// Logs which XInput slots have a controller, at start and whenever that changes (checked every 2 s on
// the main thread). The game reads pads only through XInput, so a DualSense shows up here only when
// Steam Input (or DS4Windows) presents it as an Xbox controller.
static void TracePads()
{
    static DWORD last = 0;
    static int lastMask = -1;
    const DWORD now = GetTickCount();
    if (lastMask >= 0 && now - last < 2000) return;
    last = now;
    if (!g_xinputGet) {
        if (HMODULE xi = GetModuleHandleA("XINPUT1_3.dll"))
            g_xinputGet = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState"));
        if (!g_xinputGet) return;
    }
    int mask = 0;
    uint8_t state[16];
    for (DWORD i = 0; i < 4; ++i)
        if (g_xinputGet(i, state) == ERROR_SUCCESS) mask |= 1 << i;
    if (mask == lastMask) return;
    lastMask = mask;
    if (!mask) Log("Controllers: no XInput controller connected (a DualSense needs Steam Input or DS4Windows).");
    else Log("Controllers: XInput slots%s%s%s%s connected.", mask & 1 ? " 0" : "", mask & 2 ? " 1" : "", mask & 4 ? " 2" : "", mask & 8 ? " 3" : "");
}

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
    InstallGekko(ini);
    InstallInputRepeat();
    InstallPhotoMode(ini);
    InstallWeaponSwitch(ini);
    InstallCheats();
    InstallButtonIcons(ini);

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
    if (GetPrivateProfileIntA("General", "PreciseFrameTime", 1, ini) &&
        Matches("FrameDelta", VA_FRAME_DELTA, kFrameDeltaOrig, sizeof kFrameDeltaOrig)) {
        g_frameDeltaResume = VA_FRAME_DELTA_RESUME + g_delta;
        WriteJmp(VA_FRAME_DELTA, &FrameDeltaHook, sizeof kFrameDeltaOrig);
        Log("Frame time: measured with double-precision QPC (no float drift in long sessions).");
    }
    if (GetPrivateProfileIntA("Fixes", "CameraTurnFix", 1, ini) &&
        Matches("CamPitch", VA_CAM_PITCH, kCamTurnOrig, sizeof kCamTurnOrig) &&
        Matches("CamYaw", VA_CAM_YAW, kCamTurnOrig, sizeof kCamTurnOrig)) {
        g_camPitchResume = VA_CAM_PITCH_RESUME + g_delta;
        g_camYawResume = VA_CAM_YAW_RESUME + g_delta;
        WriteJmp(VA_CAM_PITCH, &CamPitchHook, sizeof kCamTurnOrig);
        WriteJmp(VA_CAM_YAW, &CamYawHook, sizeof kCamTurnOrig);
        Log("Camera: stick turn speed scaled to real time.");
    }
    Log("Patched: TargetFPS=%d MenuFPS=%d minMult=%.3f fixedScale=%.3f ToggleKey=0x%02X PresetKey=0x%02X base=0x%08X",
        g_targetFps, g_menuFps, g_minMult, g_fixedScale, g_toggleKey, g_presetKey, (unsigned)base);

    // Depends on LimiterHook updating g_qteDecay / g_ticks60 every frame.
    InstallQte(ini);
    InstallBlade(ini);
    InstallEffects(ini);
    InstallTitleFps(ini);
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
