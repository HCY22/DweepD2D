// DSOUND.dll replacement for Dweep Gold: the DirectSound buffer API implemented on XAudio2.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmreg.h>
#include <dsound.h>
#include <xaudio2.h>
#include <stdio.h>
#include <math.h>

static char g_dir[MAX_PATH];
static void Log(const char* f, ...) {
    char p[MAX_PATH]; sprintf(p, "%sDweepD2D_log.txt", g_dir);
    FILE* fp = fopen(p, "a"); if (!fp) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(fp, "%02d:%02d:%02d.%03d [audio] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list v; va_start(v, f); vfprintf(fp, f, v); va_end(v);
    fputc('\n', fp); fclose(fp);
}

static IXAudio2* g_xa;
static IXAudio2MasteringVoice* g_master;
static DWORD g_masterChannels = 2;

struct SharedData {
    LONG ref = 1; BYTE* p; DWORD n;
    SharedData(DWORD n_) : n(n_) { p = new BYTE[n ? n : 1]; memset(p, 0, n); }
    ~SharedData() { delete[] p; }
    void AddRef() { InterlockedIncrement(&ref); }
    void Release() { if (!InterlockedDecrement(&ref)) delete this; }
};

struct Buffer : IDirectSoundBuffer {
    LONG ref = 1;
    bool primary;
    DWORD flags;
    WAVEFORMATEX wfx;
    SharedData* data;
    IXAudio2SourceVoice* voice = NULL;
    bool started = false, looping = false;
    DWORD pos = 0;           // play cursor while stopped / where the current submission began
    UINT64 base = 0;         // SamplesPlayed when the current submission began
    LONG vol = 0, pan = 0;
    DWORD freq = 0;          // 0 = original

    Buffer(bool prim, DWORD fl, const WAVEFORMATEX* f, SharedData* d) : primary(prim), flags(fl), data(d) {
        if (f) wfx = *f;
        else { wfx = { WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0 }; }
        wfx.cbSize = 0;
    }
    ~Buffer() {
        if (voice) voice->DestroyVoice();
        if (data) data->Release();
    }

    bool EnsureVoice() {
        if (voice) return true;
        if (!g_xa) return false;
        HRESULT hr = g_xa->CreateSourceVoice(&voice, &wfx, 0, XAUDIO2_MAX_FREQ_RATIO);
        if (FAILED(hr)) { Log("CreateSourceVoice failed %08X (tag=%u ch=%u rate=%u bits=%u)", hr, wfx.wFormatTag, wfx.nChannels, wfx.nSamplesPerSec, wfx.wBitsPerSample); voice = NULL; return false; }
        ApplyVolume(); ApplyPan(); ApplyFreq();
        return true;
    }
    void ApplyVolume() { if (voice) voice->SetVolume(vol <= DSBVOLUME_MIN ? 0.f : powf(10.f, vol / 2000.f)); }
    void ApplyFreq() { if (voice) voice->SetFrequencyRatio(freq ? (float)freq / wfx.nSamplesPerSec : 1.f); }
    void ApplyPan() {
        // pan 0 keeps XAudio2's default channel mapping; otherwise route to front left/right only
        if (!voice || !pan || g_masterChannels < 2 || g_masterChannels > 32 || wfx.nChannels > 2) return;
        float l = 1.f, r = 1.f;
        if (pan < 0) r = powf(10.f, pan / 2000.f); else l = powf(10.f, -pan / 2000.f);
        UINT32 src = wfx.nChannels, dst = g_masterChannels;
        float m[2 * 32] = {};                       // m[d * src + s]
        if (src == 1) { m[0] = l; m[1] = r; }
        else { m[0 * src + 0] = l; m[1 * src + 1] = r; }
        voice->SetOutputMatrix(NULL, src, dst, m);
    }
    bool IsPlaying() {
        if (!voice || !started) return false;
        XAUDIO2_VOICE_STATE st; voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) { started = false; pos = 0; voice->Stop(); return false; }
        return true;
    }
    DWORD Cursor() {
        if (!IsPlaying()) return pos;
        XAUDIO2_VOICE_STATE st; voice->GetState(&st);
        UINT64 bytes = (st.SamplesPlayed - base) * wfx.nBlockAlign + pos;
        if (looping) return (DWORD)(bytes % data->n);
        return (DWORD)min(bytes, (UINT64)data->n);
    }
    void Halt() {
        if (!voice || !started) return;
        pos = Cursor();
        voice->Stop();
        voice->FlushSourceBuffers();
        started = false;
    }
    HRESULT Start(bool loop) {
        if (!EnsureVoice()) return DSERR_GENERIC;
        voice->Stop();
        voice->FlushSourceBuffers();
        DWORD align = wfx.nBlockAlign ? wfx.nBlockAlign : 1;
        if (pos >= data->n) pos = 0;
        pos -= pos % align;
        XAUDIO2_BUFFER b = {};
        b.AudioBytes = data->n - data->n % align;
        b.pAudioData = data->p;
        b.PlayBegin = pos / align;
        b.Flags = XAUDIO2_END_OF_STREAM;
        if (loop) { b.LoopBegin = 0; b.LoopLength = 0; b.LoopCount = XAUDIO2_LOOP_INFINITE; }
        HRESULT hr = voice->SubmitSourceBuffer(&b);
        if (FAILED(hr)) { Log("SubmitSourceBuffer failed %08X", hr); return DSERR_GENERIC; }
        XAUDIO2_VOICE_STATE st; voice->GetState(&st);
        base = st.SamplesPlayed;
        looping = loop;
        voice->Start();
        started = true;
        return DS_OK;
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectSoundBuffer) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    // IDirectSoundBuffer
    STDMETHOD(GetCaps)(LPDSBCAPS c) {
        if (!c || c->dwSize < sizeof(DSBCAPS)) return DSERR_INVALIDPARAM;
        c->dwFlags = flags; c->dwBufferBytes = primary ? 0 : data->n; c->dwUnlockTransferRate = 0; c->dwPlayCpuOverhead = 0;
        return DS_OK;
    }
    STDMETHOD(GetCurrentPosition)(LPDWORD play, LPDWORD write) {
        DWORD c = primary ? 0 : Cursor();
        if (play) *play = c;
        if (write) *write = c;
        return DS_OK;
    }
    STDMETHOD(GetFormat)(LPWAVEFORMATEX f, DWORD sz, LPDWORD written) {
        DWORD n = sizeof(WAVEFORMATEX);
        if (written) *written = n;
        if (f) memcpy(f, &wfx, min(sz, n));
        return DS_OK;
    }
    STDMETHOD(GetVolume)(LPLONG v) { if (!v) return DSERR_INVALIDPARAM; *v = vol; return DS_OK; }
    STDMETHOD(GetPan)(LPLONG p) { if (!p) return DSERR_INVALIDPARAM; *p = pan; return DS_OK; }
    STDMETHOD(GetFrequency)(LPDWORD f) { if (!f) return DSERR_INVALIDPARAM; *f = freq ? freq : wfx.nSamplesPerSec; return DS_OK; }
    STDMETHOD(GetStatus)(LPDWORD s) {
        if (!s) return DSERR_INVALIDPARAM;
        *s = (!primary && IsPlaying()) ? (DSBSTATUS_PLAYING | (looping ? DSBSTATUS_LOOPING : 0)) : 0;
        return DS_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTSOUND, LPCDSBUFFERDESC) { return DSERR_ALREADYINITIALIZED; }
    STDMETHOD(Lock)(DWORD off, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD fl) {
        if (primary) return DSERR_INVALIDCALL;
        if (!p1 || !n1) return DSERR_INVALIDPARAM;
        DWORD n = data->n;
        if (fl & DSBLOCK_ENTIREBUFFER) bytes = n;
        if (fl & DSBLOCK_FROMWRITECURSOR) off = Cursor();
        if (off >= n || bytes > n) return DSERR_INVALIDPARAM;
        *p1 = data->p + off;
        *n1 = min(bytes, n - off);
        DWORD rest = bytes - *n1;
        if (p2) *p2 = rest ? data->p : NULL;
        if (n2) *n2 = rest;
        return DS_OK;
    }
    STDMETHOD(Play)(DWORD, DWORD, DWORD fl) {
        if (primary) return DS_OK;
        bool loop = (fl & DSBPLAY_LOOPING) != 0;
        if (IsPlaying()) {
            if (loop == looping) return DS_OK;
            Halt();
        }
        return Start(loop);
    }
    STDMETHOD(SetCurrentPosition)(DWORD p) {
        if (primary) return DS_OK;
        if (p >= data->n) p = 0;
        if (IsPlaying()) { bool loop = looping; Halt(); pos = p; return Start(loop); }
        pos = p;
        return DS_OK;
    }
    STDMETHOD(SetFormat)(LPCWAVEFORMATEX) { return DS_OK; }   // only meaningful for the primary buffer
    STDMETHOD(SetVolume)(LONG v) { vol = max((LONG)DSBVOLUME_MIN, min((LONG)DSBVOLUME_MAX, v)); ApplyVolume(); return DS_OK; }
    STDMETHOD(SetPan)(LONG p) { pan = max((LONG)DSBPAN_LEFT, min((LONG)DSBPAN_RIGHT, p)); ApplyPan(); return DS_OK; }
    STDMETHOD(SetFrequency)(DWORD f) { freq = f; ApplyFreq(); return DS_OK; }
    STDMETHOD(Stop)() { if (!primary) Halt(); return DS_OK; }
    STDMETHOD(Unlock)(LPVOID, DWORD, LPVOID, DWORD) { return DS_OK; }
    STDMETHOD(Restore)() { return DS_OK; }
};

