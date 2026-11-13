# MGR:R high-frame-rate research log

Target: *METAL GEAR RISING: REVENGEANCE* (Steam, PC), `METAL GEAR RISING REVENGEANCE.exe`

| Property | Value |
|---|---|
| SHA-256 | `7174741f46ab4c222304b0f1b9291a2281d53368016347e65d2ea404ff07ad34` |
| Format | PE32, x86, image base `0x400000` |
| Size | 26,347,008 bytes |
| PDB reference | `PRJ_020.pdb` (not shipped) |

All addresses below are virtual addresses for this exact build. Other builds will differ.
This document describes behaviour in our own words. It does not reproduce decompiled game code.

Tools: Ghidra 12.1.4 through REA (static analysis only). Nothing below has been confirmed at runtime yet.

Labels used in the tables:
- **Observed**: read directly from the binary.
- **Inferred**: a reasonable reading of what was observed, not yet tested at runtime.
- **Unknown**: still open.

---

## 1. Timer primitives (observed)

| Address | Role |
|---|---|
| `0x00df8110` | Timer init. Runs once. Calls `timeBeginPeriod(1)`, stores the QPF frequency and a start QPC, and precomputes a "ms per tick" float at `0x01dd4f14`. |
| `0x00df81d0` | Returns elapsed time since init as **float milliseconds**. |
| `0x00df8230` | Returns elapsed time since init as **int64 ticks**. |
| `0x00df8260` | Returns the "ms per tick" float (`0x01dd4f14`). |

Import addresses (IAT): `Sleep` `0x015fd1bc`, `QueryPerformanceCounter` `0x015fd1b0`,
`QueryPerformanceFrequency` `0x015fd1ac`, `timeGetTime` `0x015fd2ec`, `timeBeginPeriod` `0x015fd2f0`.

## 2. Frame limiter (observed)

| Address | Role |
|---|---|
| `0x00f97f90` | Present wrapper. Calls `IDirect3DDevice9::Present` (vtable +0x44). |
| `0x00f98000` | Current time in **thirds of a millisecond** (ticks × ms-per-tick × 3, rounded). |
| `0x00f98070` | **Limiter.** Calls Present, then waits until `now - last >= budget`. While waiting it calls `Sleep((budget - elapsed) / 3)`. Afterwards it stores `last = now`. |
| `0x01f206ec` | `budget` (uint32, unit: 1/3 ms). |
| `0x01f206f0` | `last` frame timestamp (same unit). |
| `0x00f98ad0` | **SetFrameRate(mode)**: if `mode == 1` the target is 1/30 s, otherwise 1/60 s. Sets `budget = round(t × 1000 × 3)`, which is **100** (30 fps) or **50** (60 fps). Also stores `mode` at `0x01f206d0`. |
| `0x00f9b2b0` | Renderer/device init. Sets the initial budget the same way. Also contains the resolution table and the 1280×720 UI scale. |

Callers of SetFrameRate: `0x00a52510` (main loop, called every frame), plus `0x00d4f6f0`, `0x00d502b0`,
`0x00d503e0`, `0x00d504f0` (**unknown**; probably cutscene/movie paths that switch to 30 fps).

For 120 fps the limiter budget must be **25** (8.33 ms). Note that the main loop calls SetFrameRate every
frame, so overwriting `0x01f206ec` once is not enough. Either SetFrameRate itself must be hooked or the
values it computes from must be changed.

The limiter's `Sleep` granularity is about 1 ms (`timeBeginPeriod(1)`). At 120 fps that leaves a
noticeable amount of jitter. A replacement limiter should spin-wait for the last ~1 ms.

## 3. Main loop (observed)

`0x00a52510` loops forever. One iteration does the following:

1. Calls `0x00e03970` on the object at `0x01be93b0` (see §4). The first argument is 1 when variable
   timing is allowed and 0 otherwise. Flags in `0x01bea060` / `0x01bea064` decide which. The second
   argument is `1.0f`.
2. Calls `SetFrameRate(mode)`. The mode comes from flags in `0x01bea060`, `0x01bea064` and `0x01bea084`.
3. Runs game update and render calls (not yet mapped).
4. Calls `0x00f9d650`, which is the limiter (`0x00f98070`) followed by the next stage.

The game simulation therefore advances **once per loop iteration**.

## 4. Frame multiplier: the key finding (observed + inferred)

The object at `0x01be93b0` has RTTI linking it to `cSlowRateUnit`. Its update function `0x00e03970`
computes a per-frame **multiplier** stored at `+0x7c`, which is absolute address **`0x01be942c`**:

- It measures the real frame time in ms, averaged over its update count (`+0x84` and `+0x90`).
- It sets `mult = measured_ms × scale`, where `scale` is the float at `+0x88` (`0x01be9438`). Scale is
  set at runtime. **Inferred:** it is ≈ 0.06, so that 16.67 ms gives 1.0.
- It clamps the result. If `mult < 1.0`, then `mult = 1.0`; the 1.0 comes from an inline `FLD1` at
  `0x00e03a07`. If `mult > 2.0`, then `mult = 2.0`; the 2.0 is read from the constant at `0x0163b740`
  (bytes `00 00 00 40`).
- When variable timing is disabled (argument 1 = 0), `mult` is set directly to argument 2, which is `1.0`.

About 185 code locations read `0x01be942c`. In the sample we looked at, the pattern is
`timer -= mult`, a frame-count timer measured in 60 fps units.

**Inferred consequence.** The engine already supports variable frame times between 30 and 60 fps
(multiplier 1.0–2.0). Above 60 fps the multiplier is held at 1.0, so the simulation advances one full
60 fps step per rendered frame. At 120 fps the game runs at **2× speed**.

**Candidate fix.** Lower the minimum clamp from 1.0 to about 0.5 (for 120 fps) and set the limiter
budget to 25.

**Unknown.**
- Do systems that do *not* read `mult` (fixed per-frame increments, animation frame stepping, physics
  substeps, input sampling, scripted timers) behave correctly at multiplier 0.5?
