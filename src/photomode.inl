// ---------------------------------------------------------------------------
// Photo mode (F10): freeze the world, fly a free camera, hide the HUD. See docs/RESEARCH.md §16.
// Included into dllmain.cpp after the patching helpers; uses Log/At/WriteCall/WriteJmp/Matches/g_delta.
//
//  - Freeze: the game's own "stop world" bit 31 of 0x1bea070 (skips AI, objects, slow-rate cascade and
//    the Havok step; rendering and the camera tick keep running). Re-asserted every frame.
//  - Input: 0xa1e8a0 calls a disabled hook 0xdf7c00 ("return 0"); when it returns non-zero the game
//    zeroes all pads. We point that call at PhotoBlockInput.
//  - Camera: cCameraApp::UpdateMatrices 0xdb8090 calls the view builder 0xdb7e00 at 0xdb8097; just
//    before it we write eye/target/up/FOV into the main camera 0x1bea1d0, so view, projection,
//    view-proj and the culling frustum all follow.
//  - HUD: the render-queue executor 0xf9ae10 skips the draw items of the five UI layers.
//  Threads: input and state are updated on the main thread (after Present); the camera hook runs on
//  the game-logic thread and reads a locked snapshot.
// ---------------------------------------------------------------------------
static const uintptr_t VA_WORLD_FLAGS   = 0x01BEA070;   // bit 31 = world stopped
static const uintptr_t VA_MAIN_CAMERA   = 0x01BEA1D0;   // cCameraGame
static const uintptr_t VA_VIEW_BUILD    = 0x00DB7E00;   // thiscall, builds the view from eye/target/up
static const uintptr_t VA_VIEW_CALL     = 0x00DB8097;   // CALL 0xdb7e00 inside UpdateMatrices
static const uintptr_t VA_PAD_GATE_FN   = 0x00DF7C00;   // xor eax,eax; ret
static const uintptr_t VA_PAD_GATE_CALL = 0x00A1E8A7;   // CALL 0xdf7c00 inside the pad copy
static const uintptr_t VA_UI_EXEC       = 0x00F9AE76;   // MOV ECX,[EAX+0xC]; MOV EAX,[EAX+8]
static const uintptr_t VA_UI_EXEC_CONT  = 0x00F9AE7C;
static const uintptr_t VA_UI_EXEC_SKIP  = 0x00F9AE87;
static const uint8_t   kUiExecOrig[]    = { 0x8B, 0x48, 0x0C, 0x8B, 0x40, 0x08 };
// Wwise event posting, as used by the pause menu: post(name, 0) for SE buses, postBgm(name) for music.
static const uintptr_t VA_SND_POST      = 0x00E5E050;   // uint32 __cdecl (const char*, uint32)
static const uintptr_t VA_SND_POST_BGM  = 0x00E5E1B0;   // uint32 __cdecl (const char*)
static const uint8_t   kSndPostOrig[]   = { 0x83, 0xEC, 0x0C, 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x4C, 0x24, 0x14 };
static const uint8_t   kSndPostBgmOrig[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x6A, 0x00, 0x50, 0xE8 };
typedef uint32_t (__cdecl* SndPostFn)(const char*, uint32_t);
typedef uint32_t (__cdecl* SndPostBgmFn)(const char*);
static SndPostFn    g_sndPost = nullptr;
static SndPostBgmFn g_sndPostBgm = nullptr;
static int          g_photoMute = 1;
static bool         g_photoAudioPaused = false;

// Pause/resume the game's audio like its pause menu does (SE, voice and ambience buses), and the
// music the way the cutscene pause menu does. Always paired, so sounds the game paused stay paused.
static void PhotoAudio(bool pause)
{
    if (!g_photoMute || !g_sndPost || !g_sndPostBgm || pause == g_photoAudioPaused) return;
    g_sndPost(pause ? "core_se_sys_se_pause" : "core_se_sys_se_resume", 0);
    g_sndPostBgm(pause ? "bgm_Paused_Movie_Enter" : "bgm_Paused_Movie_Exit");
    g_photoAudioPaused = pause;
}

