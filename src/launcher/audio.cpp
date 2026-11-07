// HF Blade launcher audio: decodes the sound files next to the exe (hfblade-sounds\*.mp3/*.wav)
// with Media Foundation and plays overlapping one-shots through XAudio2 with per-shot pitch,
// volume and stereo pan. Both are Windows components; nothing to install.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include "audio.h"

namespace {

struct Sample {
    WAVEFORMATEX fmt = {};
    std::vector<BYTE> pcm;
};

std::vector<Sample> g_samples;
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
UINT32 g_masterChannels = 2;
struct Voice { IXAudio2SourceVoice* v; int sample; };
std::vector<Voice> g_voices;
bool g_comInit = false, g_mfInit = false;

bool Decode(const wchar_t* path, Sample& out)
{
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader))) return false;
    bool ok = false;
    IMFMediaType* want = nullptr;
    IMFMediaType* got = nullptr;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(MFCreateMediaType(&want))) {
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want)) &&
            SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) {
            WAVEFORMATEX* wfx = nullptr;
            UINT32 size = 0;
            if (SUCCEEDED(MFCreateWaveFormatExFromMFMediaType(got, &wfx, &size))) {
                out.fmt = *wfx;
                out.fmt.cbSize = 0;
                out.fmt.wFormatTag = WAVE_FORMAT_PCM;
                CoTaskMemFree(wfx);
                for (;;) {
                    DWORD flags = 0;
                    IMFSample* s = nullptr;
                    if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))) break;
                    if (s) {
                        IMFMediaBuffer* buf = nullptr;
                        if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf))) {
                            BYTE* data = nullptr;
                            DWORD len = 0;
                            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                                out.pcm.insert(out.pcm.end(), data, data + len);
                                buf->Unlock();
                            }
                            buf->Release();
                        }
                        s->Release();
                    }
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                }
                ok = !out.pcm.empty() && out.fmt.nChannels >= 1 && out.fmt.nChannels <= 2;
            }
        }
    }
    if (got) got->Release();
    if (want) want->Release();
    reader->Release();
    return ok;
}

}   // namespace

bool Audio_Init(const wchar_t* exeDir)
{
    if (g_xa) return true;
    g_comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    g_mfInit = true;

    const wchar_t* patterns[] = { L"*.mp3", L"*.wav" };
    for (const wchar_t* pat : patterns) {
        wchar_t glob[MAX_PATH];
        swprintf_s(glob, L"%shfblade-sounds\\%s", exeDir, pat);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%shfblade-sounds\\%s", exeDir, fd.cFileName);
            Sample s;
            if (Decode(path, s)) g_samples.push_back(std::move(s));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_samples.empty()) return false;
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { g_xa = nullptr; return false; }
    if (FAILED(g_xa->CreateMasteringVoice(&g_master))) { g_xa->Release(); g_xa = nullptr; return false; }
    XAUDIO2_VOICE_DETAILS d;
    g_master->GetVoiceDetails(&d);
    g_masterChannels = d.InputChannels;
    return true;
}

int Audio_SampleCount() { return (int)g_samples.size(); }

void Audio_Play(int sample, float pitch, float volume, float pan)
{
    if (!g_xa || g_samples.empty()) return;
    sample = ((sample % (int)g_samples.size()) + (int)g_samples.size()) % (int)g_samples.size();
    const Sample& s = g_samples[sample];

    // Reuse a finished voice of the same format, else create one (bounded pool).
    IXAudio2SourceVoice* voice = nullptr;
    for (Voice& v : g_voices) {
        XAUDIO2_VOICE_STATE st;
        v.v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0 && g_samples[v.sample].fmt.nChannels == s.fmt.nChannels &&
            g_samples[v.sample].fmt.nSamplesPerSec == s.fmt.nSamplesPerSec) { voice = v.v; v.sample = sample; break; }
    }
    if (!voice) {
        if (g_voices.size() >= 32) return;
        if (FAILED(g_xa->CreateSourceVoice(&voice, &s.fmt, 0, 4.0f))) return;
        g_voices.push_back({ voice, sample });
    }
    voice->Stop();
    voice->FlushSourceBuffers();
    voice->SetFrequencyRatio(pitch < 0.25f ? 0.25f : pitch > 4.0f ? 4.0f : pitch);
    voice->SetVolume(volume);
    if (g_masterChannels == 2) {   // equal-power stereo pan
        const float p = pan < -1 ? -1 : pan > 1 ? 1 : pan;
        const float l = cosf((p + 1) * 0.7853982f), r = sinf((p + 1) * 0.7853982f);
        float m[4];
        if (s.fmt.nChannels == 1) { m[0] = l; m[1] = r; }
        else { m[0] = l; m[1] = 0; m[2] = 0; m[3] = r; }
        voice->SetOutputMatrix(g_master, s.fmt.nChannels, 2, m);
    }
    XAUDIO2_BUFFER b = {};
    b.AudioBytes = (UINT32)s.pcm.size();
    b.pAudioData = s.pcm.data();
    b.Flags = XAUDIO2_END_OF_STREAM;
    if (SUCCEEDED(voice->SubmitSourceBuffer(&b))) voice->Start();
}

void Audio_StopAll()
{
    for (Voice& v : g_voices) { v.v->Stop(); v.v->FlushSourceBuffers(); }
}

void Audio_Shutdown()
{
    for (Voice& v : g_voices) v.v->DestroyVoice();
    g_voices.clear();
    if (g_master) g_master->DestroyVoice();
    g_master = nullptr;
    if (g_xa) g_xa->Release();
    g_xa = nullptr;
    g_samples.clear();
    if (g_mfInit) MFShutdown();
    if (g_comInit) CoUninitialize();
}
