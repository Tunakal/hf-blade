// Quick unique-weapon switch: one button cycles Raiden's unique (heavy attack) weapon through the ones
// he owns, using the same two steps as the game's weapon menu. See docs/RESEARCH.md section 19.
// Included from dllmain.cpp after photomode.inl (uses g_xinputGet, g_sndPost, GameForeground, KeyDown).

static const uintptr_t VA_WS_PLAYER     = 0x01DC14C8;   // player object pointer
static const uintptr_t VA_WS_GAMESTATE  = 0x01BEA030;
static const uintptr_t VA_WS_MODEFLAGS  = 0x018B9140;   // [+0x34] & 0xF00 == 0xC00 / 0xD00: menu hides these slots
// Unique (boss) weapon slot, confirmed in game by watching a change made in the weapon menu:
// equipped kind 0x1BE9FD4, requested 0x1BE9FD8 (bit 31 = flag), loading 0x1BE9FDC.
static const uintptr_t VA_WS_EQUIPPED   = 0x01BE9FD4;
static const uintptr_t VA_WS_REQUESTED  = 0x01BE9FD8;
static const uintptr_t VA_WS_LOADING    = 0x01BE9FDC;
static const uintptr_t VA_WS_DIRTY      = 0x01DC074C;   // set by the menu after a change
static const uintptr_t VA_WS_TABLE      = 0x016566A4;   // 3 x {item id, menu value}
static const uintptr_t VA_WS_OWNED      = 0x009C4900;   // owned(item id), ret 4, ecx unused
static const uintptr_t VA_WS_DETACH     = 0x00B7D0E0;   // player->(), before a request
static const uintptr_t VA_WS_REQUEST    = 0x00B7F5E0;   // player->(kind)
static const uintptr_t VA_WS_BUSY       = 0x00B7D0C0;   // player->() != 0 while it cannot attach
static const uintptr_t VA_WS_ATTACH_VT  = 0x3D8;        // player vtable slot that attaches the loaded weapon
static const uintptr_t VA_WS_MENU_REQ   = 0x00992CCE;   // menu: MOV [0x1BE9FD8],EAX (signature only)
// Persistent "equipped boss weapon" record. The menu's commit writes it (0x9B31A1: MOV [0x1B7588C],2/3/4)
// and the game refills the slot from it (0x9BBED3). Without it the game kept treating the old weapon as
// equipped, re-requested it and crashed in the motion code with the new weapon's data.
static const uintptr_t VA_WS_SAVED      = 0x01B7588C;
static const uintptr_t VA_WS_MENU_SAVE  = 0x009B31A1;   // C7 05 <0x1B7588C> 02 00 00 00 (signature only)

static const uint8_t kWsDetachOrig[] = { 0x56, 0x8D, 0xB1, 0xF8, 0x0F, 0x00, 0x00 };
static const uint8_t kWsBusyOrig[]   = { 0x81, 0xC1, 0xF8, 0x0F, 0x00, 0x00 };
static const uint8_t kWsOwnedOrig[]  = { 0x8B, 0x44, 0x24, 0x04, 0x8D, 0x48, 0xF3 };
static const uint8_t kWsReqOrig[]    = { 0xE8, 0x3B, 0x43, 0x09, 0x00, 0x8B, 0x10 };   // relative CALL, position-independent
static const uint8_t kWsMenuReqOp    = 0xA3;
static const int     kWsValueToKind[3] = { 2, 3, 4 };   // menu value -> weapon kind (menu 0x992C6F)

typedef int  (__stdcall* WsOwnedFn)(int item);
typedef void (__fastcall* WsVoidFn)(void* self, void* edx);
typedef void (__fastcall* WsArgFn)(void* self, void* edx, int v);
typedef int  (__fastcall* WsBusyFn)(void* self, void* edx);

static struct {
    bool  enabled = false;
    WORD  padButton = 0x0002;   // XInput D-pad down
    int   key = 0;
    bool  padDown = false, keyDown = false;
    bool  pending = false;
    bool  queued = false;
    ULONGLONG queuedAt = 0;
    int   quietMs = 300;
    int   settleMs = 750;
    int   pendingFrames = 0;
    int   fromKind = 0, toKind = 0;
} g_ws;

static void* WsPlayer() { return *At<void*>(VA_WS_PLAYER); }