struct Vec3 { float x, y, z; };
static Vec3  V3(float x, float y, float z) { Vec3 v = { x, y, z }; return v; }
static Vec3  Add(Vec3 a, Vec3 b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec3  Mul(Vec3 a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
static Vec3  Cross(Vec3 a, Vec3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vec3  Norm(Vec3 a) { const float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); return l > 1e-6f ? Mul(a, 1 / l) : V3(0, 0, -1); }

struct PhotoCam { Vec3 eye, target, up; float fov; };
static SRWLOCK  g_photoLock = SRWLOCK_INIT;
static PhotoCam g_photoCam;                 // snapshot for the camera hook
static volatile LONG g_photoActive = 0;     // camera override on
static volatile LONG g_photoRestoreFov = 0; // write g_photoSavedFov once after exit
static float    g_photoSavedFov = 0;
static int      g_photoKey = VK_F10, g_photoEnabled = 1;
static float    g_photoSpeed = 4.0f;        // m/s

extern "C" {
    uintptr_t g_viewBuildAddr = 0;
    volatile LONG g_photoBlockInput = 0;    // read by the pad gate (logic thread)
    volatile LONG g_photoHideUi = 0;        // read by the UI executor (render thread)
    uint32_t  g_uiLayerMask[4] = { 0, 0x40000000u, 0, 0x283u };   // layers 0x3e, 0x60, 0x61, 0x67, 0x69
    uintptr_t g_uiExecCont = 0, g_uiExecSkip = 0;
}

// Free-camera state (main thread only).
static struct {
    bool on = false, hud = false, f10Down = false, hDown = false, padToggleDown = false;
    Vec3 eye;
    float yaw = 0, pitch = 0, roll = 0, fov = 0;
    uint32_t savedWorldBit = 0;
    int releaseWait = 0;
    POINT center = {};
} g_pm;

typedef DWORD (WINAPI* XInputGetStateFn)(DWORD, void*);
static XInputGetStateFn g_xinputGet = nullptr;
struct XPad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };

static float Stick(SHORT v)
{
    const float f = v / 32767.0f, dz = 0.18f;
    if (fabsf(f) < dz) return 0;
    return (f > 0 ? f - dz : f + dz) / (1 - dz);
}

static bool GameForeground()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Called by the game's pad copy instead of the disabled hook: non-zero = zero every pad.
extern "C" int __cdecl PhotoBlockInput(int) { return g_photoBlockInput ? 1 : 0; }

// Camera hook: runs on the logic thread right before the view is built (ECX = camera).
extern "C" void __cdecl PhotoCameraPre(uint8_t* cam)
{
    if (cam != reinterpret_cast<uint8_t*>(VA_MAIN_CAMERA + g_delta)) return;
    if (g_photoRestoreFov) {
        *reinterpret_cast<float*>(cam + 0x94) = g_photoSavedFov;
        g_photoRestoreFov = 0;
    }
    if (!g_photoActive) return;
    AcquireSRWLockShared(&g_photoLock);
    const PhotoCam pc = g_photoCam;
    ReleaseSRWLockShared(&g_photoLock);
    float* eye = reinterpret_cast<float*>(cam + 0x1B0);
    float* tgt = reinterpret_cast<float*>(cam + 0x1C0);
    float* up = reinterpret_cast<float*>(cam + 0x1D0);
    eye[0] = pc.eye.x; eye[1] = pc.eye.y; eye[2] = pc.eye.z;
    tgt[0] = pc.target.x; tgt[1] = pc.target.y; tgt[2] = pc.target.z;
    up[0] = pc.up.x; up[1] = pc.up.y; up[2] = pc.up.z;
    *reinterpret_cast<float*>(cam + 0x4F0) = 0.0f;   // the game's own roll; ours is in `up`
    *reinterpret_cast<float*>(cam + 0x94) = pc.fov;
}

// Replaces "CALL 0xdb7e00" (thiscall, ECX = camera): run our pre-step, then the original.
extern "C" __declspec(naked) void PhotoViewHook()
{
    __asm {
        push  ecx
        push  ecx
        call  PhotoCameraPre
        add   esp, 4
        pop   ecx
        jmp   dword ptr [g_viewBuildAddr]
    }
}

// Replaces "MOV ECX,[EAX+0xC]; MOV EAX,[EAX+8]" in the render-queue executor.
// EAX = command node ([eax] = key: layer bits 23..29, phase bits 20..22), EDX = layer.
extern "C" __declspec(naked) void PhotoUiExecHook()
{
    __asm {
        cmp   dword ptr [g_photoHideUi], 0
        je    orig
        mov   ecx, dword ptr [eax]
        shr   ecx, 20
        and   ecx, 7
        cmp   ecx, 1                 // only the draw items; begin/end callbacks still run
        jne   orig
        bt    dword ptr [g_uiLayerMask], edx
        jc    skip
    orig:
        mov   ecx, dword ptr [eax + 0x0C]
        mov   eax, dword ptr [eax + 0x08]
        jmp   dword ptr [g_uiExecCont]
    skip:
        jmp   dword ptr [g_uiExecSkip]
    }
}

static void PhotoPublish()
{
    const float cp = cosf(g_pm.pitch);
    const Vec3 fwd = V3(-sinf(g_pm.yaw) * cp, sinf(g_pm.pitch), -cosf(g_pm.yaw) * cp);
    const Vec3 right = Norm(Cross(fwd, V3(0, 1, 0)));
    const Vec3 up0 = Cross(right, fwd);
    PhotoCam pc;
    pc.eye = g_pm.eye;
    pc.target = Add(g_pm.eye, fwd);
    pc.up = Add(Mul(up0, cosf(g_pm.roll)), Mul(right, sinf(g_pm.roll)));
    pc.fov = g_pm.fov;
    AcquireSRWLockExclusive(&g_photoLock);
    g_photoCam = pc;
    ReleaseSRWLockExclusive(&g_photoLock);
}

static void PhotoEnter()
{
    uint8_t* cam = reinterpret_cast<uint8_t*>(VA_MAIN_CAMERA + g_delta);
    const float* eye = reinterpret_cast<const float*>(cam + 0x1B0);
    const float* tgt = reinterpret_cast<const float*>(cam + 0x1C0);
    g_pm.eye = V3(eye[0], eye[1], eye[2]);
    const Vec3 d = Norm(V3(tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]));
    g_pm.pitch = asinf(d.y < -1 ? -1 : d.y > 1 ? 1 : d.y);
    g_pm.yaw = atan2f(-d.x, -d.z);
    g_pm.roll = 0;
    g_photoSavedFov = *reinterpret_cast<const float*>(cam + 0x94);
    g_pm.fov = g_photoSavedFov > 0.1f && g_photoSavedFov < 3.0f ? g_photoSavedFov : 0.8727f;
    uint32_t* flags = At<uint32_t>(VA_WORLD_FLAGS);
    g_pm.savedWorldBit = *flags & 0x80000000u;
    *flags |= 0x80000000u;
    PhotoPublish();
    g_pm.on = true;
    g_pm.hud = false;
    g_photoBlockInput = 1;
    g_photoHideUi = 1;
    g_photoActive = 1;
    PhotoAudio(true);
    GetCursorPos(&g_pm.center);
    Log("photo mode: on (eye %.2f %.2f %.2f, fov %.3f)", g_pm.eye.x, g_pm.eye.y, g_pm.eye.z, g_pm.fov);
}