struct DSound : IDirectSound {
    LONG ref = 1;

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectSound) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN) {
        if (!d || !out) return DSERR_INVALIDPARAM;
        *out = NULL;
        if (d->dwFlags & DSBCAPS_PRIMARYBUFFER) { *out = new Buffer(true, d->dwFlags, NULL, new SharedData(0)); return DS_OK; }
        const WAVEFORMATEX* f = d->lpwfxFormat;
        if (!f || d->dwBufferBytes < 4 || d->dwBufferBytes > DSBSIZE_MAX) return DSERR_INVALIDPARAM;
        if (f->wFormatTag != WAVE_FORMAT_PCM) { Log("unsupported format tag %u", f->wFormatTag); return DSERR_BADFORMAT; }
        *out = new Buffer(false, d->dwFlags, f, new SharedData(d->dwBufferBytes));
        return DS_OK;
    }
    STDMETHOD(GetCaps)(LPDSCAPS c) {
        if (!c || c->dwSize < sizeof(DSCAPS)) return DSERR_INVALIDPARAM;
        DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                     DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT | DSCAPS_CONTINUOUSRATE;
        c->dwMinSecondarySampleRate = 1000; c->dwMaxSecondarySampleRate = 200000;
        c->dwPrimaryBuffers = 1; c->dwMaxHwMixingAllBuffers = c->dwFreeHwMixingAllBuffers = 256;
        return DS_OK;
    }
    STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER* out) {
        if (!src || !out) return DSERR_INVALIDPARAM;
        Buffer* s = (Buffer*)src;
        if (s->primary) return DSERR_INVALIDCALL;
        s->data->AddRef();
        Buffer* b = new Buffer(false, s->flags, &s->wfx, s->data);
        b->vol = s->vol; b->pan = s->pan; b->freq = s->freq;
        *out = b;
        return DS_OK;
    }
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) { return DS_OK; }
    STDMETHOD(Compact)() { return DS_OK; }
    STDMETHOD(GetSpeakerConfig)(LPDWORD c) { if (!c) return DSERR_INVALIDPARAM; *c = DSSPEAKER_STEREO; return DS_OK; }
    STDMETHOD(SetSpeakerConfig)(DWORD) { return DS_OK; }
    STDMETHOD(Initialize)(LPCGUID) { return DSERR_ALREADYINITIALIZED; }
};

static bool InitAudio() {
    if (g_xa) return true;
    HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) { Log("XAudio2Create failed %08X", hr); g_xa = NULL; return false; }
    hr = g_xa->CreateMasteringVoice(&g_master);
    if (FAILED(hr)) { Log("CreateMasteringVoice failed %08X", hr); g_xa->Release(); g_xa = NULL; return false; }
    XAUDIO2_VOICE_DETAILS det; g_master->GetVoiceDetails(&det);
    g_masterChannels = det.InputChannels;
    Log("XAudio2 ready, %u channels @ %u Hz", det.InputChannels, det.InputSampleRate);
    return true;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCreate(LPCGUID, LPDIRECTSOUND* out, LPUNKNOWN) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (!InitAudio()) return DSERR_NODRIVER;
    *out = new DSound();
    return DS_OK;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx) {
    if (cb) cb(NULL, "XAudio2", "", ctx);
    return DS_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        char* s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
    }
    return TRUE;
}
