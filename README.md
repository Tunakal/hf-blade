# HF Blade

An open-source enhancement mod for *METAL GEAR RISING: REVENGEANCE* (PC, Steam). The goal is a
remaster-like experience:
- high frame rate (60–240 fps) with real-time gameplay, QTE and Blade Mode timing;
- 16:10, wide and ultrawide with an aspect-correct FOV;
- persistent cut debris and enemy bodies;
- a photo mode (freeze, free camera, hidden HUD);
- a settings launcher.

> Status: **work in progress.** Confirmed in game: 120 fps, the unlocked menus, 16:10, the
> Sundowner shield QTE, persistent bodies and debris (including objective progress and zandatsu),
> and the photo mode (freeze, free camera, hidden HUD, paused audio). Ultrawide and frame rates other
> than 120 are untested. Keep a backup of your save.

## Why the game breaks above 60 fps

The engine scales its simulation by a per-frame multiplier measured in 60 fps units. The multiplier is
clamped to the range 1.0–2.0. This lets the game slow down gracefully to 30 fps, but above 60 fps it
stays at 1.0, so the game runs too fast. The frame limiter is also hard-coded to 30 or 60 fps.

See [docs/RESEARCH.md](docs/RESEARCH.md) for addresses and evidence.

## What it does

The mod is a `dinput8.dll` proxy. It loads alongside dgVoodoo and ReShade without conflicts.

1. **Frame-multiplier clamp.** The minimum is lowered from `1.0` to `60 / TargetFPS`, which is 0.5 at
   120 fps.
2. **Frame limiter.** The game's Sleep-based 30/60 limiter is replaced with a QPC deadline limiter that
   spins for the last ~1.5 ms. Scenes the game runs at 30 fps stay at 30 fps.

3. **16:10, wide and ultrawide (`[Display] Width`/`Height`).** The in-game "1920x1080" option is
   rendered at the size you choose, with no black bars. Examples: 1920×1200, 2560×1080, 3440×1440,
   5120×1440. The camera aspect follows the chosen size. With `VertPlusFov`, screens taller than
   16:9 get more view above and below (Vert+), wider screens keep the game's Hor+, and the 3D HUD
   layer is sized for the real height. Use fullscreen.
   - On 16:10 the 2D HUD is stretched about 11% vertically.
   - On ultrawide the 2D HUD is stretched horizontally. A centred 16:9 HUD is planned.
   - Ultrawide is untested in game.
4. **Persistent debris and bodies.** The pieces of your last cuts and the bodies of the last killed
   enemies stay on the ground instead of exploding and vanishing.