- Is `+0x88` constant, or does it change with the "slow rate" (Blade Mode / slow-motion)?
- Are there other places that assume `mult >= 1` (for example integer truncation of `mult`)?

## 5. Other strings (observed)

`DrawFrameRateType` (`0x016cf80c`), `MotionFrameRateType` (`0x016cf82c`) and `ProgressFrameRateType`
(`0x016cf840`) appear as event/parameter names. They are **not yet traced**, but they may control
per-scene draw and motion rates. `framerate_n` / `framerate_d` belong to the CRI movie header, which is
not relevant here.

## 6. Environment notes

The game loads `D3D9.dll` (dgVoodoo → D3D11) and `dxgi.dll` (ReShade) from its folder in the
author's setup. The mod therefore loads as a `dinput8.dll` proxy (the exe imports `DINPUT8.dll`).

## 7. PoC patches (implemented, `src/dllmain.cpp`)

| Site | Original bytes | Patch |
|---|---|---|
| `0x00e03a07` | `D9 E8 D8 56 7C` (`FLD1; FCOM [ESI+0x7C]`) | `JMP` to a stub that runs `FLD [minMult]; FCOM [ESI+0x7C]`, then returns to `0x00e03a0c`. The x87 stack depth and the flags are unchanged. |
| `0x00f98070` | `55 8B EC 83 E4 F8` (limiter prologue) | `JMP` to a replacement limiter. It calls the Present wrapper `0x00f97f90`, then waits for a QPC deadline at `TargetFPS`, or at 30 fps when mode `0x01f206d0 == 1`. |

SetFrameRate is left alone. Its budget variable is only read by the original limiter, which is now
bypassed.

Guards: the PE `TimeDateStamp` must be `0x52E76F3A` and the original bytes above must match.
`0x00f97f90` must also read `A1 <0x01F206D4 + delta>`. Otherwise no patches are applied.

**ASLR:** the exe has `DYNAMIC_BASE` set (DllCharacteristics `0x8140`) and loads at a random base,
for example `0x005F0000`. All addresses are rebased by `delta = base - 0x400000`, and signatures that
contain absolute operands are compared after relocation. The first in-game run failed precisely
because of this. `tests/check_signatures.py` starts the exe **suspended**, so no game code runs, then
checks the relocated signatures and terminates the process.

Verified offline: the DLL builds as x86 and exports `DirectInput8Create`. In a non-game process the
call is forwarded to the system DLL (`S_OK`) and the wrong exe is rejected. **Not yet run in-game.**

## 8. First in-game run (2026-11-05, Tunahan)

Observed in `mgr-hfr.log` (now `hfblade.log`):
- Gameplay runs at a steady `fps=120.0`.
- `mult` stays between 0.500 and 0.506.
- `scale` (`0x01be9438`) is exactly `0.06`, which confirms the inference in §4.
- The 30 fps mode (`mode=1`) appears with **both** multiplier paths: `mult=2.000` on the variable path
  and `mult=1.000` on the fixed path.

Player report:
- Gameplay feels much smoother.
- Menus stayed at 30 fps. This was expected, because the limiter honoured mode 1.
- **Button-mash QTEs cannot be completed.**
- **Quickly repeated presses of the same button are not recognised.**

Simple QTEs work.

## 9. Input path (observed)

| Address | Role |
|---|---|
| `0x00ddb430` | Per-frame input poll: XInput pads, then keyboard, then mouse. |
| `0x00ddb350` | Calls `XInputGetState` for 4 pads into `0x01dd05f0` (stride 0x38). A rumble timer at `+0x2c` counts down **one per frame**. |
| `0x00dda900` | Rumble output (`XInputSetState`). |
| `0x00dd9670` / `0x00dd9710` | DirectInput keyboard (256-byte state at `0x01dd06f8`) and mouse. |
| `0x00ddaac0` | Pad device update (vtable `0x00ddafe0`). Computes four masks: hold `[0]`, trigger `[1]` = rising edge, release `[2]`, and repeat `[3]`. The repeat counter `[0xb]` counts **frames** against a delay at `0x01dd05bc` and an interval at `0x01dd05b8`, both set at runtime. |

Edge detection is frame-rate independent, so the press-detection problem must sit in a higher layer.
**Hypothesis (unverified):** the game logic uses input windows or mash-gauge decay counted in frames
that ignore the multiplier, so they shrink to half their real-time length at 120 fps. Menu auto-repeat
also speeds up 2× (frame-counted repeat). The higher input layer is reached only through virtual calls
and has not been traced yet.

## 10. Patches added after the first run

| Site | Original bytes | Patch |
|---|---|---|
| `0x00e039a9` | `DD D8 DD D9 D9 44 24 10` (`FSTP ST0; FSTP ST1; FLD [ESP+0x10]`) | Same instructions plus `FMUL [fixedScale]`, then `JMP 0x00e039b1`. `fixedScale = 30 / MenuFPS`, so the fixed path keeps its per-second rate when the 30 fps mode is unlocked. |

- The limiter now uses `MenuFPS` in mode 1.
- A hotkey (`ToggleKey`, default F9) switches between HFR and the original 60/30 fps behaviour at
  runtime. This is the stop-gap for QTEs.
- On every timing-state change the mod logs the flag words `0x01bea060`, `0x01bea064` and
  `0x01bea084`. The goal is to tell which state the menus, QTEs and cutscenes use.

Second run (`evidence/run2`):
- Menus show `mode=1`, `timing=variable` and `f084=02600140` (bit 0x200000 set), running at
  `fps=120.0` with `mult=0.500`, so the time step is correct.
- The F9 toggle works.

## 11. 16:10 display (static; offsets verified against the exe)

These findings come from a delegated static pass. I re-checked every patch-site byte against the file.

- Resolution index: field 0 of `Documents\MGR\SaveData\GraphicOption` (read by `0x009c7a20` into
  `0x01b6efb0`). Indices 0..5 map to 800x600, 1024x768, 1280x720, 1366x768, 1680x1050 and 1920x1080.
