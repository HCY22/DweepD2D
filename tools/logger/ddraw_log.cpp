// Phase-1 logging proxy DDraw.dll: forwards to the system ddraw.dll and logs every call to ddraw_log.txt
#include "common.h"
#include <ddraw.h>

enum { K_DD = 1, K_SURF, K_PAL, K_CLIP, K_GAMMA };

static const char* const M_DD1[] = { "QueryInterface/2", "AddRef/0", "Release/0", "Compact/0", "CreateClipper/3", "CreatePalette/4",
    "CreateSurface/3", "DuplicateSurface/2", "EnumDisplayModes/4", "EnumSurfaces/4", "FlipToGDISurface/0", "GetCaps/2",
    "GetDisplayMode/1", "GetFourCCCodes/2", "GetGDISurface/1", "GetMonitorFrequency/1", "GetScanLine/1",
    "GetVerticalBlankStatus/1", "Initialize/1", "RestoreDisplayMode/0", "SetCooperativeLevel/2", "SetDisplayMode/3",
    "WaitForVerticalBlank/2" };
static const char* const M_DD7[] = { "QueryInterface/2", "AddRef/0", "Release/0", "Compact/0", "CreateClipper/3", "CreatePalette/4",
    "CreateSurface/3", "DuplicateSurface/2", "EnumDisplayModes/4", "EnumSurfaces/4", "FlipToGDISurface/0", "GetCaps/2",
    "GetDisplayMode/1", "GetFourCCCodes/2", "GetGDISurface/1", "GetMonitorFrequency/1", "GetScanLine/1",
    "GetVerticalBlankStatus/1", "Initialize/1", "RestoreDisplayMode/0", "SetCooperativeLevel/2", "SetDisplayMode/5",
    "WaitForVerticalBlank/2", "GetAvailableVidMem/3", "GetSurfaceFromDC/2", "RestoreAllSurfaces/0",
    "TestCooperativeLevel/0", "GetDeviceIdentifier/2", "StartModeTest/3", "EvaluateMode/2" };
static const char* const M_SURF[] = { "QueryInterface/2", "AddRef/0", "Release/0", "AddAttachedSurface/1", "AddOverlayDirtyRect/1",
    "Blt/5", "BltBatch/3", "BltFast/5", "DeleteAttachedSurface/2", "EnumAttachedSurfaces/2", "EnumOverlayZOrders/3",
    "Flip/2", "GetAttachedSurface/2", "GetBltStatus/1", "GetCaps/1", "GetClipper/1", "GetColorKey/2", "GetDC/1",
    "GetFlipStatus/1", "GetOverlayPosition/2", "GetPalette/1", "GetPixelFormat/1", "GetSurfaceDesc/1", "Initialize/2",
    "IsLost/0", "Lock/4", "ReleaseDC/1", "Restore/0", "SetClipper/1", "SetColorKey/2", "SetOverlayPosition/2",
    "SetPalette/1", "Unlock/1", "UpdateOverlay/5", "UpdateOverlayDisplay/1", "UpdateOverlayZOrder/2",
    "GetDDInterface/1", "PageLock/1", "PageUnlock/1", "SetSurfaceDesc/2", "SetPrivateData/4", "GetPrivateData/3",
    "FreePrivateData/1", "GetUniquenessValue/1", "ChangeUniquenessValue/0", "SetPriority/1", "GetPriority/1",
    "SetLOD/1", "GetLOD/1" };
static const char* const M_PAL[] = { "QueryInterface/2", "AddRef/0", "Release/0", "GetCaps/1", "GetEntries/4", "Initialize/3", "SetEntries/4" };
static const char* const M_CLIP[] = { "QueryInterface/2", "AddRef/0", "Release/0", "GetClipList/3", "GetHWnd/1", "Initialize/2",
    "IsClipListChanged/1", "SetClipList/2", "SetHWnd/2" };
static const char* const M_GAMMA[] = { "QueryInterface/2", "AddRef/0", "Release/0", "GetGammaRamp/2", "SetGammaRamp/2" };