// Owned boss weapons (kinds) in the menu's order, as built at 0x9933DF.
static int WsOwned(int* kinds)
{
    int n = 0;
    auto owned = reinterpret_cast<WsOwnedFn>(VA_WS_OWNED + g_delta);
    for (int i = 0; i < 3; ++i) {
        const int item = *At<int>(VA_WS_TABLE + i * 8);
        const int val  = *At<int>(VA_WS_TABLE + i * 8 + 4);
        if (val < 0 || val > 2 || !owned(item)) continue;
        kinds[n++] = kWsValueToKind[val];
    }
    return n;
}

static bool WsGameplay()
{
    if (g_pm.on) return false;
    if (*At<int>(VA_RATE_MODE) == 1) return false;                  // menus, codec, cutscenes
    if (*At<uint32_t>(VA_WORLD_FLAGS) & 0x80000000u) return false;  // world stopped
    const int st = *At<int>(VA_WS_GAMESTATE);
    if (st == 4 || st == 5 || st == 7) return false;
    const uint32_t mf = *At<uint32_t>(VA_WS_MODEFLAGS + 0x34) & 0xF00;
    if (mf == 0xC00 || mf == 0xD00) return false;
    return WsPlayer() != nullptr;
}

static void WsRequestNext()
{
    if (*At<uint32_t>(VA_WS_LOADING) != 0) return;
    int kinds[3];
    const int n = WsOwned(kinds);
    const int cur = *At<int>(VA_WS_EQUIPPED);
    if (n == 0 || (n == 1 && kinds[0] == cur)) { Log("Weapon switch: %d boss weapon(s) owned, nothing to switch.", n); return; }
    int next = kinds[0];
    for (int i = 0; i < n; ++i) if (kinds[i] == cur) { next = kinds[(i + 1) % n]; break; }
    if (next == cur) return;

    void* pl = WsPlayer();
    const int st = *At<int>(VA_WS_GAMESTATE);
    *At<int>(VA_WS_SAVED) = next;   // as the menu's commit does
    reinterpret_cast<WsVoidFn>(VA_WS_DETACH + g_delta)(pl, nullptr);
    *At<uint32_t>(VA_WS_REQUESTED) = uint32_t(next) | ((unsigned)(st - 8) <= 1 ? 0x80000000u : 0u);
    reinterpret_cast<WsArgFn>(VA_WS_REQUEST + g_delta)(pl, nullptr, next);
    *At<uint32_t>(VA_WS_DIRTY) = 1;
    g_ws.pending = true;
    g_ws.pendingFrames = 0;
    g_ws.fromKind = cur;
    g_ws.toKind = next;
    if (g_sndPost) g_sndPost("core_se_sys_custom_item_equip", 0);
    Log("Weapon switch: boss weapon %d -> %d requested (%d owned).", cur, next, n);
}

static void WsFinish()
{
    void* pl = WsPlayer();
    if (!pl) { g_ws.pending = false; return; }
    if (++g_ws.pendingFrames > 60 * 10) {
        Log("Weapon switch: weapon %d did not finish loading; giving up.", g_ws.toKind);
        g_ws.pending = false;
        return;
    }
    if (*At<uint32_t>(VA_WS_LOADING) != 0) return;
    if (int(*At<uint32_t>(VA_WS_REQUESTED) & 0x7FFFFFFFu) != *At<int>(VA_WS_EQUIPPED)) return;
    if (reinterpret_cast<WsBusyFn>(VA_WS_BUSY + g_delta)(pl, nullptr) != 0) return;
    void** vt = *reinterpret_cast<void***>(pl);
    reinterpret_cast<WsVoidFn>(vt[VA_WS_ATTACH_VT / 4])(pl, nullptr);
    g_ws.pending = false;
    Log("Weapon switch: weapon %d attached.", g_ws.toKind);
}

