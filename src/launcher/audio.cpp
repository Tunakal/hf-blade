// HF Blade launcher audio: decodes the sound files next to the exe (hfblade-sounds\*.mp3/*.wav)
// with Media Foundation and plays overlapping one-shots through XAudio2 with per-shot pitch,
// volume and stereo pan. Both are Windows components; nothing to install.
// If no sound files are present, three sword-slice sounds are synthesized in code (see SynthSlice).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
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

// SYNTH-BEGIN
// Procedural sword slice (no third-party audio). Mono, 16-bit, 44.1 kHz, deterministic.
//  1. Whoosh: pink noise through a resonant 2-pole state-variable band-pass whose centre frequency
//     sweeps low -> high -> mid (log-interpolated), ~15 ms attack and an exponential decay.
//  2. Shing: four inharmonic partials (ratios 1, 2.76, 5.40, 8.93) with fast attack and a different
//     exponential decay each, starting shortly after the whoosh, plus a short high-passed noise scrape.
//  3. Both layers are peak-normalised, mixed (shing below the whoosh), faded in (8 ms)/out (30 ms) and scaled to
//     about -3 dBFS. The variant index changes base frequency, sweep, decay and mix.
static const int kSynthRate = 44100;

static void SynthSlice(int variant, std::vector<int16_t>& out)
{
    struct Par { float dur, f0, f1, f2, tPeak, tau, q, base, shingDelay, shingGain, scrape; uint32_t seed; };
    static const Par kPar[3] = {
        { 0.55f,  600.0f, 4000.0f, 1500.0f, 0.12f, 0.090f, 3.0f, 2400.0f, 0.040f, 0.45f, 0.30f, 0x1234ABCDu },
        { 0.45f,  800.0f, 5000.0f, 2000.0f, 0.09f, 0.070f, 2.5f, 3200.0f, 0.030f, 0.38f, 0.40f, 0x9E3779B1u },
        { 0.65f,  500.0f, 3400.0f, 1200.0f, 0.15f, 0.120f, 3.5f, 1800.0f, 0.055f, 0.55f, 0.25f, 0x7F4A7C15u },
    };
    static const float kRatio[4] = { 1.0f, 2.76f, 5.40f, 8.93f };
    static const float kAmp[4]   = { 1.0f, 0.6f, 0.4f, 0.25f };
    static const float kDecay[4] = { 0.45f, 0.30f, 0.22f, 0.15f };   // seconds (time constants)
    const Par& p = kPar[((variant % 3) + 3) % 3];
    const float fs = (float)kSynthRate;
    const float pi = 3.14159265f;
    const int n = (int)(p.dur * fs);
    std::vector<float> wh(n), sh(n);

    uint32_t rng = p.seed;
    auto noise = [&rng]() {   // LCG, white noise in [-1, 1)
        rng = rng * 1664525u + 1013904223u;
        return (float)(int32_t)rng * (1.0f / 2147483648.0f);
    };

    // Whoosh layer
    float b0 = 0, b1 = 0, b2 = 0, lp = 0, bp = 0;
    for (int i = 0; i < n; i++) {
        const float t = i / fs;
        float fc;   // log-interpolated sweep f0 -> f1 (at tPeak) -> f2 (at end)
        if (t < p.tPeak) fc = p.f0 * powf(p.f1 / p.f0, t / p.tPeak);
        else fc = p.f1 * powf(p.f2 / p.f1, (t - p.tPeak) / (p.dur - p.tPeak));
        const float w = noise();
        b0 = 0.99765f * b0 + w * 0.0990460f;   // pink noise (Kellet)
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        const float pink = b0 + b1 + b2 + w * 0.1848f;
        const float f = 2.0f * sinf(pi * fc / fs);
        const float hp = pink - lp - bp / p.q;
        bp += f * hp;
        lp += f * bp;
        const float env = (t < 0.015f ? t / 0.015f : expf(-(t - 0.015f) / p.tau));
        wh[i] = bp * env;
    }

    // Shing layer: inharmonic ring + scrape
    float hpLp = 0;
    for (int i = 0; i < n; i++) {
        const float t = i / fs;
        const float ts = t - p.shingDelay;
        float v = 0;
        if (ts >= 0) {
            const float atk = ts < 0.002f ? ts / 0.002f : 1.0f;
            for (int k = 0; k < 4; k++)
                v += kAmp[k] * sinf(2.0f * pi * p.base * kRatio[k] * ts) * expf(-ts / kDecay[k]);
            v *= atk;
        }
        const float w = noise();
        hpLp += 0.55f * (w - hpLp);   // one-pole low-pass; subtracting it leaves high-passed noise
        v += p.scrape * (w - hpLp) * (t < 0.004f ? t / 0.004f : 1.0f) * expf(-t / 0.012f) * 2.0f;
        sh[i] = v;
    }

    auto peak = [](const std::vector<float>& a) {
        float m = 1e-9f;
        for (float x : a) { const float ax = x < 0 ? -x : x; if (ax > m) m = ax; }
        return m;
    };
    const float pw = 1.0f / peak(wh), ps = p.shingGain / peak(sh);
    std::vector<float> mix(n);
    for (int i = 0; i < n; i++) mix[i] = wh[i] * pw + sh[i] * ps;
    const float g = 0.7079f / peak(mix);   // -3 dBFS
    const int fadeIn = (int)(0.008f * fs), fadeOut = (int)(0.030f * fs);
    out.resize(n);
    for (int i = 0; i < n; i++) {
        float x = mix[i] * g;
        if (i < fadeIn) x *= (float)i / fadeIn;
        if (i >= n - fadeOut) x *= (float)(n - 1 - i) / fadeOut;
        out[i] = (int16_t)lrintf(x * 32767.0f);
    }
}
// SYNTH-END

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
    if (g_samples.empty()) {   // no user files: synthesize three slice variations
        for (int v = 0; v < 3; v++) {
            std::vector<int16_t> pcm;
            SynthSlice(v, pcm);
            Sample s;
            s.fmt.wFormatTag = WAVE_FORMAT_PCM;
            s.fmt.nChannels = 1;
            s.fmt.nSamplesPerSec = kSynthRate;
            s.fmt.wBitsPerSample = 16;
            s.fmt.nBlockAlign = 2;
            s.fmt.nAvgBytesPerSec = kSynthRate * 2;
            s.pcm.resize(pcm.size() * 2);
            memcpy(s.pcm.data(), pcm.data(), s.pcm.size());
            g_samples.push_back(std::move(s));
        }
    }
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