static const IfaceDef D_DD1 = { "IDirectDraw", K_DD, 23, M_DD1 };
static const IfaceDef D_DD2 = { "IDirectDraw2", K_DD, 24, M_DD7 };
static const IfaceDef D_DD4 = { "IDirectDraw4", K_DD, 28, M_DD7 };
static const IfaceDef D_DD7 = { "IDirectDraw7", K_DD, 30, M_DD7 };
static const IfaceDef D_S1 = { "IDirectDrawSurface", K_SURF, 36, M_SURF };
static const IfaceDef D_S2 = { "IDirectDrawSurface2", K_SURF, 39, M_SURF };
static const IfaceDef D_S3 = { "IDirectDrawSurface3", K_SURF, 40, M_SURF };
static const IfaceDef D_S4 = { "IDirectDrawSurface4", K_SURF, 45, M_SURF };
static const IfaceDef D_S7 = { "IDirectDrawSurface7", K_SURF, 49, M_SURF };
static const IfaceDef D_PAL = { "IDirectDrawPalette", K_PAL, 7, M_PAL };
static const IfaceDef D_CLIP = { "IDirectDrawClipper", K_CLIP, 9, M_CLIP };
static const IfaceDef D_GAMMA = { "IDirectDrawGammaControl", K_GAMMA, 5, M_GAMMA };

static const IfaceDef* FromIID(const GUID& g) {
    if (g == IID_IDirectDraw) return &D_DD1;
    if (g == IID_IDirectDraw2) return &D_DD2;
    if (g == IID_IDirectDraw4) return &D_DD4;
    if (g == IID_IDirectDraw7) return &D_DD7;
    if (g == IID_IDirectDrawSurface) return &D_S1;
    if (g == IID_IDirectDrawSurface2) return &D_S2;
    if (g == IID_IDirectDrawSurface3) return &D_S3;
    if (g == IID_IDirectDrawSurface4) return &D_S4;
    if (g == IID_IDirectDrawSurface7) return &D_S7;
    if (g == IID_IDirectDrawPalette) return &D_PAL;
    if (g == IID_IDirectDrawClipper) return &D_CLIP;
    if (g == IID_IDirectDrawGammaControl) return &D_GAMMA;
    return NULL;
}

static const IfaceDef* SurfFor(const IfaceDef* dd) {
    if (dd == &D_DD4) return &D_S4;
    if (dd == &D_DD7) return &D_S7;
    return &D_S1;
}

static void AfterCall(SlotInfo& s, DWORD* a, DWORD hr) {
    if (FAILED(hr)) return;
    int k = s.iface->kind, i = s.slot;
    if (i == 0) { HookVtbl(*(void**)a[2], FromIID(*(GUID*)a[1])); return; }
    if (k == K_DD) {
        if (i == 4) HookVtbl(*(void**)a[2], &D_CLIP);
        else if (i == 5) HookVtbl(*(void**)a[3], &D_PAL);
        else if (i == 6 || i == 7) HookVtbl(*(void**)a[2], SurfFor(s.iface));
        else if (i == 14) HookVtbl(*(void**)a[1], SurfFor(s.iface));
        else if (i == 24) HookVtbl(*(void**)a[2], SurfFor(s.iface));
    } else if (k == K_SURF) {
        if (i == 12) HookVtbl(*(void**)a[2], s.iface);
        else if (i == 15) HookVtbl(*(void**)a[1], &D_CLIP);
        else if (i == 20) HookVtbl(*(void**)a[1], &D_PAL);
    }
}

static void PRect(Buf& b, const char* tag, DWORD p) {
    if (!p) { Ap(b, " %s=NULL", tag); return; }
    RECT* r = (RECT*)p;
    Ap(b, " %s=(%d,%d,%d,%d)", tag, r->left, r->top, r->right, r->bottom);
}

static void PPF(Buf& b, const BYTE* f) {
    Ap(b, " pf{flags=%X fourcc=%X bpp=%u R=%X G=%X B=%X A=%X}", *(DWORD*)(f + 4), *(DWORD*)(f + 8),
       *(DWORD*)(f + 12), *(DWORD*)(f + 16), *(DWORD*)(f + 20), *(DWORD*)(f + 24), *(DWORD*)(f + 28));
}

static void PSD(Buf& b, DWORD p) {
    if (!p) { Ap(b, " sd=NULL"); return; }
    const BYTE* d = (const BYTE*)p;
    DWORD fl = *(DWORD*)(d + 4);
    Ap(b, " sd{size=%u flags=%X w=%u h=%u pitch=%d backbuf=%u surface=%p caps=%X", *(DWORD*)d, fl,
       *(DWORD*)(d + 12), *(DWORD*)(d + 8), *(LONG*)(d + 16), *(DWORD*)(d + 20), *(void**)(d + 36), *(DWORD*)(d + 104));
    if (fl & DDSD_CKSRCBLT) Ap(b, " ckSrcBlt=%X..%X", *(DWORD*)(d + 64), *(DWORD*)(d + 68));
    if (fl & DDSD_PIXELFORMAT) PPF(b, d + 72);
    Ap(b, "}");
}

static void PWnd(Buf& b, HWND h) {
    if (!h) return;
    char cls[64] = "", title[64] = "";
    GetClassNameA(h, cls, 64); GetWindowTextA(h, title, 64);
    RECT wr, cr; GetWindowRect(h, &wr); GetClientRect(h, &cr);
    Ap(b, " wnd{class=\"%s\" title=\"%s\" style=%X exstyle=%X rect=(%d,%d,%d,%d) client=%dx%d}", cls, title,
       GetWindowLongA(h, GWL_STYLE), GetWindowLongA(h, GWL_EXSTYLE), wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom);
}

static void Decode(SlotInfo& s, DWORD* a, DWORD hr, Buf& b) {
    const char* m = s.name;
    int k = s.iface->kind;
    if (s.slot == 0) { char g[64]; GuidStr((GUID*)a[1], g); Ap(b, " riid=%s out=%p", g, SUCCEEDED(hr) ? *(void**)a[2] : 0); return; }
    if (k == K_DD) {
        if (!strcmp(m, "SetCooperativeLevel")) { PWnd(b, (HWND)a[1]); }
        else if (!strcmp(m, "CreateSurface")) { PSD(b, a[1]); if (SUCCEEDED(hr)) Ap(b, " out=%p", *(void**)a[2]); }
        else if (!strcmp(m, "CreatePalette")) {
            Ap(b, " entries:");
            PALETTEENTRY* pe = (PALETTEENTRY*)a[2];
            for (int i = 0; pe && i < 8; i++) Ap(b, " %02X%02X%02X", pe[i].peRed, pe[i].peGreen, pe[i].peBlue);
            if (SUCCEEDED(hr)) Ap(b, " out=%p", *(void**)a[3]);
        }
        else if (!strcmp(m, "CreateClipper")) { if (SUCCEEDED(hr)) Ap(b, " out=%p", *(void**)a[2]); }
        else if (!strcmp(m, "GetDisplayMode")) { if (SUCCEEDED(hr)) PSD(b, a[1]); }
        else if (!strcmp(m, "GetCaps") && SUCCEEDED(hr) && a[1]) {
            DWORD* c = (DWORD*)a[1];
            Ap(b, " hwcaps{caps=%X caps2=%X ckey=%X fx=%X vidmem=%u free=%u}", c[1], c[2], c[3], c[5], c[15], c[16]);
        }
    } else if (k == K_SURF) {
        if (!strcmp(m, "Blt")) {
            PRect(b, "dst", a[1]); Ap(b, " src=%X", a[2]); PRect(b, "srcR", a[3]);
            if (a[4] & DDBLT_COLORFILL && a[5]) Ap(b, " fill=%X", *(DWORD*)(a[5] + 68));
        } else if (!strcmp(m, "BltFast")) { Ap(b, " at=(%d,%d) src=%X", a[1], a[2], a[3]); PRect(b, "srcR", a[4]); }
        else if (!strcmp(m, "Lock")) { PRect(b, "rect", a[1]); if (SUCCEEDED(hr)) PSD(b, a[2]); }
        else if (!strcmp(m, "GetSurfaceDesc")) { if (SUCCEEDED(hr)) PSD(b, a[1]); }
        else if (!strcmp(m, "GetPixelFormat")) { if (SUCCEEDED(hr) && a[1]) PPF(b, (BYTE*)a[1]); }
        else if (!strcmp(m, "GetCaps")) { if (SUCCEEDED(hr) && a[1]) Ap(b, " caps=%X", *(DWORD*)a[1]); }
        else if (!strcmp(m, "GetAttachedSurface")) { Ap(b, " wantCaps=%X", a[1] ? *(DWORD*)a[1] : 0); if (SUCCEEDED(hr)) Ap(b, " out=%p", *(void**)a[2]); }
        else if (!strcmp(m, "SetColorKey") || !strcmp(m, "GetColorKey")) { if (a[2]) Ap(b, " key=%X..%X", ((DWORD*)a[2])[0], ((DWORD*)a[2])[1]); }
        else if (!strcmp(m, "GetDC")) { if (SUCCEEDED(hr)) Ap(b, " hdc=%p", *(void**)a[1]); }
    } else if (k == K_PAL) {
        if (!strcmp(m, "SetEntries") || !strcmp(m, "GetEntries")) {
            Ap(b, " entries:");
            PALETTEENTRY* pe = (PALETTEENTRY*)a[4];
            for (int i = 0; pe && i < 8 && i < (int)a[3]; i++) Ap(b, " %02X%02X%02X", pe[i].peRed, pe[i].peGreen, pe[i].peBlue);
        }
    } else if (k == K_CLIP) {
        if (!strcmp(m, "SetHWnd")) PWnd(b, (HWND)a[2]);
    }
}

// ---------------- exports
static HMODULE g_real;
static FARPROC Real(const char* n) {
    if (!g_real) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\ddraw.dll");
        g_real = LoadLibraryA(p);
        Log("loaded system ddraw: %s -> %p", p, g_real);
    }
    return g_real ? GetProcAddress(g_real, n) : NULL;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreate(GUID* g, LPDIRECTDRAW* out, IUnknown* outer) {
    typedef HRESULT(WINAPI * F)(GUID*, LPDIRECTDRAW*, IUnknown*);
    F f = (F)Real("DirectDrawCreate");
    HRESULT hr = f ? f(g, out, outer) : E_FAIL;
    char gs[64]; GuidStr(g, gs);
    Log("DirectDrawCreate(guid=%s) -> %08X obj=%p", gs, hr, SUCCEEDED(hr) ? *out : 0);
    if (SUCCEEDED(hr)) HookVtbl(*out, &D_DD1);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreateEx(GUID* g, void** out, REFIID iid, IUnknown* outer) {
    typedef HRESULT(WINAPI * F)(GUID*, void**, REFIID, IUnknown*);
    F f = (F)Real("DirectDrawCreateEx");
    HRESULT hr = f ? f(g, out, iid, outer) : E_FAIL;
    Log("DirectDrawCreateEx -> %08X obj=%p", hr, SUCCEEDED(hr) ? *out : 0);
    if (SUCCEEDED(hr)) HookVtbl(*out, FromIID(iid));
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    typedef HRESULT(WINAPI * F)(LPDDENUMCALLBACKA, LPVOID);
    F f = (F)Real("DirectDrawEnumerateA");
    HRESULT hr = f ? f(cb, ctx) : E_FAIL;
    Log("DirectDrawEnumerateA -> %08X", hr);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD fl) {
    typedef HRESULT(WINAPI * F)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
    F f = (F)Real("DirectDrawEnumerateExA");
    HRESULT hr = f ? f(cb, ctx, fl) : E_FAIL;
    Log("DirectDrawEnumerateExA(flags=%X) -> %08X", fl, hr);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) LogOpen("ddraw_log.txt");
    else if (why == DLL_PROCESS_DETACH) { Summary("exit"); Log("log end"); }
    return TRUE;
}
