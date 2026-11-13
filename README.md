# HF Blade

An open-source enhancement mod for *METAL GEAR RISING: REVENGEANCE* (PC, Steam). The goal is a
remaster-like experience:
- high frame rate (60–360 fps) with real-time gameplay, QTE and Blade Mode timing;
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
   enemies stay on the ground instead of exploding and vanishing. GEKKOs still explode as in the
   original game, because their many large pieces cost frame rate and block the way (`KeepGekko`).
5. **Photo mode (F10).** Freezes the game, hides the HUD and gives you a free camera. See
   [Photo mode](#photo-mode) below.
6. **Quick weapon switch (D-pad down).** Switches the unique (heavy attack) weapon to the next one you
   own, without opening the weapon menu (`[WeaponSwitch]`; the button and an optional keyboard key are
   configurable).
7. **Cheats (off by default).** Infinite health, infinite fuel cells, and Repair Units and Electrolyte
   packs that are not used up (`[Cheats]`, launcher page "Cheats"). Saving in the launcher switches
   them on or off in the running game within about a second.
8. **Timing fixes above 60 fps.** Systems that count frames are brought back to real time:
   - the right-stick camera turns at the same speed at any frame rate (`CameraTurnFix`);
   - button-mash QTEs (including Sundowner's shield), Blade Mode slow motion and the Blade Mode energy
     drain (`[Fixes]`); `QteEase` (default 60%) also makes mash gauges drain slower;
   - particle effects such as blood on the floor update at 60 Hz with the right time step, so they last
     as long as in the original game (`EffectTimingFix`);
   - menu auto-repeat runs at the original rate, so one press moves one step;
   - frame time is measured with a double-precision timer, so long sessions do not jitter
     (`PreciseFrameTime`).

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

- **Launcher art.** The Play page shows a built-in banner: a Raiden drawing by the project author,
  under the project's MIT license. To use your own picture instead, put an image named
  `hfblade-launcher-art.jpg` or `.png` next to the launcher. No game art ships with this project.
- **Launcher sounds.** The opening cuts play three sword sounds that the launcher synthesizes in code,
  at a random pitch, volume and stereo position. To use your own, put `.mp3` or `.wav` files in a
  `hfblade-sounds` folder next to the launcher. The speaker button in the top bar mutes them
  (`[Launcher] Sound`). No game sounds ship with this project.
- **Launcher.** `hfblade-launcher.exe` edits `hfblade.ini`. A sidebar switches between these pages:
  - **Play:** a summary and Save & Play;
  - **Gameplay:** FPS preset (60 to 360), menu FPS, the fixes, QTE ease, the weapon switch, debris and bodies;
  - **Display:** render size, FOV and supersampling. Supersampling (on/off, factor 1.78x or 2.25x)
    renders the game larger than the render size and lets NVIDIA DLDSR or AMD VSR shrink it to the
    screen, for a cleaner image. The driver option must be on. It can lower fps a lot (in one test, 2.25x
    on a laptop RTX 5070 Ti went from about 200 to 110 fps).
  - **Photo Mode:** the controls;
  - **Cheats:** infinite health, fuel cells and repair items;
  - **Launcher:** opening animation and sound;
  - **Log:** the last lines of `hfblade.log`.

  Every setting has a short description under it. The opening animation can be a flurry of cuts, a single
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
- Any other `TargetFPS` from 60 to 360 can be set in the ini. The launcher shows it as "Custom".
- Above 165 fps you need a matching display (or V-Sync off) and a fast CPU: the limiter mostly spins at
  240–360 fps. Particle effects still update at 60 Hz (see `EffectTimingFix`).

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
| Look | Right stick | Arrow keys (mouse look does not work yet) |
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

The launcher's Photo Mode page shows the same controls.

## Known issues / open questions

- **Antivirus:** some antivirus products quarantine the `dinput8.dll` proxy as a heuristic false
  positive, because it loads into the game and patches its memory. Kaspersky has been seen doing this.
  Build it from source if you prefer, and add an exclusion for the game folder.
- If a QTE still fails at a high frame rate, press **F9** to switch to the original 60 fps, then press
  F9 again to switch back.
- Timed hit-stop and short slow-motion effects still last half as long at 120 fps.
- Quick weapon switch: switching while L'Étranger (polearm) is still in Raiden's hand after a Y attack
  crashes the game. Switch to the sword (X) first. Other boss weapons are fine.
- Controller rumble is shorter, because its timer counts frames.
- Other game systems that ignore the frame multiplier may also run at the wrong speed. See the
  checklist in [docs/RESEARCH.md](docs/RESEARCH.md).
- Photo mode: mouse look does not work yet; use the arrow keys or the right stick.

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
