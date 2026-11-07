#pragma once
// Launcher sound effects (see audio.cpp).
bool Audio_Init(const wchar_t* exeDir);   // false if no sounds or no audio device
int  Audio_SampleCount();
void Audio_Play(int sample, float pitch, float volume, float pan);   // pan -1 (left) .. 1 (right)
void Audio_StopAll();
void Audio_Shutdown();