- In fullscreen the backbuffer already matches the desktop size (`GetSystemMetrics` →
  `0x018ce9ac/b0`). The black bars come from two places:
  - the internal render target uses the option's size (1920x1080);
  - the final blit `0x00a3a790` recomputes a 16:9 letterbox itself.
- The engine camera `0x00de5aa0` (and its constructor `0x00da8390`) loads a hard-coded 16:9 aspect
  from `0x016c3078`. UI cameras and 2D ortho already use the render width/height. The HUD scale floats
  `0x018da490/494` are written but never read.

| Site | Original | Patch |
|---|---|---|
| `0x016ebf1c` (.rdata) | `00 00 87 44` (1080.0, mode table entry 0 height) | `Height1920` as a float |
| `0x00a30eb9` | `B8 38 04 00 00` (index 5 height) | `B8 <Height1920>` |
| `0x016c3078` (.rdata) | `39 8E E3 3F` (16/9) | `1920 / Height1920` |
| `0x00a3a8ce`, `0x00a3a868` (blit) and `0x00f9b9fc`, `0x00f9b9a9`, `0x00f9d5c6`, `0x00f9d56f` (init/reset) | `2B C7` / `2B C6` (`SUB EAX,reg`, the bar size before `SHR 1`) | `31 C0` (`XOR EAX,EAX`), which gives offset 0 and full size |

Expected result: the vertical FOV stays fixed, so the image is undistorted but slightly narrower
horizontally than 16:9 (Hor−). Unknowns:
- FMV / pre-rendered movie aspect (it may stretch);
- the mouse Y mapping;
- post-FX tables built from the render size;
- other projection callers `0x00985e30`, `0x00d1e1e0`, `0x00f43bb0`;
- windowed mode, which keeps a 1280x720 window, so use fullscreen.

**Width / ultrawide (2026-11-07).**
- The "1920x1080" slot width comes from two places:
  - the mode-table float at `0x016ebf20` (1920.0); entries are (w, h, 0) triples built at `0x00f9b5c7`;
  - `MOV EAX,0x780` at `0x00a30eaa` (index 5).
- Both are now patched to `[Display] Width`.
- The UI resolution index `0x01b6efb0` is only rewritten for exact widths 800/1024/1280/1366/1680/1920
  (`0x009c5330`); other widths keep index 5, so the 3D UI eye distance still assumes H=1080.
- FOV rule per call site (the return address identifies it):
  - world cameras: `tan` × (16/9)/aspect only when aspect < 16/9; wider screens are already Hor+;
  - the two 3D UI calls (`0x00cacb8d`, `0x00cacc2d`): `tan` × H/1080.
- Not done:
  - the 2D HUD is still mapped per axis (`x·W/1280`), so it stretches on ultrawide;
  - FMV aspect is unknown.

**Vert+ FOV (added after the fourth run).** The perspective builder `0x00ddccc0` is cdecl
`(out, fov, aspect, near, far, 0, 0)`. It computes `cot(fov × 0.5)` (constant `0x01646bc0` = 0.5) as
the Y scale and divides by the aspect for X, so `fov` is the full **vertical** FOV in radians. With
aspect 1.6 the horizontal view shrinks (Hor−), which looks like a zoomed-in 16:9 image. The mod
redirects all 9 calls (`0x00985f6c`, `0x00a28128`, `0x00a460f6`, `0x00cacb8d`, `0x00cacc2d`,
`0x00d1e43e`, `0x00d1e47c`, `0x00de5b18`, `0x00f43cfb`) to a stub. When the aspect equals the patched
one, the stub replaces the FOV with `2·atan(tan(fov/2) · (16/9)/aspect)`, then tail-jumps to the
builder. Other aspects pass through unchanged.

**HUD at 16:10 (static, delegated; fuller notes in `evidence/hud_notes-2026-11-06.md`).**
Fourth run report: many HUD elements are shifted.
- UI code maps the virtual 1280x720 space to pixels one axis at a time (`x·W/1280`, `y·H/720`), so the
  2D layer is stretched 11% vertically at 1200 but stays anchored.
- The misplacement comes from the 3D UI camera. It is built by `0x00cacbf0` (perspective call
  `0x00cacc2d`, aspect = W/H). Its eye distance comes from a table at `0x00cad430`, indexed by
  **width only**. For 1920 the table holds −10.5 = `1080 · 0.005 / tan(fov/2)`, which assumes H=1080,
  so at H=1200 the layer is magnified 1.11× around the screen centre and edge elements drift outward.
- The Vert+ hook already covers this call. Widening `tan(fov/2)` by `(16/9)/1.6 = 1200/1080` gives the
  same visible height as moving the eye to −11.667, so no separate patch is needed (inferred; not yet
  tested in game).
- Still open:
  - elements sized uniformly from the height (`0x00cc42d0`, `0x00cc4300`, and `0x00efd280` when flag
    0x100000 is clear) are 11% too wide;
  - a true centred 16:9 HUD band would mean squeezing clip-space Y in the UI projections `0x00cacc90`,
    `0x00cacbd0` and `0x00cacc70`, plus remapping the scissor and hit tests.

Third run (2026-11-06, player report): 16:10 works. At 120 fps, mash QTEs need about 2× faster
mashing, but they work after pressing F9 to switch to 60 fps. Blade Mode slows time less at 120 fps.
The timing state does not change during QTEs or Blade Mode.

## 12. Button-mash QTEs (static, delegated; I verified the bytes)

There is no shared mash routine. Each enemy has its own state code, but they use the same idioms:
each press adds to a value, and every update removes a fixed amount from it, ignoring the
multiplier.

- **Speed struggles** (Em0600 / pc60 code, probably EXCELSUS and others):
  - each press: `[+0x920] += 0.15`, clamped to at most 2.0;
  - each update: `FSUB [0x0163CCE0]` (0.01), clamped to at least 0.5;
  - sites `0x005ac120`, `0x005ac5b3`, `0x005aeea5`, `0x00840ac9`, `0x008410fa`.
  - **Patch:** point the FSUB operand at a mod float `0.01 × mult`. The shared 0.01 constant itself is
    left unchanged.