// Main thread, once per frame (LimiterHook).
static void WeaponSwitchTick()
{
    if (!g_ws.enabled) return;
    if (!g_xinputGet) {
        if (HMODULE xi = GetModuleHandleA("XINPUT1_3.dll"))
            g_xinputGet = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState"));
    }
    const bool fg = GameForeground();
    XPad pad = {};
    const bool padNow = fg && g_xinputGet && g_xinputGet(0, &pad) == ERROR_SUCCESS && (pad.buttons & g_ws.padButton);
    const bool keyNow = fg && g_ws.key && KeyDown(g_ws.key);
    const bool pressed = (padNow && !g_ws.padDown) || (keyNow && !g_ws.keyDown);
    g_ws.padDown = padNow;
    g_ws.keyDown = keyNow;

    // Swapping while an attack animation still uses the boss weapon crashes the game. The menu never
    // hits this because it pauses the game, so the switch is queued until attacks have been idle for a
    // while: no X/Y/B (pad) or mouse attack button held, and the last one released >= QuietMs ago.
    static ULONGLONG lastAttack = 0;
    const WORD attackMask = 0x4000 | 0x8000 | 0x2000;   // X, Y, B
    const bool attackHeld = (fg && g_xinputGet && (pad.buttons & attackMask)) ||
                            (fg && (KeyDown(VK_LBUTTON) || KeyDown(VK_RBUTTON) || KeyDown(VK_MBUTTON)));
    const ULONGLONG now = GetTickCount64();
    if (attackHeld) lastAttack = now;

    if (g_ws.pending) { WsFinish(); return; }
    if (pressed) {
        if (!g_ws.queued) Log("Weapon switch: queued until attacks are idle.");
        g_ws.queued = true;
        g_ws.queuedAt = now;
    }
    // Player flags measured in game (Frida, 30 Hz with the Y button): [player+0x11A4] is 1 during attack
    // animations; [player+0x2658] is 0 from the attack until the weapon is put away (1 when idle).
    // Swapping before the weapon is put away crashes, so both must be settled for SettleMs.
    static ULONGLONG lastBusy = 0;
    if (void* plNow = WsPlayer()) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(plNow);
        if (*reinterpret_cast<const int*>(p + 0x11A4) != 0 || *reinterpret_cast<const int*>(p + 0x2658) == 0) lastBusy = now;
    }
    if (!g_ws.queued) return;
    if (now - g_ws.queuedAt > 10000) { g_ws.queued = false; Log("Weapon switch: dropped, no idle moment within 10 s."); return; }
    if (attackHeld || now - lastAttack < (ULONGLONG)g_ws.quietMs || now - lastBusy < (ULONGLONG)g_ws.settleMs) return;
    if (!WsGameplay()) return;
    g_ws.queued = false;
    WsRequestNext();
}

static void InstallWeaponSwitch(const char* ini)
{
    if (!GetPrivateProfileIntA("WeaponSwitch", "Enabled", cfg::kWeaponSwitch, ini)) { Log("Weapon switch disabled."); return; }
    g_ws.padButton = WORD(GetPrivateProfileIntA("WeaponSwitch", "PadButton", cfg::kWeaponSwitchPad, ini));
    g_ws.key = GetPrivateProfileIntA("WeaponSwitch", "Key", cfg::kWeaponSwitchKey, ini);
    g_ws.quietMs = GetPrivateProfileIntA("WeaponSwitch", "QuietMs", 300, ini);
    if (g_ws.quietMs < 0) g_ws.quietMs = 0;
    g_ws.settleMs = GetPrivateProfileIntA("WeaponSwitch", "SettleMs", 750, ini);
    if (g_ws.settleMs < 100) g_ws.settleMs = 100;
    bool ok = Matches("WsDetach", VA_WS_DETACH, kWsDetachOrig, sizeof kWsDetachOrig);
    ok &= Matches("WsBusy", VA_WS_BUSY, kWsBusyOrig, sizeof kWsBusyOrig);
    ok &= Matches("WsOwned", VA_WS_OWNED, kWsOwnedOrig, sizeof kWsOwnedOrig);
    ok &= Matches("WsRequest", VA_WS_REQUEST, kWsReqOrig, sizeof kWsReqOrig);
    const uint8_t* req = At<uint8_t>(VA_WS_MENU_REQ);
    ok &= req[0] == kWsMenuReqOp && *reinterpret_cast<const uint32_t*>(req + 1) == uint32_t(VA_WS_REQUESTED + g_delta);
    const uint8_t* sv = At<uint8_t>(VA_WS_MENU_SAVE);
    ok &= sv[0] == 0xC7 && sv[1] == 0x05 && *reinterpret_cast<const uint32_t*>(sv + 2) == uint32_t(VA_WS_SAVED + g_delta) &&
          *reinterpret_cast<const uint32_t*>(sv + 6) == 2;
    if (!ok) { Log("Weapon switch: signatures do not match. Not enabled."); return; }
    g_ws.enabled = true;
    Log("Weapon switch: ready (pad mask 0x%04X, key 0x%02X).", g_ws.padButton, g_ws.key);
}