static void PhotoExit()
{
    g_photoActive = 0;
    g_photoRestoreFov = 1;
    g_photoHideUi = 0;
    uint32_t* flags = At<uint32_t>(VA_WORLD_FLAGS);
    *flags = (*flags & ~0x80000000u) | g_pm.savedWorldBit;
    g_pm.on = false;
    PhotoAudio(false);
    g_pm.releaseWait = 90;   // keep the pads blocked until the buttons are released (max ~0.75 s)
    Log("photo mode: off");
}

// Once per frame from LimiterHook (main thread). dt in seconds.
static void PhotoTick(float dt)
{
    if (!g_photoEnabled || !g_viewBuildAddr) return;
    const bool fg = GameForeground();
    const bool f10 = fg && KeyDown(g_photoKey);
    if (f10 && !g_pm.f10Down) { if (g_pm.on) PhotoExit(); else PhotoEnter(); }
    g_pm.f10Down = f10;

    XPad pad = {};
    const bool padOk = g_xinputGet && g_xinputGet(0, &pad) == ERROR_SUCCESS;
    if (!g_pm.on) {
        if (g_pm.releaseWait > 0) {
            const bool held = padOk && (pad.buttons || pad.lt > 30 || pad.rt > 30 || Stick(pad.lx) || Stick(pad.ly));
            if (!held || --g_pm.releaseWait == 0) { g_pm.releaseWait = 0; g_photoBlockInput = 0; }
        }
        return;
    }
    *At<uint32_t>(VA_WORLD_FLAGS) |= 0x80000000u;   // scene init may rewrite the flags; keep the world stopped
    if (dt > 0.1f) dt = 0.1f;

    // HUD toggle: H or pad X.
    const bool hKey = fg && KeyDown('H');
    if (hKey && !g_pm.hDown) g_pm.hud = !g_pm.hud;
    g_pm.hDown = hKey;
    const bool xBtn = padOk && (pad.buttons & 0x4000);
    if (xBtn && !g_pm.padToggleDown) g_pm.hud = !g_pm.hud;
    g_pm.padToggleDown = xBtn;
    g_photoHideUi = g_pm.hud ? 0 : 1;

    float mx = 0, mz = 0, my = 0, look_x = 0, look_y = 0, roll = 0, zoom = 0;
    bool fast = false, slow = false;
    if (padOk) {
        mx += Stick(pad.lx); mz += Stick(pad.ly);
        look_x += Stick(pad.rx); look_y += Stick(pad.ry);
        my += (pad.rt - pad.lt) / 255.0f;
        if (pad.buttons & 0x0100) roll -= 1;   // LB
        if (pad.buttons & 0x0200) roll += 1;   // RB
        if (pad.buttons & 0x0001) zoom -= 1;   // d-pad up: narrower FOV (zoom in)
        if (pad.buttons & 0x0002) zoom += 1;   // d-pad down
        if (pad.buttons & 0x0040) fast = true; // L3
        if (pad.buttons & 0x8000) { g_pm.roll = 0; g_pm.fov = g_photoSavedFov > 0.1f ? g_photoSavedFov : g_pm.fov; }   // Y: reset
    }
    if (fg) {
        if (KeyDown('W')) mz += 1;
        if (KeyDown('S')) mz -= 1;
        if (KeyDown('D')) mx += 1;
        if (KeyDown('A')) mx -= 1;
        if (KeyDown(VK_SPACE)) my += 1;
        if (KeyDown(VK_CONTROL)) my -= 1;
        if (KeyDown('Q')) roll -= 1;
        if (KeyDown('E')) roll += 1;
        if (KeyDown('Z') || KeyDown(VK_PRIOR)) zoom -= 1;
        if (KeyDown('X') || KeyDown(VK_NEXT)) zoom += 1;
        if (KeyDown(VK_LEFT)) look_x -= 1;
        if (KeyDown(VK_RIGHT)) look_x += 1;
        if (KeyDown(VK_UP)) look_y += 1;
        if (KeyDown(VK_DOWN)) look_y -= 1;
        if (KeyDown(VK_SHIFT)) fast = true;
        if (KeyDown(VK_MENU)) slow = true;
        if (KeyDown('R')) { g_pm.roll = 0; g_pm.fov = g_photoSavedFov > 0.1f ? g_photoSavedFov : g_pm.fov; }
        POINT p;   // mouse look: cursor delta, re-centred every frame
        if (GetCursorPos(&p)) {
            g_pm.yaw -= (p.x - g_pm.center.x) * 0.0025f;
            g_pm.pitch -= (p.y - g_pm.center.y) * 0.0025f;
            SetCursorPos(g_pm.center.x, g_pm.center.y);
        }
    }
    const float lookRate = 1.8f;   // rad/s at full stick
    g_pm.yaw -= look_x * lookRate * dt;
    g_pm.pitch += look_y * lookRate * dt;
    if (g_pm.pitch > 1.55f) g_pm.pitch = 1.55f;
    if (g_pm.pitch < -1.55f) g_pm.pitch = -1.55f;
    g_pm.roll += roll * 1.2f * dt;
    g_pm.fov *= expf(zoom * 0.9f * dt);
    if (g_pm.fov < 0.09f) g_pm.fov = 0.09f;   // ~5 deg
    if (g_pm.fov > 2.1f) g_pm.fov = 2.1f;     // ~120 deg

    const float speed = g_photoSpeed * (fast ? 4.0f : 1.0f) * (slow ? 0.25f : 1.0f) * dt;
    const float cp = cosf(g_pm.pitch);
    const Vec3 fwd = V3(-sinf(g_pm.yaw) * cp, sinf(g_pm.pitch), -cosf(g_pm.yaw) * cp);
    const Vec3 right = Norm(Cross(fwd, V3(0, 1, 0)));
    g_pm.eye = Add(g_pm.eye, Add(Add(Mul(fwd, mz * speed), Mul(right, mx * speed)), V3(0, my * speed, 0)));
    PhotoPublish();
}

