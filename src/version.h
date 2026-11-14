// HF Blade version: the only place to change it. Used by the mod, the launcher, both resource files
// (VERSIONINFO, shown in the file's Properties > Details) and tools/make_release.py.
#pragma once

#define HFB_VERSION_MAJOR 0
#define HFB_VERSION_MINOR 1
#define HFB_VERSION_PATCH 1

#define HFB_STR2(x) #x
#define HFB_STR(x) HFB_STR2(x)
#define HFB_VERSION_STR HFB_STR(HFB_VERSION_MAJOR) "." HFB_STR(HFB_VERSION_MINOR) "." HFB_STR(HFB_VERSION_PATCH)
#define HFB_VERSION_NUM HFB_VERSION_MAJOR, HFB_VERSION_MINOR, HFB_VERSION_PATCH, 0

#define HFB_COPYRIGHT  "Copyright (c) 2026 Tunahan. MIT License."
#define HFB_HOMEPAGE   "https://github.com/Tunakal/hf-blade"
