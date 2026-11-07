// DWGFX.DLL - Dweep Gold drawing layer rebuilt on Direct2D / DirectWrite.
// ddemu.cpp : software DirectDraw (the CPU reference image), window + input hooks
// gfx.cpp   : GPU renderer, detours of the game's blitter functions, GDI text hooks, verification
#pragma once
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <math.h>

// ---- config (DweepD2D.ini)
extern int g_cfgWindow, g_cfgScale, g_cfgFilter, g_cfgVsync, g_cfgStretch;
extern int g_cfgRenderScale;   // 0 = auto
extern int g_cfgVerify;        // 0 off, 1 frame compare, 2 per-primitive compare
extern int g_cfgGpu;           // 0 = present the CPU image only (old proxy behaviour)
extern char g_dir[MAX_PATH];
void Log(const char* f, ...);

// ---- window state (ddemu.cpp)
extern HWND g_hwnd;
extern bool g_modeSet;
extern int g_mw, g_mh;
extern RECT g_dst;
void ComputeDst(int cw, int ch);

struct Surface : IDirectDrawSurface {
    LONG ref = 1;
    int w, h, pitch;
    BYTE* bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = NULL;
    HGDIOBJ oldBmp = NULL;
    DWORD caps;
    bool primary;
    bool hasCk = false;
    DDCOLORKEY ck = { 0, 0 };
    Surface* back = NULL;
    int dcDepth = 0, dcSaved[8];

    Surface(int w_, int h_, DWORD caps_);
    ~Surface();
    void Show();
    void Touched();
    void FillDesc(LPDDSURFACEDESC d, bool withSurface);
    void Copy(const RECT& d, Surface* s, const RECT& r, bool key, DDCOLORKEY k);

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE);
    STDMETHOD(AddOverlayDirtyRect)(LPRECT);
    STDMETHOD(Blt)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDBLTFX);
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD);
    STDMETHOD(BltFast)(DWORD, DWORD, LPDIRECTDRAWSURFACE, LPRECT, DWORD);
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE);
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK);
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK);
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE, DWORD);
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS, LPDIRECTDRAWSURFACE*);
    STDMETHOD(GetBltStatus)(DWORD);
    STDMETHOD(GetCaps)(LPDDSCAPS);
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*);
    STDMETHOD(GetColorKey)(DWORD, LPDDCOLORKEY);
    STDMETHOD(GetDC)(HDC*);
    STDMETHOD(GetFlipStatus)(DWORD);
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG);
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*);
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT);
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC);
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC);
    STDMETHOD(IsLost)();
    STDMETHOD(Lock)(LPRECT, LPDDSURFACEDESC, DWORD, HANDLE);
    STDMETHOD(ReleaseDC)(HDC);
    STDMETHOD(Restore)();
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER);
    STDMETHOD(SetColorKey)(DWORD, LPDDCOLORKEY);
    STDMETHOD(SetOverlayPosition)(LONG, LONG);
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE);
    STDMETHOD(Unlock)(LPVOID);
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX);
    STDMETHOD(UpdateOverlayDisplay)(DWORD);
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE);
};
extern Surface* g_primary;

// ---- gfx.cpp
void Gfx_Init();                                  // DllMain: detours + GDI hooks
void Gfx_Present(Surface* primary);               // primary surface was updated
void Gfx_SurfaceTouched(Surface* s);              // CPU-side write the GPU did not see (Blt, Flip, GDI on primary...)
void Gfx_WindowChanged(HWND old);                 // SetCooperativeLevel switched windows
void Gfx_Resize();
void Gfx_Key(WPARAM vk);
void PatchIAT(HMODULE mod, const char* dll, const char* fn, void* repl);
template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }
