// Phase-1 logging proxy DSOUND.dll: forwards to the system dsound.dll and logs every call to dsound_log.txt
#include "common.h"
#include <mmreg.h>
#include <dsound.h>

enum { K_DS = 1, K_BUF, K_NOTIFY };

static const char* const M_DS[] = { "QueryInterface/2", "AddRef/0", "Release/0", "CreateSoundBuffer/3", "GetCaps/1",
    "DuplicateSoundBuffer/2", "SetCooperativeLevel/2", "Compact/0", "GetSpeakerConfig/1", "SetSpeakerConfig/1",
    "Initialize/1", "VerifyCertification/1" };
static const char* const M_BUF[] = { "QueryInterface/2", "AddRef/0", "Release/0", "GetCaps/1", "GetCurrentPosition/2",
    "GetFormat/3", "GetVolume/1", "GetPan/1", "GetFrequency/1", "GetStatus/1", "Initialize/2", "Lock/7", "Play/3",
    "SetCurrentPosition/1", "SetFormat/1", "SetVolume/1", "SetPan/1", "SetFrequency/1", "Stop/0", "Unlock/4",
    "Restore/0", "SetFX/3", "AcquireResources/3", "GetObjectInPath/4" };
static const char* const M_NOTIFY[] = { "QueryInterface/2", "AddRef/0", "Release/0", "SetNotificationPositions/2" };

static const IfaceDef D_DS = { "IDirectSound", K_DS, 11, M_DS };
static const IfaceDef D_DS8 = { "IDirectSound8", K_DS, 12, M_DS };
static const IfaceDef D_BUF = { "IDirectSoundBuffer", K_BUF, 21, M_BUF };
static const IfaceDef D_BUF8 = { "IDirectSoundBuffer8", K_BUF, 24, M_BUF };
static const IfaceDef D_NOTIFY = { "IDirectSoundNotify", K_NOTIFY, 4, M_NOTIFY };

static const IfaceDef* FromIID(const GUID& g) {
    if (g == IID_IDirectSound) return &D_DS;
    if (g == IID_IDirectSound8) return &D_DS8;
    if (g == IID_IDirectSoundBuffer) return &D_BUF;
    if (g == IID_IDirectSoundBuffer8) return &D_BUF8;
    if (g == IID_IDirectSoundNotify) return &D_NOTIFY;
    return NULL;
}

static void AfterCall(SlotInfo& s, DWORD* a, DWORD hr) {
    if (FAILED(hr)) return;
    int k = s.iface->kind, i = s.slot;
    if (i == 0) { HookVtbl(*(void**)a[2], FromIID(*(GUID*)a[1])); return; }
    if (k == K_DS && (i == 3 || i == 5)) HookVtbl(*(void**)a[2], &D_BUF);
}

static void PWfx(Buf& b, const WAVEFORMATEX* w) {
    if (!w) { Ap(b, " wfx=NULL"); return; }
    Ap(b, " wfx{tag=%u ch=%u rate=%u bits=%u align=%u}", w->wFormatTag, w->nChannels, w->nSamplesPerSec, w->wBitsPerSample, w->nBlockAlign);
}