- **EM0130/0131 finish QTE** (`EM013x_FINISHQTE_RENDA`):
  - the gauge decrements by one per frame at `0x006127b7` (`DEC [ESI+0x948]`);
  - a 300-frame timeout increments at `0x00612782` (`INC [ESI+0x94C]`);
  - a variant also increments at `0x00611ba4`.
  - **Patch:** `CALL` stubs that add or subtract whole 60 Hz ticks, accumulated from the multiplier
    (0 or 1 per frame at 120 fps).
- Grab-escape QTEs count presses and have no decay, so they are unaffected.
- **Ease (fourth run).** With the fix active the player still found mash QTEs harder at 120 fps.
  `[Fixes] QteEase` (percent, default 80) scales both decays (the float gauges and the `0x948`
  counter) while HFR is on. Timeouts stay in real time. One possible cause is that some presses are
  lost when a consumer runs only on 60 Hz ticks; this is unverified.
- Fifth run: still hard at 80%, and the player did not say which QTE. Observed for EM013x RENDA at
  `0x00612788`: the press test reads the global pad-0 trigger `0x01b7b914 & 0x40` directly, every
  update. A press adds `15 − int((g+15)·c)` and skips that frame's decrement. Presses are therefore
  not frame-gated in that QTE.
- The input path is polled every frame. Task `0x00a4d130` runs `0x00a1e8a0`, which updates the pads
  and copies them to `0x01b7b910 + n·0x30`.
- **Sundowner shield QTE (root cause found).** The player identified the QTE: Raiden jumps on
  Sundowner and mashes X to pry the shields apart. Em0310 (Sundowner; class found through RTTI, vtable
  `0x0167a1bc`) runs a tug-of-war at `0x00584c22`:
  `g = clamp01(g + (pressed ? dt·[esi+0x938] : −dt·[esi+0x93c]))`, where `dt = [esi+0x910]` is the
  per-frame time step. A press is a single-frame trigger, so at 120 fps each press adds half as much
  while the decay stays real-time. That is exactly the "mash 2× faster" symptom.
  - **Patch:** `0x00584c3d` `D8 8E 38 09 00 00` → `fmul [esi+0x938]; fdiv [mult]` (press adds the
    60 fps amount).
  - **Patch:** `0x00584c4b` `D8 8E 3C 09 00 00` → `fmul [esi+0x93c]; fmul [ease]`.
  - A scan for the same idiom (trigger test, then `fmul` by dt and `fadd`) across `.text` found no
    other gameplay site. The other hits are debug tweak code.
- The default ease is now 60%. A diagnostic, `LogInput=1`, logs the per-second game trigger count
  next to rising edges computed from the hold mask, to check whether presses are lost.
- **Not covered:** mash code that reads input through other paths (Sundowner and Armstrong are
  unconfirmed). There is also no root cause yet for "rapid repeated presses not recognised".

## 13. Blade Mode / slow motion (static, delegated; I verified the bytes)

- `0x01be93b0` is the slow-rate manager. Its per-frame cascade `0x00e087f0` computes group rates as
  `mult × slow × base`. The getter is `getRate` `0x00e049b0` (thiscall, no stack args, result in ST0).
  Blade Mode is driven from `0x00bc27a0` and started by `0x00b85350`, which sets the world target to
  0.01 or 0.025.
- **Cause 1** (observed code, inferred effect): 17 sites set an animation speed to `X / getRate()`.
  The intent is to cancel the world slow-down so a synced animation plays at full speed. Because the
  rate already contains `mult`, the division cancels the frame-rate scaling too, and these animations
  run 2× fast at 120 fps.
  - **Patch:** redirect each `CALL` to a stub that returns `getRate() / mult`. This is identical at
    60 fps.
  - Sites: `0x005cc9b8`, `0x005cd009`, …, `0x005cebfd` (15 sites), `0x008ce295` and `0x00ae44c7`.
- **Cause 2** (observed): the slow-mo ramp `inter += (target − inter) × k` runs once per frame, at
  `0x00bc285a` (world) and `0x00bc287c` (player).
  - **Patch:** `k × mult`, a first-order approximation of `1 − (1 − k)^mult`. At 120 fps the ramp is
    about 8% slower than exact.