5. **Photo mode (F10).** Freezes the game, hides the HUD and gives you a free camera. See
   [Photo mode](#photo-mode) below.
6. **Quick weapon switch (D-pad down).** Switches the unique (heavy attack) weapon to the next one you
   own, without opening the weapon menu (`[WeaponSwitch]`; the button and an optional keyboard key are
   configurable).

The mod checks the exe's PE timestamp and the bytes at each patch site before changing anything. If
any check fails it applies no patches and writes the reason to `hfblade.log`. Patches live only in
memory. Game files are never modified.

## Build

You need Visual Studio 2022 with the C++ x86 tools. Run `build.bat`. The outputs are
`build\dinput8.dll` (the mod) and `build\hfblade-launcher.exe` (an optional settings launcher).

## Install / uninstall

Copy `build\dinput8.dll`, `hfblade.ini` and, if you want it, `build\hfblade-launcher.exe` next to
`METAL GEAR RISING REVENGEANCE.exe`. To uninstall, delete those files.

## Settings

Settings live in `hfblade.ini`. An older `mgr-hfr.ini` from before the rename is copied over
automatically on first start.

- **Launcher art (optional).** Put an image named `hfblade-launcher-art.jpg` or `.png` next to the launcher
  and it is shown in the left column. No game art ships with this project.
- **Launcher sounds (optional).** Put `.mp3` or `.wav` files in a `hfblade-sounds` folder next to the
  launcher. They play with the opening cuts at a random pitch, volume and stereo position. The speaker
  button in the top bar mutes them (`[Launcher] Sound`). No sound files ship with this project.
- **Launcher.** `hfblade-launcher.exe` edits `hfblade.ini` in two tabs:
  - **Gameplay:** FPS preset (60/90/120/144/165), menu FPS, the fixes, QTE ease, debris and bodies;
  - **Display:** render size and FOV.

  Hover over a setting to see what it does. The opening animation can be a flurry of cuts, a single
  cut or off, and has a preview button. **Save & Play** starts the game through Steam. Starting the
  game from the Steam library also works, because the mod always reads the ini.
- **In game:**
  - **F8** switches to the next FPS preset and saves it to the ini;
  - **F9** toggles between the mod and the original 60/30 fps.
- **Debris** (`[Debris] KeepLastCuts`, default 2): the pieces of your last N cuts stay on the ground
  instead of exploding and vanishing. Older cuts are cleaned up normally.
- **Bodies** (`[Debris] KeepCorpses`, default 3): the bodies of the last N killed enemies stay on the
  ground. So far this covers the common cyborg soldiers and one other enemy type.
  "Kill all enemies" objectives complete as soon as the last enemy is dead, even if bodies or pieces
  are still lying around. The target of a zandatsu (spine grab) still disappears as in the original
  game.
- **Photo mode** (`[PhotoMode]`): see [Photo mode](#photo-mode).
- Any other `TargetFPS` from 60 to 240 can be set in the ini. The launcher shows it as "Custom".

To actually reach 120 fps you need a 120 Hz+ display, or V-Sync turned off (for example in dgVoodoo).
With `LogStats=1`, `hfblade.log` reports fps and the multiplier every 600 frames. At 120 fps the
multiplier should be about 0.5.

## Photo mode

Press **F10** during gameplay. The game freezes where it is (enemies, effects and flying pieces
included), game sounds and music pause, and the camera comes loose. Take the picture with **F12**
(Steam) or **PrtSc** (ReShade, if installed), then press **F10** again to continue exactly where
you stopped.

| Action | Controller | Keyboard + mouse |
| --- | --- | --- |
| Move | Left stick | W A S D |
| Up / down | RT / LT | Space / Ctrl |
| Look | Right stick | Mouse or arrow keys |
| Roll | LB / RB | Q / E |
| Zoom (FOV) | D-pad up / down | Z / X or Page Up / Page Down |
| Faster / slower | L3 (faster) | Shift (faster) / Alt (slower) |
| Reset roll and FOV | Y | R |
| Show / hide HUD | X | H |
| Exit | F10 | F10 |

Settings in `hfblade.ini` under `[PhotoMode]`:
- `Enabled`;
- `Key` (virtual-key code, default 121 = F10);
- `SpeedCmPerSec` (camera speed, default 400);
- `PauseAudio` (default 1).

The launcher's Photo Mode tab shows the same controls.

## Known issues / open questions

- **Antivirus:** some antivirus products quarantine the `dinput8.dll` proxy as a heuristic false
  positive, because it loads into the game and patches its memory. Kaspersky has been seen doing this.
  Build it from source if you prefer, and add an exclusion for the game folder.
- Button-mash QTEs, Blade Mode slow motion and the Blade Mode energy drain are scaled to real time
  (`[Fixes]`). This includes Sundowner's shield QTE.
- Some particle effects (for example blood on the floor) count frames, so above 60 fps they used to
  vanish too early. With `EffectTimingFix` (default on) effects update at 60 Hz with the right time
  step, so they last as long as in the original game. `QteEase` (default 60%) also makes mash gauges drain slower at high fps.
  If a QTE still fails, press **F9** to switch to the original 60 fps, then press F9 again to switch
  back.
- Timed hit-stop and short slow-motion effects still last half as long at 120 fps.
- Quick weapon switch: switching while L'Étranger (polearm) is still in Raiden's hand after a Y attack
  crashes the game. Switch to the sword (X) first. Other boss weapons are fine.
- Menu auto-repeat (holding a direction) runs faster at high frame rates.
- Controller rumble is shorter, because its timer counts frames.
- Other game systems that ignore the frame multiplier may also run at the wrong speed. See the
  checklist in [docs/RESEARCH.md](docs/RESEARCH.md).
- The engine measures frame time as float milliseconds since startup. In long sessions the precision
  drops, so the multiplier may jitter at high frame rates.

## Legal

This project contains no code or assets from the game. Addresses and behavioural descriptions are the
result of independent analysis for interoperability. You need your own legal copy of the game.
METAL GEAR RISING: REVENGEANCE is a trademark of Konami Digital Entertainment. This project is not
affiliated with or endorsed by Konami or PlatinumGames.

## Credits

The launcher icon (`src/launcher/launcher.ico`) is a Raiden drawing by Tunahan, released with this
project under the same license.

## License

MIT. See [LICENSE](LICENSE).
