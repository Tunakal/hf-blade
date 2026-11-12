# HF Blade: brief for sub-agents

Read this before you change anything. Your task prompt tells you what to do. This file tells you how
the project works and what you must not break.

## What the project is
- HF Blade is an open-source enhancement mod for *METAL GEAR RISING: REVENGEANCE* (Steam, 32-bit x86
  exe).
- It is a `dinput8.dll` proxy that patches the game in memory. It never modifies game files.
- `hfblade-launcher.exe` is a native Win32 settings app that edits `hfblade.ini`.
- MIT license. The repo is public: github.com/Tunakal/hf-blade.

## Hard rules
- **Never commit, push or make PRs.** The lead agent does that after the user confirms in game.
- **No game code, binaries or Konami art in the repo.** Addresses and short byte signatures are fine.
- **Every patch is signature-checked.** Before writing to game memory, check the original bytes with
  `Matches(name, va, bytes, len)`. If they do not match, log the reason and skip that feature (never
  crash, never half-apply).
  - Addresses are preferred-base VAs (`0x400000`). Always relocate them with `At<T>(va)` or
    `va + g_delta`.
  - Absolute operands inside instructions are relocated by ASLR, so compare them against
    `value + g_delta`.
- **Every new patch site goes into `tests/check_signatures.py`.** Run it, and the result must be
  `RESULT: PASS`.
- **Do not guess addresses.** Use only addresses given in your task or verified in the disassembly. If
  you are unsure, stop and report.
- **Do not touch** `evidence/`, the user's ini in the game folder, or files outside this repo, unless
  the task says so.
- **The game must be closed to install the DLL.** Never kill the game process.
- **Report honestly:** what you changed, what you verified (build, test), and what is untested in game.

## Layout
| File | What it holds |
| --- | --- |
| `src/dllmain.cpp` | Proxy, helpers (`Log`, `Matches`, `WriteBytes`, `WriteJmp`, `WriteCall`, `At<T>`), frame limiter `LimiterHook` (runs once per frame on the main thread), all `Install*` functions, `Install()` order |
| `src/photomode.inl` | Photo mode (F10): world stop, free camera, HUD layer skip (`PhotoUiExecHook`), Wwise pause, XInput (`g_xinputGet`, `XPad`), `GameForeground()`, `KeyDown()`, `g_sndPost` |
| `src/weaponswitch.inl` | D-pad down boss-weapon switch (`WeaponSwitchTick`, called from `LimiterHook`) |
| `src/config.h` | **Single source of defaults** shared by the DLL and the launcher (namespace `cfg`) |
| `src/launcher/launcher.cpp`, `launcher.rc`, `resource.h` | Launcher UI: GDI+ owner-draw, cards (`kCards`), tabs, toggles (`IsToggle`, `SetToggle`/`GetToggle`), tooltips (`AddTooltips`), load/save (`ReadInt`/`WriteInt`) |
| `src/launcher/audio.cpp` | Intro sounds (XAudio2 + Media Foundation) |
| `hfblade.ini` | Shipped default ini, with comments |
| `docs/RESEARCH.md` | Every address and finding, one section per feature (§1–19) |
| `tests/check_signatures.py` | Starts the game suspended and checks every patch site |

## Adding a setting (the usual pattern)
1. Add the default to `src/config.h` (`static const int kFoo = …; // [Section] Key`).
2. In the DLL, read it with `GetPrivateProfileIntA("Section", "Key", cfg::kFoo, ini)` in the feature's
   `Install*`.
3. Add it to `hfblade.ini` with a one-line comment that says what it does for the player.
4. In the launcher, add the control:
   - `resource.h`: a new `IDC_` id;
   - `launcher.rc`: position it inside a card;
   - `kTab*Ids`: so it shows on the right tab;
   - `IsToggle` and the owner-draw list: if it is a toggle;
   - load in the `Set…` block and save in the `WriteInt` block;
   - defaults in the default-ini writer;
   - a tooltip text.
5. Update `README.md` (player-facing, short) and `docs/RESEARCH.md` (addresses, evidence).

## Build and check
- Build: `cmd //c "C:\Users\Asus\Desktop\Projeler\hf-blade\build.bat"` from Bash (VS2022 x86).
  - Outputs: `build\dinput8.dll` and `build\hfblade-launcher.exe`.
  - There must be no `error` and no new `warning C` lines.
- Signatures:
  `py -3 -I tests/check_signatures.py "D:/SteamLibrary/steamapps/common/METAL GEAR RISING REVENGEANCE/METAL GEAR RISING REVENGEANCE.exe"`
  must end with `RESULT: PASS`.
- Install (only if the task says so, and only when the game is not running): copy `build\dinput8.dll`
  (and the launcher if you changed it) into the game folder. Compare file hashes afterwards.
- Runtime log: `hfblade.log` in the game folder. Each feature logs one line at install.

## Style
- C++ with MSVC x86. Naked `__asm` stubs for mid-function hooks. Keep hooks minimal.
- Comments explain *why* and name the game address. No commented-out code.
- Text the player sees (README, ini comments, launcher, tooltips) is plain English, short, and says what
  the option does for the player.
- Prefer real-time scaling with the frame multiplier `[0x01be942c]` (60 fps = 1.0, 120 fps = 0.5) over
  frame counting.