- **Cause 3** (observed; reported in game on 2026-11-07 as "the energy bar empties too fast in Blade
  Mode"): the energy drain.
  - Every Blade Mode state calls `0x00bf24f0` once per frame. It checks the `GAME_` flags at
    `0x01bea090`:
    - bit `0x80000000` is the single-bar mode;
    - bit `0x2` is `GAME_MUGEN_ZANGEKI` (id 30, mask `0x80000000 >> id`; flag getter `0x00416d50`).
  - It then subtracts `vfunc[0x3B0]() × [player+0x338C]` from the fuel cells with
    `SubEnergy` `0x00bc3000`. The cells are 24-byte entries at `[player+0x3820]`: value `+4`, max
    `+0xC`.
  - There is no time step, so the bar drains twice as fast at 120 fps. By contrast, the Ripper Mode
    drain at `0x00c10513` already multiplies by the player's step `[+0x910]`.
  - The DLC characters use the same code at `0x00877147` and `0x008b8457`.
  - **Patch:** at all three sites, the `FMUL dword [ESI+0x338C]` (`D8 8E 8C 33 00 00`) becomes a
    `CALL` to a stub that also multiplies by `min(mult, 1)`.
- Also seen, not patched: the Blade Mode cut-angle smoothing `0x00be2b54`.
  - It eases the plane angle `[player+0x3F8]` toward the stick angle by k = (stick magnitude)² per
    frame (`0x00bb5fa0`).
  - At full tilt it snaps, so only partial tilts react faster at 120 fps.
- **Not fixed:** timed per-unit slows and hit-stop count whole frames, so they last half as long at
  120 fps. These are the counters at `0x00e0896c`, `0x00e061b1` and `0x00c238e0`.

## 14. Persistent debris (static, partly delegated; hook bytes verified by me)

Goal: keep the pieces of the last N cuts instead of letting them explode and vanish.
Pieces disappear through three paths:

1. **Object debris lifetime.** `BehaviorDebrisBase` update `0x005dda00` (vtable `0x0164393c`, slot
   0x13). `BehaviorDebrisObject` `0x005dde10` calls it.
   - Age is a float at `+0x884`, in seconds.
   - Lifetime L: 3 s for enemy classes, 6 s, or 10/15/30 s depending on the graphics option.
   - The piece fades for 2 s before L and is deleted at L.
   - `+0x928 ≠ 0` skips the whole block.
2. **Enemy-part debris.** `BehaviorDebrisActor` `0x005de4e0`, `BehaviorDebrisEmActor` `0x005ddfb0`
   and `Em0010DebrisActor` `0x005e6620` have their own update. Age `+0x884` grows; the fade starts
   at `[+0x8f0]` and the piece is deleted a little later.
3. **Group explosion.** `DebrisExplodeManager` (`0x01b36a60`) has 12 slots of 0xB0 bytes.
   - The group id at slot `+0x14` is the piece's `+0x83c`; 0 means no group.
   - The countdown is checked in `0x00942d90`. When it ends, `0x0093eee0` runs the explosion and
     `0x0093e740` deletes every piece in the group.
   - Parameters come from `debrisExplodeParameter.bxm`.
4. **Reducer.** `DebrisManager` force-fades one piece (`0x005d9560`) when more than 90 exist. Pieces
   with priority `+0x524 < 0` are skipped.

**Patches** (`[Debris] KeepLastCuts`):
- A "cut event" is a run of new pieces (age < 0.05 s) that appear within 0.3 s of each other. The mod
  keeps a ring of the last N events (piece pointers and group ids). Pointers are compared but never
  dereferenced.

| Site | Original | Patch |
|---|---|---|
| `0x005dda84` | `39 9E 28 09 00 00 0F 85 72 03 00 00` | Protected → priority −1 and age `+0x884` := 0, then the original test and the full update. This covers the fade/delete and the explosive-piece timer (`age ≥ [+0x900]`). |
| `0x005ddfbe`, `0x005de4ee`, `0x005e662e` | `CALL 0x00a8b7f0` | Call it, then hold age `+0x884` at 0 for protected pieces. |
| `0x00942dca` | `8D 44 24 10 50` | Skip the slot while its group belongs to a protected event. Once the event is released, the group explodes on the next frame. |
| `0x005d9560` | `56 8B F1 83 BE 80 08 00 00 00` | Return immediately for protected pieces. |

**Not covered:**
- FastDie pieces (deleted on their first update);
- explosive pieces from the explosion table (`+0x890`);
- the chain explosion `0x0093f4e5`;
- other debris classes (Armor `0x005dab60`, Slider `0x005de340`, ZangekiBonus `0x005de1e0`).

`LogDebris=1` logs events, group ids and releases.

Seventh run: enemies still exploded shortly after landing. The log showed skipped cut ids, releases
with "0 pieces" and lost log lines. The cause is that the debris updates run on several game worker
threads at the same time and raced on the unprotected ring.
- **Fix:** an SRW lock around all ring access, plus a lock in `Log()`.
- Group ids are now re-read on every frame, because a piece may join its group after its first
  update.
- `LogDebris` logs each new thread that updates debris.

Eighth run: bodies stay, but some pieces hung in the air. The first version jumped to the epilogue, which skipped the rest of the update, physics and transform included, so pieces froze where they spawned. Fix: hold the age at 0 and let the update run.

Performance note (sixth run, from the log): at `TargetFPS=165` the game held 105–161 fps, so 165 is
not stable natively on this machine. The multiplier tracked the real frame time correctly.

## 15. Persistent enemy bodies (static, delegated; call sites verified by me)

Fuller notes are in `evidence/corpse_notes-2026-11-06.md`.

- All Em*/Et* classes derive from `BehaviorEmBase` (vtable `0x0163fa54`).
- There is no shared corpse timer. Each type has death states and its own **vanish** function, which
  plays the explosion or vanish effect and sound, cleans up debris and hides the body. The object is
  deleted (`0x009fdde0`) 180 frames later.
- **Em0010** (cyborg soldier):
  - death states `0xb0000` (`0x00b3f320`), `0xb0001`, `0xb0002` (fallback, `0x00b3f940`) and
    `0xc0003` (`0x00b359e0`);
  - vanish `0x00b346d0` (thiscall(int), `RET 4`).
- **Em0030:** vanish `0x00b67710` (thiscall, `RET`), called when the death animation ends.
- `EmSet` (`0x01c78cb0`) does not count bodies with `+0x6BC ≠ 0`, so waves and enemy-count triggers
  keep working.
- `EnemyManager` caps live enemies at 128.

**Patch** (`[Debris] KeepCorpses`, default 3, up to 8):
- The 7 vanish call sites (`0x00b3f668`, `0x00b3f8c7`, `0x00b3f9b8`, `0x00b35aba`, `0x00b6d1ef`,
  `0x00b6d3ee`, `0x00b6dba6`) call stubs instead.
- For the last N bodies a stub:
  - skips vanish;
  - rewinds the death step `+0x61C` so the same code calls it again next frame (step := 2 or 0, or
    undo the following `INC` / `ADD EBX`);
  - sets `+0x6BC = 1`.
- When a body is pushed out of the ring, its next call goes through, so it explodes and is deleted
  as usual.
- Skipped bodies: special deaths (`Em0010 +0xDD4 & 0x100`) and instant vanishes (`Em0030 +0xA50 == 0`).
- The ring is locked, and pointers are only compared.

**Not covered yet:**
- other enemy types (Em0060/0070/0080/0100/0120/0190/0221, the DLC clones), which have their own
  vanish functions;
- kill-volume removal (`0x00b35560`).

### 15b. Kept bodies and pieces vs. objectives, and zandatsu

First photo-mode test (2026-11-07): some "kill all enemies" objectives never completed, and after a
zandatsu the grabbed enemy's pieces hung in the air.

**Objectives**
- Layout of the EmSet update `0x00ca7b20`:
  - set = `0x01c78cb0 + 0x20 + s·0x7170`;
  - group G = set `+ 0x40 + g·0x660`;
  - slot = G `+ 0x30 + k·0x40`.
- `slot+4` holds the enemy's debris group id. Every frame, each slot is checked with `0x0093db40`
  ("does a debris group with this id exist").
- G+0x654 ("finished, debris included") is set only when no slot still has a group and G+1 (all
  waves done) is set. The `cCondEnemy*FinishDebris*` conditions read G+0x654.
- Kept bodies (vanish skipped) and kept pieces never release their group, so these objectives were
  stuck.
- **Patch:** at `0x00ca7bfb` (`39 5C 24 14 75 0F`) the `JNE` is NOPed, so G+0x654 is set as soon as
  G+1 is.
  - It is applied only when KeepLastCuts or KeepCorpses is on.
  - Vanilla waits a few seconds for the pieces to explode first.

**Zandatsu**
- On entering a cut-and-grab state, the player calls `0x00941450` (DebrisExplodeManager, thiscall,
  `[esp+4]` = the target's group id).
- In vanilla, the pieces are deleted when the grab ends: the enemy's vanish calls `0x00940450`.
- **Patch:** an entry detour at `0x00941450` (`51 53 55 56 57 8D 69 50`) records those group ids.
  Pieces and bodies of a recorded group are no longer kept, so they vanish as in vanilla.

**GEKKO** (2026-11-13, player request)
- Em0030 is the GEKKO (the exe has the string `em0030_gekko`). It is large: cut, it breaks into many
  pieces, which cost frame rate and block the way when kept.
- Its death handler `0x00b6cfb0` calls vanish `0x00b67710` when the death animation ends; vanish plays
  the explosion and deletes the enemy's debris groups through `0x00940450`.
- `[Debris] KeepGekko=0` (default): CorpseKeep lets Em0030 vanish, and an entry detour at `0x00b67710`
  (`D9 EE 56 83 EC 08`) records the GEKKO's group `[+0x83C]` like a zandatsu group first, so its kept
  pieces are released and explode with it.
- Not covered: the DLC/VR clones Em8030 and Emc030 have their own vanish functions.

## 16. Photo mode (static, delegated; hook bytes verified by me)

Fuller notes are in `evidence/photo_*.md`.

**Freeze**
- Bit 31 of `0x01bea070` is the game's own "stop world" flag. Script actions set it at `0x00c87c6a`
  and clear it at `0x00c87d0a`.
- The scene update `0x00a54490` tests it at `0x00a54574` and `0x00a5474c`. When set it skips AI and
  battle managers, the object phases, the slow-rate cascade and the Havok step. Rendering and the
  camera tick keep running.
- In the camera update, only the virtual state update is skipped (`0x00dd1c5e`). `0x00dd03e0` still
  runs and calls `0x00db8090`.
- The mod sets the bit on entry and re-asserts it every frame, because scene init rewrites the dword.
  On exit it restores the saved bit.
- Zeroing the multiplier alone would produce NaN (`mult / getRate()` sites), so it is not used.

**Input**
- The pad copy `0x00a1e8a0` calls a disabled hook, `0x00df7c00` (`xor eax,eax; ret`), at
  `0x00a1e8a7`. When that call returns non-zero, the game zeroes all pads.
- The call is redirected to a stub that returns 1 while photo mode is on. After exit the stub keeps
  returning 1 until the buttons are released.
- The mod reads the controller itself through `XINPUT1_3!XInputGetState`, and the keyboard through
  `GetAsyncKeyState`.

**Camera**
- `cCameraApp::UpdateMatrices` `0x00db8090` is the common path for gameplay, cutscene and Blade Mode
  cameras.
- At `0x00db8097` it calls the view builder `0x00db7e00`. The mod replaces that call with a stub that
  writes into the main camera `0x01bea1d0`:
  - eye `+0x1b0`, target `+0x1c0`, up `+0x1d0` (with our roll);
  - FOV `+0x94`;
  - game roll `+0x4f0` = 0.

  The stub then tail-jumps to the original, so view, projection, view-proj and the frustum all match.
- Conventions: Y-up, right-handed (D3DX LookAtRH / PerspectiveFovRH), metres. Forward =
  (−sin y·cos p, sin p, −cos y·cos p).
- The FOV is restored on the first frame after exit. The game rewrites eye, target and up from its
  own copies each frame.

**HUD**
- The render-queue executor `0x00f9ae10` runs on the render job. At `0x00f9ae76`
  (`8B 48 0C 8B 40 08`) a stub skips the phase-1 draw items of UI layers 0x3e, 0x60, 0x61, 0x66, 0x67
  and 0x69. Begin and end callbacks still run.
- Layer 0x66 holds the map/objective marker. It was found in game on 2026-11-13 with the layer hunt
  (M / N in photo mode), which hides one more layer that drew while the HUD was hidden and logs each step.
  `[PhotoMode] HideLayers` adds more layers without a rebuild.
- Mouse look is not working yet. The first method (cursor delta, re-centred every frame) made the camera
  jump: the game also moves and clips the cursor. The second (raw input, `RIDEV_INPUTSINK` on a
  message-only window in its own thread) receives no motion in game (test 2026-11-13). A likely cause is
  that DirectInput 8 registers raw mouse input for the same process, and a process has only one target
  window per device type. Next step: read the game's own DirectInput mouse state instead (the mod is the
  dinput8 proxy), or look at how other free cameras do it.
- World layers, post effects and fades (0x65, 0x68) are untouched.

**Audio** (added after the first photo-mode test)
- Sound is Wwise; FMV audio goes through CRI Mana and is separate.
- The pause menu posts the named events `core_se_sys_se_pause` / `_resume` through `0x00e5e050`
  (`(name, 0)`). These pause the SE, voice and ambience buses.
- The cutscene pause menu posts `bgm_Paused_Movie_Enter` / `_Exit` through `0x00e5e1b0` (`(name)`),
  which pauses the music.
- The mod posts the same pairs on photo-mode entry and exit (`[PhotoMode] PauseAudio`).
- Sounds that start during photo mode are still heard.

**Untested in game, with known risks**
- Particles, cloth or animation might keep moving.
- Fast camera moves may cause motion blur, because the previous view is not reset.
- Streaming and LOD follow the camera, so areas far from the player may be missing.
- The sound listener may move with the camera.

## 17. Effect timing above 60 fps (runtime with Frida, then static)

Reported in game on 2026-11-07: blood on the floor stayed 1-2 s at 120 fps and visibly longer at
60 fps (F9).

**Structure**
- `EspListThread` (vtable `0x016dfcc8`) is kicked once per frame. The kick is `0x00f43a20`.
- Worker jobs (`0x00f41310`, `0x00f41490`) call each live effect's `vfunc[2]` (update) once. The three
  dispatch sites are `MOV r,[vtable+8]; MOV ECX,ESI; CALL r` at `0x00f41412`, `0x00f41470` and
  `0x00f41513`.
- `0x00edfc20` fills each effect's update context:
  - `+0x1C` rate: `getRate` of the effect's slow unit, which includes the frame multiplier;
  - `+0x20` local speed;
  - `+0x24` frame multiplier: `[0x01be93b0+0x7C]`, or 1.0 for flag `0x10` effects.

**Measurements** (Frida)
- The context rate equals the frame multiplier for every effect seen, about 0.5 at 120 fps.
- Effect times such as `+0x118` advance correctly.
- Lifetime per effect type at 120 vs 60 fps: most types are equal. A few live 1.4-3x longer at
  60 fps. Their particles count update calls instead of using the context step.
- The effect data blocks carry no names, so the blood type could not be singled out.

**Fix (verified in game with a Frida prototype, then moved into the DLL)**
- Above 60 fps, effects update at most once per 60 Hz tick, with the time step of all frames since
  the last update.
- At the kick: `acc += mult`.
  - When `acc >= 1` (or `mult >= 0.95`), effects update with `factor = acc / mult`, and the context's
    `+0x1C` and `+0x24` are multiplied by `factor` (hook at `0x00edfca5`).
  - Otherwise the three dispatch sites call a `RET` stub instead of the update.
- At 60 fps and below it changes nothing. Effects then animate at 60 Hz while everything else runs at
  120 fps. This was not visible in the test.
- `[Fixes] EffectTimingFix`.

## 18. Title screen eye glow (runtime A/B, then static)

Reported 2026-11-07: Raiden's eye glow on the title screen was missing.
- It is not an Esp effect: no effect goes through `0x00edfc20` on the title screen, and undoing the
  effect timing patch live changed nothing.
- It came back with F9 (original timing; the title runs in the game's 30 fps mode).

**Structure**
- The title screen is `cTitleStart` (vtable `0x01656c8c`). Its init `0x009b6670` plays the alpha movie
  `movie_ui/Title_Alpha.usm`.
- The update is slot 5 (`0x009a7220`). It switches on state `[+0x38]`:
  - 1-2: load and init;
  - 3: "press start";
  - 4: fade-out after start;
  - 5: main menu;
  - 6: fade back in.
- The fades step a constant per frame (`0x0163d8f8`).

**Fix**
- The vtable slot is redirected to a stub that marks the frame when the state is not 5.
- While a mark is recent, the limiter paces at `TitleFPS` (default 30), and the fixed-path scale is
  set to `30 / TitleFPS`.
- The main menu stays at MenuFPS.
- The exact reason the movie fails at high fps (CRI Mana timing) was not investigated.

**Correction (2026-11-12):** Tunahan pointed out that the published build ran the title screen at
165 fps with the eye glow intact, so the cause was the effect timing patch (§17), not the frame rate.
The A/B tests above were misleading:
- the glow is probably created once when the title opens, so undoing the patch live could not bring
  it back;
- F9 also changes the effect patch's behaviour.

The title runs in the game's 30 fps mode. Effect decimation is now skipped entirely in that mode, and
`TitleFPS` defaults to 0 (off). The 30 fps title cap stays available as an option.

**Open: blood that stays on the floor**
- The effect timing fix (§17) restores the original duration, but the request is for blood that never
  fades.
- The blood effect type has not been identified yet. A wall-clock capture failed because the game
  pauses when unfocused.
- Next: a game-time capture tied to cut events (debris actor creation `0x00a8b7f0`). Then the plan is
  to stop updating settled blood effects, keeping the last N; an effect that is not updated stays
  drawn as is, which the §17 test showed.

## 19. Quick boss-weapon switch (static + in-game slot watch)

The in-game weapon menu (`cWeaponSelectMenu`, vtable `0x01656654`) applies each slot in `0x00992ab0`.
Two slots were tried first and turned out wrong in game:
- `0x01be9fe8`: sub-weapons;
- `0x01be9ffc`: another customize slot.

A temporary log of every change in `0x01be9fb8..0x01bea060`, recorded while Tunahan changed the boss
weapon in the menu, identified the right slot.

**Boss weapon slot**
- `0x01be9fd4`: equipped kind;
- `0x01be9fd8`: requested (bit 31 in game states 8 and 9);
- `0x01be9fdc`: loading flag.

**Request** (menu `0x00992ca7`), when `kind != [fd4]`:
1. `player->0x00b7d0e0()`;
2. `[fd8] = kind`;
3. `player->0x00b7f5e0(kind)`;
4. `[0x01dc074c] = 1`.

**Finish** (menu `0x00992d6b`)
- When `[fdc] == 0 && player->0x00b7d0c0() == 0`, player vtable `+0x3D8` attaches the weapon.

**Ownership** (menu `0x009933df`)
- Table `0x016566a4`: 3 entries `{item, value}`, items `0x18`, `0x17` and `0x16`.
- Owned if `0x009c4900(item)` returns nonzero, which means the status byte
  `[0x01b73860 + item*32] == 3`.
- Value → kind: `0→2, 1→3, 2→4`.

**Crash mid-attack** (seen in game): swapping while a boss-weapon attack animation still uses the
weapon crashes the game. The menu never hits this because it pauses the game. The switch is now queued
until no attack button (pad X/Y/B, mouse buttons) has been held for `QuietMs` (default 1000 ms);
after 8 s without such a moment it is dropped. The input wait alone still crashed: Raiden keeps using the weapon after the button is released.
A Frida probe sampled the player at 30 Hz with the pad's Y button. `[player+0x11A4]` was 0 in every
idle sample and 1 in 99% of the samples within 1 s of Y, and attack code sets it (`0xb99xxx`). The
switch first waited for that flag to be clear for 15 frames, but it still crashed right after an
attack (idle swaps worked). The weapon stays out after the attack flag clears. The probe's second
field, `[player+0x2658]`, is 1 when idle and 0 from the attack until the weapon is put away. The
switch now waits until `0x11A4 == 0` and `0x2658 != 0` have both held for `SettleMs`.
- That still crashed. Windows' crash record gives the same address every time: `0x00e23924`,
  in `0x00e23900`, which searches a motion's 12-byte frame/event table (terminator `0x7fff`, 17
  callers in the motion system).
- Inferred: each boss weapon loads its own set of Raiden's motions. Shortly after an attack Raiden is
  still in one of them, so unloading the old weapon leaves the current motion pointing at freed data.
  Idle swaps after 5-10 s work.
- **Root cause, found with a Frida exception handler:**
  - The crash's call chain goes through the player update (`0x00b94804`) into the motion code.
  - The log showed the first switch succeed (2 → 4). Then the slot was back at 2 and was re-requested.
  - The game refills the boss slot from the persistent record `[0x01b7588c]` (`0x009bbed3`), and the
    menu's commit writes that record (`0x009b31a1`: `MOV [0x1B7588C],2/3/4`). The switch did not write
    it, so the game still treated the old weapon as equipped and drove its motions with the new
    weapon's data.
- **Fix:** write `[0x01b7588c] = kind` with the request. `SettleMs` is back to 750.
- The real fix is to check that the player's current motion is not from the weapon's set. That needs
  the crash's call stack (Frida exception handler). `QuietMs` default lowered
to 300.

**Settings:** `[WeaponSwitch]` `Enabled`, `PadButton` (XInput mask, default 2 = D-pad down), `Key`., `QuietMs`.

## 20. 240–360 fps

- **Range and presets.** `kMaxFps` is now 360. The presets are 60, 90, 120, 144, 165, 240 and 360.
  The minimum multiplier clamp becomes `60 / 360 = 0.167`.
- **Frame-time precision (observed).** The multiplier update `0x00e03970` computes
  `now - last` with `FLD ST0; FSUB [ESI+0x80]` at `0x00e03999`. Both values are float
  milliseconds since startup (`0x00df8c10` → `0x00df81d0`). A float's resolution is 0.25 ms after
  1 h and 1 ms after 4 h of play, so at 2.8 ms frames the multiplier would jitter by tens of percent.
  - **Patch** (`PreciseFrameTime`, default on): those 8 bytes jump to a stub that returns a
    double-precision QPC delta.
  - The stub falls back to the game's own delta on the first call and for values ≤ 0 or > 1 s.
  - It preserves flags and registers, because the `JNE` at `0x00e039a7` tests flags set before the
    patch.
- **Limiter.** It sleeps only while more than 2 ms remain, then spins. `Sleep(1)` overshoots by up
  to about 1 ms, which is a third of a 360 fps frame.
- **Effects.** The effect step factor cap was raised from 4 to 8: at 360 fps the effects update every
  6th frame.
- **Weapon switch.** The give-up timeout is now in time (5 s) instead of frames.
- **Untested:** 240 and 360 fps in game. The user's display is 165 Hz, so a test above that needs
  V-Sync off. Integer frame counters that §12 and §13 did not cover will be even further off at
  360 fps.

## 21. Camera turn speed (static)

Reported 2026-11-12: the right-stick camera turned faster the higher the frame rate.
- The camera integrates its angles every frame:
  `[+0x790] += [+0x780] + [+0x778] + [+0x788]` (yaw), and the same pattern for pitch (`+0x794`), at
  `0x00da248a` and `0x00daa35b`.
- The stick term `[+0x780]` / `[+0x784]` is set by `0x00db89b0` / `0x00db8a40` from
  `0x00db9880`, which 8 camera states call. That function turns the pad value (bytes / 255) into
  `x*|x|*K*arg` (K = 0.4). Only the clamp, `mult * 8 deg`, is scaled by the multiplier.
- **Patch** (`CameraTurnFix`): both `FMUL [ESP+0x4C]; FSTP [ESP]` sites (`0x00db99cb` pitch,
  `0x00db9a3d` yaw) also multiply by `min(mult, 1)`. 60 fps and below are unchanged.
- **Not changed:** the follow/recentre terms `+0x788` / `+0x78C` are per-frame lerps (diff × 0.1), so
  auto-recentring is still a little faster at high fps.
- **Unverified:** whether mouse look also goes through the stick path. If it does not, mouse speed is
  unaffected.

## Next steps

1. In-game test checklist. Every 600 frames, check `hfblade.log` for `mult≈0.5` at 120 fps.
   - [ ] Walking/running speed versus 60 fps (stopwatch over a fixed distance)
   - [ ] Combat timing, parry window, enemy attack speed
   - [ ] Blade Mode (slow-motion interaction with `cSlowRateUnit`)
   - [ ] QTEs, scripted timers, boss phases
   - [ ] Real-time and pre-rendered cutscenes (30 fps mode)
   - [ ] Physics debris, cloth, particles
   - [ ] UI/menu animations, audio/lip sync
2. If any of these are wrong, find code that steps per frame without reading `0x01be942c`.
3. Trace the consumers of the three `*FrameRateType` parameters.
4. Fix float time precision: rebase `0x00df81d0`, or hook `0x00e03970` and measure with a double QPC.