static void Decode(SlotInfo& s, DWORD* a, DWORD hr, Buf& b) {
    const char* m = s.name;
    if (s.slot == 0) { char g[64]; GuidStr((GUID*)a[1], g); Ap(b, " riid=%s out=%p", g, SUCCEEDED(hr) ? *(void**)a[2] : 0); return; }
    if (s.iface->kind == K_DS) {
        if (!strcmp(m, "CreateSoundBuffer") && a[1]) {
            DSBUFFERDESC* d = (DSBUFFERDESC*)a[1];
            Ap(b, " desc{size=%u flags=%X bytes=%u}", d->dwSize, d->dwFlags, d->dwBufferBytes);
            PWfx(b, d->lpwfxFormat);
            if (SUCCEEDED(hr)) Ap(b, " out=%p", *(void**)a[2]);
        } else if (!strcmp(m, "DuplicateSoundBuffer")) { if (SUCCEEDED(hr)) Ap(b, " out=%p", *(void**)a[2]); }
        else if (!strcmp(m, "SetCooperativeLevel")) { char c[64] = ""; GetClassNameA((HWND)a[1], c, 64); Ap(b, " wndclass=\"%s\" level=%u", c, a[2]); }
    } else if (s.iface->kind == K_BUF) {
        if (!strcmp(m, "Play")) Ap(b, " flags=%s", (a[3] & DSBPLAY_LOOPING) ? "LOOPING" : "once");
        else if (!strcmp(m, "SetVolume") || !strcmp(m, "SetPan")) Ap(b, " value=%d", (LONG)a[1]);
        else if (!strcmp(m, "SetFrequency")) Ap(b, " hz=%u", a[1]);
        else if (!strcmp(m, "SetFormat")) PWfx(b, (WAVEFORMATEX*)a[1]);
        else if (!strcmp(m, "GetStatus")) { if (SUCCEEDED(hr) && a[1]) Ap(b, " status=%X", *(DWORD*)a[1]); }
        else if (!strcmp(m, "GetCurrentPosition") && SUCCEEDED(hr)) Ap(b, " play=%u write=%u", a[1] ? *(DWORD*)a[1] : 0, a[2] ? *(DWORD*)a[2] : 0);
        else if (!strcmp(m, "GetCaps") && SUCCEEDED(hr) && a[1]) { DSBCAPS* c = (DSBCAPS*)a[1]; Ap(b, " caps{flags=%X bytes=%u}", c->dwFlags, c->dwBufferBytes); }
        else if (!strcmp(m, "Lock")) Ap(b, " off=%u bytes=%u flags=%X", a[1], a[2], a[7]);
    }
}

// ---------------- exports
static HMODULE g_real;
static FARPROC Real(const char* n) {
    if (!g_real) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\dsound.dll");
        g_real = LoadLibraryA(p);
        Log("loaded system dsound: %s -> %p", p, g_real);
    }
    return g_real ? GetProcAddress(g_real, n) : NULL;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCreate(LPCGUID g, LPDIRECTSOUND* out, LPUNKNOWN outer) {
    typedef HRESULT(WINAPI * F)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
    F f = (F)Real("DirectSoundCreate");
    HRESULT hr = f ? f(g, out, outer) : E_FAIL;
    char gs[64]; GuidStr(g, gs);
    Log("DirectSoundCreate(guid=%s) -> %08X obj=%p", gs, hr, SUCCEEDED(hr) ? *out : 0);
    if (SUCCEEDED(hr)) HookVtbl(*out, &D_DS);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCreate8(LPCGUID g, LPDIRECTSOUND8* out, LPUNKNOWN outer) {
    typedef HRESULT(WINAPI * F)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
    F f = (F)Real("DirectSoundCreate8");
    HRESULT hr = f ? f(g, out, outer) : E_FAIL;
    Log("DirectSoundCreate8 -> %08X obj=%p", hr, SUCCEEDED(hr) ? *out : 0);
    if (SUCCEEDED(hr)) HookVtbl(*out, &D_DS8);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx) {
    typedef HRESULT(WINAPI * F)(LPDSENUMCALLBACKA, LPVOID);
    F f = (F)Real("DirectSoundEnumerateA");
    HRESULT hr = f ? f(cb, ctx) : E_FAIL;
    Log("DirectSoundEnumerateA -> %08X", hr);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx) {
    typedef HRESULT(WINAPI * F)(LPDSENUMCALLBACKW, LPVOID);
    F f = (F)Real("DirectSoundEnumerateW");
    HRESULT hr = f ? f(cb, ctx) : E_FAIL;
    Log("DirectSoundEnumerateW -> %08X", hr);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) LogOpen("dsound_log.txt");
    else if (why == DLL_PROCESS_DETACH) { Summary("exit"); Log("log end"); }
    return TRUE;
}