static void InstallPhotoMode(const char* ini)
{
    g_photoEnabled = GetPrivateProfileIntA("PhotoMode", "Enabled", cfg::kPhotoMode, ini);
    if (!g_photoEnabled) { Log("Photo mode: disabled."); return; }
    g_photoKey = GetPrivateProfileIntA("PhotoMode", "Key", cfg::kPhotoKey, ini);
    g_photoSpeed = GetPrivateProfileIntA("PhotoMode", "SpeedCmPerSec", cfg::kPhotoSpeed, ini) / 100.0f;
    g_photoMute = GetPrivateProfileIntA("PhotoMode", "PauseAudio", 1, ini);

    bool ok = Matches("UiExec", VA_UI_EXEC, kUiExecOrig, sizeof kUiExecOrig);
    const struct { uintptr_t site, target; const char* name; } calls[] = {
        { VA_VIEW_CALL, VA_VIEW_BUILD, "ViewCall" }, { VA_PAD_GATE_CALL, VA_PAD_GATE_FN, "PadGate" } };
    for (auto& c : calls) {
        const uint8_t* p = At<uint8_t>(c.site);
        if (p[0] != 0xE8 || c.site + g_delta + 5 + *reinterpret_cast<const int32_t*>(p + 1) != c.target + g_delta) {
            Log("Signature mismatch at %s (0x%08X).", c.name, (unsigned)(c.site + g_delta));
            ok = false;
        }
    }
    if (!ok) { Log("Photo mode: signatures do not match. Not applied."); g_photoEnabled = 0; return; }

    if (Matches("SndPost", VA_SND_POST, kSndPostOrig, sizeof kSndPostOrig) &&
        Matches("SndPostBgm", VA_SND_POST_BGM, kSndPostBgmOrig, sizeof kSndPostBgmOrig)) {
        g_sndPost = reinterpret_cast<SndPostFn>(VA_SND_POST + g_delta);
        g_sndPostBgm = reinterpret_cast<SndPostBgmFn>(VA_SND_POST_BGM + g_delta);
    } else Log("Photo mode: sound functions not matched; audio keeps playing.");
    if (HMODULE xi = GetModuleHandleA("XINPUT1_3.dll"))
        g_xinputGet = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState"));
    g_viewBuildAddr = VA_VIEW_BUILD + g_delta;
    g_uiExecCont = VA_UI_EXEC_CONT + g_delta;
    g_uiExecSkip = VA_UI_EXEC_SKIP + g_delta;
    WriteCall(VA_VIEW_CALL, &PhotoViewHook, 5);
    WriteCall(VA_PAD_GATE_CALL, &PhotoBlockInput, 5);
    WriteJmp(VA_UI_EXEC, &PhotoUiExecHook, sizeof kUiExecOrig);
    Log("Photo mode: ready (key 0x%02X, gamepad %s).", g_photoKey, g_xinputGet ? "yes" : "no");
}
