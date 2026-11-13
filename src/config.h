// HF Blade: settings shared by the mod DLL and the launcher (hfblade.ini). Keep both in sync through here.
#pragma once

namespace cfg {

static const char kIniName[] = "hfblade.ini";
static const char kLegacyIniName[] = "mgr-hfr.ini";   // pre-rename name, migrated on first run
static const char kLogName[] = "hfblade.log";

// Frame-rate presets offered by the launcher and cycled in game with PresetKey.
static const int kPresets[] = { 60, 90, 120, 144, 165, 240, 360 };
static const int kPresetCount = int(sizeof kPresets / sizeof kPresets[0]);

static const int kMinFps = 60, kMaxFps = 360;   // TargetFPS range
static const int kMinMenuFps = 30;              // MenuFPS range: 30..kMaxFps, 0 = same as TargetFPS

// Defaults ([section] key).
static const int kTargetFps    = 120;   // [General] TargetFPS
static const int kMenuFps      = 0;     // [General] MenuFPS (0 = follow TargetFPS)
static const int kToggleKey    = 0x78;  // [General] ToggleKey, F9: HFR <-> original 60/30 fps
static const int kPresetKey    = 0x77;  // [General] PresetKey, F8: next FPS preset
static const int kLogStats     = 1;     // [General] LogStats
static const int kLogInput     = 0;     // [General] LogInput
static const int kHighFrameRate = 1;    // [General] HighFrameRate
static const int kQteFix       = 1;     // [Fixes] QteFix
static const int kQteEase      = 60;    // [Fixes] QteEase, percent 10..100
static const int kBladeModeFix = 1;     // [Fixes] BladeModeFix
static const int kWidth        = 1920;  // [Display] Width  (render size for the game's "1920x1080" option)
static const int kHeight       = 1200;  // [Display] Height (0 = off; older configs: Height1920)
static const int kVertPlusFov  = 1;     // [Display] VertPlusFov
static const int kKeepLastCuts = 2;     // [Debris] KeepLastCuts, 0 = off, max 8
static const int kKeepCorpses  = 3;     // [Debris] KeepCorpses, 0 = off, max 8
static const int kKeepGekko    = 0;     // [Debris] KeepGekko: 1 = keep GEKKO bodies and pieces too
static const int kLogDebris    = 0;     // [Debris] LogDebris
static const int kPhotoMode    = 1;     // [PhotoMode] Enabled
static const int kPhotoKey     = 0x79;  // [PhotoMode] Key, F10
static const int kPhotoSpeed   = 400;   // [PhotoMode] SpeedCmPerSec
static const int kWeaponSwitch    = 1;      // [WeaponSwitch] Enabled
static const int kWeaponSwitchPad = 0x0002; // [WeaponSwitch] PadButton, XInput D-pad down
static const int kWeaponSwitchKey = 0;      // [WeaponSwitch] Key, none
static const int kSound        = 1;     // [Launcher] Sound: cut sound effects on/off
static const int kIntro        = 2;     // [Launcher] Intro: 0 = off, 1 = single cut (classic), 2 = flurry

inline int ClampFps(int fps) { return fps < kMinFps ? kMinFps : fps > kMaxFps ? kMaxFps : fps; }

// The first preset above fps, wrapping around to the lowest one.
inline int NextPreset(int fps)
{
    for (int i = 0; i < kPresetCount; ++i)
        if (kPresets[i] > fps) return kPresets[i];
    return kPresets[0];
}

} // namespace cfg
