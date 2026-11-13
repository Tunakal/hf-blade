// ---------------------------------------------------------------------------
// Cheats ([Cheats] in hfblade.ini). See docs/RESEARCH.md section 22.
// Included into dllmain.cpp after the patching helpers; uses Log/At/WriteJmp/Matches/g_delta/g_ini.
//
//  - Infinite health: the player's HP [pl+0x870] is set to its maximum every frame. The maximum comes
//    from the game's own getter 0xb7c980 (thiscall), the one the Repair Unit uses (0xb877b0).
//  - Infinite fuel cells: every cell (24-byte entries at [pl+0x3820], count [pl+0x3828], value +4,
//    max +0xC, +0x14 != 0 = unusable) is set to its maximum every frame. In the single-bar mode
//    (bit 31 of 0x1bea090) the bar is [pl+0x3830] with max [pl+0x3834].
//  - Infinite Repair Units / Electrolyte packs: the item consume function 0x94d8d0 (DEC [ECX+0x54])
//    skips these two items (item id at +0x10: 0x23a6f56d = Repair Unit, 0x0d92bb0f = Electrolyte).
//  All of them are gated by flags read live from the ini (checked about once a second), so turning a
//  cheat off in the launcher takes it out of the running game: nothing is written any more and items
//  are used up again. Nothing is saved by the mod; the game only saves what it already holds.
// ---------------------------------------------------------------------------
static const uintptr_t VA_CH_PLAYER    = 0x01DC14C8;   // player object pointer
static const uintptr_t VA_CH_GAMEFLAGS = 0x01BEA090;   // bit 31: single-bar mode
static const uintptr_t VA_CH_MAXHP     = 0x00B7C980;   // int __thiscall maxHp(player, int unused): RET 4
static const uintptr_t VA_CH_CONSUME   = 0x0094D8D0;   // item consume: DEC [ECX+0x54]; MOV EAX,[ECX+0x54]
static const uint8_t   kChMaxHpOrig[]  = { 0x56, 0x57, 0x8B, 0xF1 };
static const uint8_t   kChConsumeOrig[] = { 0xFF, 0x49, 0x54, 0x8B, 0x41, 0x54 };
typedef int (__thiscall* MaxHpFn)(void*, int);   // the game calls it with 0 (0xb877b0)
static MaxHpFn g_chMaxHp = nullptr;
static int     g_chHealth = 0, g_chFuel = 0, g_chReady = 0;
static FILETIME g_chIniTime = {};

extern "C" {
    volatile LONG g_chItems = 0;
    uintptr_t g_chConsumeCont = 0;
}

// Entry detour of the item consume function (thiscall, ECX = item, plain RET).
extern "C" __declspec(naked) void CheatConsumeHook()
{
    __asm {
        cmp   dword ptr [g_chItems], 0
        je    orig
        cmp   dword ptr [ecx + 0x10], 0x23A6F56D   // Repair Unit
        je    keep
        cmp   dword ptr [ecx + 0x10], 0x0D92BB0F   // Electrolyte pack
        je    keep
    orig:
        dec   dword ptr [ecx + 0x54]
        mov   eax, dword ptr [ecx + 0x54]
        jmp   dword ptr [g_chConsumeCont]
    keep:
        mov   eax, dword ptr [ecx + 0x54]
        ret
    }
}

static void CheatReadIni(bool log)
{
    const int health = GetPrivateProfileIntA("Cheats", "InfiniteHealth", 0, g_ini);
    const int fuel = GetPrivateProfileIntA("Cheats", "InfiniteFuelCells", 0, g_ini);
    const int items = GetPrivateProfileIntA("Cheats", "InfiniteRepairItems", 0, g_ini);
    if (log || health != g_chHealth || fuel != g_chFuel || items != (int)g_chItems)
        Log("Cheats: health %s, fuel cells %s, repair items %s.", health ? "on" : "off", fuel ? "on" : "off", items ? "on" : "off");
    g_chHealth = health;
    g_chFuel = fuel;
    g_chItems = g_chReady ? items : 0;
}

// Once per frame from LimiterHook (main thread).
static void CheatTick()
{
    if (!g_chReady) return;
    static DWORD lastCheck = 0;
    const DWORD now = GetTickCount();
    if (now - lastCheck > 1000) {   // the launcher may have changed the ini while the game runs
        lastCheck = now;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &g_chIniTime) != 0) {
            g_chIniTime = fa.ftLastWriteTime;
            CheatReadIni(false);
        }
    }
    if (!g_chHealth && !g_chFuel) return;
    uint8_t* pl = *At<uint8_t*>(VA_CH_PLAYER);
    if (!pl || *reinterpret_cast<int*>(pl + 0x4E4) != 0) return;   // no player, or dying
    if (g_chHealth) {
        const int maxHp = g_chMaxHp(pl, 0);
        int& hp = *reinterpret_cast<int*>(pl + 0x870);
        if (maxHp > 0 && hp < maxHp) hp = maxHp;   // also when a hit took it to 0 this frame
    }
    if (g_chFuel) {
        if (*At<uint32_t>(VA_CH_GAMEFLAGS) & 0x80000000u) {
            float* bar = reinterpret_cast<float*>(pl + 0x3830);
            if (bar[1] > 0 && bar[0] < bar[1]) bar[0] = bar[1];
        } else {
            uint8_t* cells = *reinterpret_cast<uint8_t**>(pl + 0x3820);
            const int n = *reinterpret_cast<int*>(pl + 0x3828);
            if (cells && n > 0 && n <= 16)
                for (int i = 0; i < n; ++i) {
                    uint8_t* c = cells + i * 0x18;
                    float& v = *reinterpret_cast<float*>(c + 4);
                    const float m = *reinterpret_cast<float*>(c + 0xC);
                    if (*reinterpret_cast<int*>(c + 0x14) == 0 && m > 0 && v < m) v = m;
                }
        }
    }
}

static void InstallCheats()
{
    if (!Matches("ChMaxHp", VA_CH_MAXHP, kChMaxHpOrig, sizeof kChMaxHpOrig) ||
        !Matches("ChConsume", VA_CH_CONSUME, kChConsumeOrig, sizeof kChConsumeOrig)) {
        Log("Cheats: signatures do not match. Not available.");
        return;
    }
    g_chMaxHp = reinterpret_cast<MaxHpFn>(VA_CH_MAXHP + g_delta);
    g_chConsumeCont = VA_CH_CONSUME + g_delta + sizeof kChConsumeOrig;
    WriteJmp(VA_CH_CONSUME, &CheatConsumeHook, sizeof kChConsumeOrig);
    g_chReady = 1;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa)) g_chIniTime = fa.ftLastWriteTime;
    CheatReadIni(true);
}
