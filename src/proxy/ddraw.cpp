// DDraw.dll replacement for Dweep Gold: DirectDraw 1 API implemented in software, presented with Direct2D.
// The display mode is never changed: the game's 640x480x16 frame is scaled into its window.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <ddraw.h>
#include <d2d1.h>
#include <stdio.h>
#include <math.h>

// ------------------------------------------------------------------ config + log
static int g_cfgWindow = 0;      // 0 = borderless fullscreen, 1 = window
static int g_cfgScale = 2;       // window size multiplier
static int g_cfgFilter = 0;      // 0 sharp, 1 nearest, 2 linear, 3 integer
static int g_cfgVsync = 0;
static int g_cfgStretch = 0;     // 1 = fill the whole window, ignoring 4:3
static char g_dir[MAX_PATH];

static void Log(const char* f, ...) {
    char p[MAX_PATH]; sprintf(p, "%sDweepD2D_log.txt", g_dir);
    FILE* fp = fopen(p, "a"); if (!fp) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(fp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list v; va_start(v, f); vfprintf(fp, f, v); va_end(v);
    fputc('\n', fp); fclose(fp);
}

static void LoadConfig() {
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char* s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
    char ini[MAX_PATH]; sprintf(ini, "%sDweepD2D.ini", g_dir);
    char v[64];
    GetPrivateProfileStringA("Display", "Mode", "fullscreen", v, 64, ini);
    g_cfgWindow = !_stricmp(v, "window");
    g_cfgScale = GetPrivateProfileIntA("Display", "WindowScale", 2, ini);
    if (g_cfgScale < 1) g_cfgScale = 1;
    GetPrivateProfileStringA("Display", "Filter", "sharp", v, 64, ini);
    g_cfgFilter = !_stricmp(v, "nearest") ? 1 : !_stricmp(v, "linear") ? 2 : !_stricmp(v, "integer") ? 3 : 0;
    g_cfgVsync = GetPrivateProfileIntA("Display", "VSync", 0, ini);
    GetPrivateProfileStringA("Display", "Aspect", "keep", v, 64, ini);
    g_cfgStretch = !_stricmp(v, "stretch");
}

// ------------------------------------------------------------------ window state
static HWND g_hwnd;
static WNDPROC g_oldProc;
static bool g_modeSet;
static int g_mw = 640, g_mh = 480;
static RECT g_dst = { 0, 0, 640, 480 };     // where the game image lands in the client area
struct Surface;
static Surface* g_primary;

static void ComputeDst(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    if (g_cfgStretch) { g_dst = { 0, 0, cw, ch }; return; }
    double s = min((double)cw / g_mw, (double)ch / g_mh);
    if (g_cfgFilter == 3 && s >= 1) s = floor(s);
    int w = (int)(g_mw * s + 0.5), h = (int)(g_mh * s + 0.5);
    g_dst.left = (cw - w) / 2; g_dst.top = (ch - h) / 2;
    g_dst.right = g_dst.left + w; g_dst.bottom = g_dst.top + h;
}

static void ClientToGame(LONG& x, LONG& y) {
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    if (dw <= 0 || dh <= 0) return;
    x = (LONG)floor((x - g_dst.left) * (double)g_mw / dw);
    y = (LONG)floor((y - g_dst.top) * (double)g_mh / dh);
    x = max(0, min(g_mw - 1, x)); y = max(0, min(g_mh - 1, y));
}

static void GameToClient(LONG& x, LONG& y) {
    x = g_dst.left + (LONG)((x + 0.5) * (g_dst.right - g_dst.left) / g_mw);
    y = g_dst.top + (LONG)((y + 0.5) * (g_dst.bottom - g_dst.top) / g_mh);
}

static void PlaceWindow() {
    if (!g_hwnd) return;
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r;
    DWORD style, ex = GetWindowLongA(g_hwnd, GWL_EXSTYLE) & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    if (g_cfgWindow) {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE | WS_CLIPSIBLINGS;
        r = { 0, 0, g_mw * g_cfgScale, g_mh * g_cfgScale };
        AdjustWindowRectEx(&r, style, FALSE, ex);
        int w = r.right - r.left, h = r.bottom - r.top;
        RECT& wa = mi.rcWork;
        r.left = wa.left + max(0L, ((wa.right - wa.left) - w) / 2);
        r.top = wa.top + max(0L, ((wa.bottom - wa.top) - h) / 2);
        r.right = r.left + w; r.bottom = r.top + h;
    } else {
        style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS;
        r = mi.rcMonitor;
    }
    SetWindowLongA(g_hwnd, GWL_STYLE, style);
    SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    RECT c; GetClientRect(g_hwnd, &c);
    ComputeDst(c.right, c.bottom);
}

// ------------------------------------------------------------------ Direct2D presenter
static ID2D1Factory* g_fac;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1Bitmap* g_bmp;
static ID2D1BitmapRenderTarget* g_up;
static int g_upK;
static UINT32* g_lut;
static UINT32* g_conv;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

static void DropDevice() { SafeRelease(g_up); g_upK = 0; SafeRelease(g_bmp); SafeRelease(g_rt); }

static void Present(const BYTE* bits, int pitch, int w, int h) {
    if (!g_hwnd || IsIconic(g_hwnd)) return;
    RECT c; GetClientRect(g_hwnd, &c);
    if (c.right <= 0 || c.bottom <= 0) return;
    if (!g_fac && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_fac))) { Log("D2D1CreateFactory failed"); return; }
    if (!g_lut) {
        g_lut = new UINT32[65536];
        for (int i = 0; i < 65536; i++) {
            int r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
            g_lut[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
    D2D1_SIZE_U cs = D2D1::SizeU(c.right, c.bottom);
    if (!g_rt) {
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        HRESULT hr = g_fac->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_hwnd, cs,
            g_cfgVsync ? D2D1_PRESENT_OPTIONS_NONE : D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_rt);
        if (FAILED(hr)) { Log("CreateHwndRenderTarget failed %08X", hr); return; }
    } else {
        D2D1_SIZE_U ps = g_rt->GetPixelSize();
        if (ps.width != cs.width || ps.height != cs.height) g_rt->Resize(cs);
    }
    ComputeDst(c.right, c.bottom);
    if (g_bmp) { D2D1_SIZE_U bs = g_bmp->GetPixelSize(); if ((int)bs.width != w || (int)bs.height != h) { SafeRelease(g_bmp); SafeRelease(g_up); g_upK = 0; } }
    if (!g_bmp) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(g_rt->CreateBitmap(D2D1::SizeU(w, h), bp, &g_bmp))) { Log("CreateBitmap failed"); return; }
        delete[] g_conv; g_conv = new UINT32[w * h];
    }
    for (int y = 0; y < h; y++) {
        const WORD* s = (const WORD*)(bits + y * pitch);
        UINT32* d = g_conv + y * w;
        for (int x = 0; x < w; x++) d[x] = g_lut[s[x]];
    }
    g_bmp->CopyFromMemory(NULL, g_conv, w * 4);

    D2D1_RECT_F dst = D2D1::RectF((float)g_dst.left, (float)g_dst.top, (float)g_dst.right, (float)g_dst.bottom);
    double sx = (double)(g_dst.right - g_dst.left) / w, sy = (double)(g_dst.bottom - g_dst.top) / h;
    double scale = max(sx, sy);
    bool fractional = fabs(sx - floor(sx + 0.5)) > 1e-6 || fabs(sy - floor(sy + 0.5)) > 1e-6;
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0, 0, 0));
    if (g_cfgFilter == 0 && fractional && scale > 1) {
        // sharp: integer nearest-neighbour upscale first, then a smooth downscale to the final size
        int k = (int)ceil(scale);
        if (k != g_upK) {
            SafeRelease(g_up);
            if (SUCCEEDED(g_rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)w * k, (float)h * k), D2D1::SizeU(w * k, h * k),
                                                             &g_up))) g_upK = k;
        }
        if (g_up) {
            g_up->BeginDraw();
            g_up->DrawBitmap(g_bmp, D2D1::RectF(0, 0, (float)w * k, (float)h * k), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            g_up->EndDraw();
            ID2D1Bitmap* ub = NULL;
            g_up->GetBitmap(&ub);
            g_rt->DrawBitmap(ub, dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            SafeRelease(ub);
        }
    } else {
        g_rt->DrawBitmap(g_bmp, dst, 1.f, g_cfgFilter == 2 ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { Log("render target lost, recreating"); DropDevice(); }
}

// ------------------------------------------------------------------ surfaces
static const DWORD CAPS_PRIMARY = DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;

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
    Surface* back = NULL;     // attached back buffer (flip chains)

    Surface(int w_, int h_, DWORD caps_) : w(w_), h(h_), caps(caps_), primary(!!(caps_ & DDSCAPS_PRIMARYSURFACE)) {
        struct { BITMAPINFOHEADER h; DWORD m[3]; } bi = {};
        bi.h.biSize = sizeof bi.h; bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1;
        bi.h.biBitCount = 16; bi.h.biCompression = BI_BITFIELDS;
        bi.m[0] = 0xF800; bi.m[1] = 0x07E0; bi.m[2] = 0x001F;
        void* p = NULL;
        bmp = CreateDIBSection(NULL, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &p, NULL, 0);
        bits = (BYTE*)p;
        pitch = ((w * 16 + 31) / 32) * 4;
        dc = CreateCompatibleDC(NULL);
        oldBmp = SelectObject(dc, bmp);
        if (bits) memset(bits, 0, pitch * h);
    }
    ~Surface() {
        if (back) back->Release();
        if (dc) { SelectObject(dc, oldBmp); DeleteDC(dc); }
        if (bmp) DeleteObject(bmp);
        if (g_primary == this) g_primary = NULL;
    }
    void Show() { if (primary) { GdiFlush(); Present(bits, pitch, w, h); } }

    void FillDesc(LPDDSURFACEDESC d, bool withSurface) {
        DWORD sz = d->dwSize;
        memset(d, 0, sz);
        d->dwSize = sz;
        d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        d->dwWidth = w; d->dwHeight = h; d->lPitch = pitch;
        if (withSurface) d->lpSurface = bits;
        d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d->ddpfPixelFormat.dwFlags = DDPF_RGB;
        d->ddpfPixelFormat.dwRGBBitCount = 16;
        d->ddpfPixelFormat.dwRBitMask = 0xF800; d->ddpfPixelFormat.dwGBitMask = 0x07E0; d->ddpfPixelFormat.dwBBitMask = 0x001F;
        d->ddsCaps.dwCaps = caps;
        if (hasCk) { d->dwFlags |= DDSD_CKSRCBLT; d->ddckCKSrcBlt = ck; }
        if (back) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = 1; }
    }

    // nearest-neighbour stretch copy with optional source color key; rects already validated
    void Copy(const RECT& d, Surface* s, const RECT& r, bool key, DDCOLORKEY k) {
        int dw = d.right - d.left, dh = d.bottom - d.top, sw = r.right - r.left, sh = r.bottom - r.top;
        if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
        bool same = (s == this);
        BYTE* tmp = NULL;
        const BYTE* sb = s->bits; int sp = s->pitch;
        if (same) { tmp = new BYTE[pitch * h]; memcpy(tmp, bits, pitch * h); sb = tmp; }
        for (int y = 0; y < dh; y++) {
            int dy = d.top + y; if (dy < 0 || dy >= h) continue;
            int sy = r.top + (int)((long long)y * sh / dh); if (sy < 0 || sy >= s->h) continue;
            WORD* dp = (WORD*)(bits + dy * pitch);
            const WORD* spx = (const WORD*)(sb + sy * sp);
            for (int x = 0; x < dw; x++) {
                int dx = d.left + x; if (dx < 0 || dx >= w) continue;
                int sx = r.left + (int)((long long)x * sw / dw); if (sx < 0 || sx >= s->w) continue;
                WORD c = spx[sx];
                if (key && c >= k.dwColorSpaceLowValue && c <= k.dwColorSpaceHighValue) continue;
                dp[dx] = c;
            }
        }
        delete[] tmp;
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    // IDirectDrawSurface
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) { return DDERR_UNSUPPORTED; }
    STDMETHOD(Blt)(LPRECT dr, LPDIRECTDRAWSURFACE src, LPRECT sr, DWORD flags, LPDDBLTFX fx) {
        RECT d = dr ? *dr : RECT{ 0, 0, w, h };
        if (flags & DDBLT_COLORFILL) {
            if (!fx) return DDERR_INVALIDPARAMS;
            WORD c = (WORD)fx->dwFillColor;
            for (int y = max(0L, d.top); y < min((LONG)h, d.bottom); y++) {
                WORD* p = (WORD*)(bits + y * pitch);
                for (int x = max(0L, d.left); x < min((LONG)w, d.right); x++) p[x] = c;
            }
        } else if (src) {
            Surface* s = (Surface*)src;
            RECT r = sr ? *sr : RECT{ 0, 0, s->w, s->h };
            bool key = false; DDCOLORKEY k = s->ck;
            if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { key = true; k = fx->ddckSrcColorkey; }
            else if ((flags & DDBLT_KEYSRC) && s->hasCk) key = true;
            Copy(d, s, r, key, k);
        }
        Show();
        return DD_OK;
    }
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) { return DDERR_UNSUPPORTED; }
    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT sr, DWORD trans) {
        if (!src) return DDERR_INVALIDPARAMS;
        Surface* s = (Surface*)src;
        RECT r = sr ? *sr : RECT{ 0, 0, s->w, s->h };
        RECT d = { (LONG)x, (LONG)y, (LONG)x + r.right - r.left, (LONG)y + r.bottom - r.top };
        Copy(d, s, r, (trans & DDBLTFAST_SRCCOLORKEY) && s->hasCk, s->ck);
        Show();
        return DD_OK;
    }
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
        if (back && cb) { DDSURFACEDESC sd = { sizeof sd }; back->FillDesc(&sd, false); back->AddRef(); cb(back, &sd, ctx); }
        return DD_OK;
    }
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE, DWORD) {
        if (!back) return DDERR_NOTFLIPPABLE;
        memcpy(bits, back->bits, pitch * h);
        Show();
        return DD_OK;
    }
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS, LPDIRECTDRAWSURFACE* out) {
        if (!out) return DDERR_INVALIDPARAMS;
        if (!back) { *out = NULL; return DDERR_NOTFOUND; }
        back->AddRef(); *out = back; return DD_OK;
    }
    STDMETHOD(GetBltStatus)(DWORD) { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDSCAPS c) { if (!c) return DDERR_INVALIDPARAMS; c->dwCaps = caps; return DD_OK; }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*) { return DDERR_NOCLIPPERATTACHED; }
    STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY k) {
        if (!k) return DDERR_INVALIDPARAMS;
        if (flags == DDCKEY_SRCBLT && hasCk) { *k = ck; return DD_OK; }
        return DDERR_NOCOLORKEY;
    }
    // Real DirectDraw hands out a fresh DC each time; keep text align / font / colors from leaking between calls
    int dcDepth = 0, dcSaved[8];
    STDMETHOD(GetDC)(HDC* out) {
        if (!out) return DDERR_INVALIDPARAMS;
        GdiFlush();
        if (dcDepth < 8) dcSaved[dcDepth] = SaveDC(dc);
        dcDepth++;
        *out = dc;
        return DD_OK;
    }
    STDMETHOD(GetFlipStatus)(DWORD) { return DD_OK; }
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*) { return DDERR_NOPALETTEATTACHED; }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) {
        if (!pf) return DDERR_INVALIDPARAMS;
        DDSURFACEDESC sd = { sizeof sd }; FillDesc(&sd, false); *pf = sd.ddpfPixelFormat; return DD_OK;
    }
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC d) {
        if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2))) return DDERR_INVALIDPARAMS;
        FillDesc(d, false); return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsLost)() { return DD_OK; }
    STDMETHOD(Lock)(LPRECT r, LPDDSURFACEDESC d, DWORD, HANDLE) {
        if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2))) return DDERR_INVALIDPARAMS;
        GdiFlush();
        FillDesc(d, true);
        if (r) d->lpSurface = bits + r->top * pitch + r->left * 2;
        return DD_OK;
    }
    STDMETHOD(ReleaseDC)(HDC) {
        GdiFlush();
        if (dcDepth > 0) { dcDepth--; if (dcDepth < 8 && dcSaved[dcDepth]) RestoreDC(dc, dcSaved[dcDepth]); }
        Show();
        return DD_OK;
    }
    STDMETHOD(Restore)() { return DD_OK; }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) { return DD_OK; }
    STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY k) {
        if (flags & DDCKEY_SRCBLT) { if (k) { ck = *k; if (!(flags & DDCKEY_COLORSPACE)) ck.dwColorSpaceHighValue = ck.dwColorSpaceLowValue; hasCk = true; } else hasCk = false; }
        return DD_OK;
    }
    STDMETHOD(SetOverlayPosition)(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) { return DDERR_UNSUPPORTED; }
    STDMETHOD(Unlock)(LPVOID) { Show(); return DD_OK; }
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(UpdateOverlayDisplay)(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_NOTAOVERLAYSURFACE; }
};

// ------------------------------------------------------------------ window subclass
static POINT MouseGame();
static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_modeSet) {
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) {
            POINT p = MouseGame();      // see mouseaccel.h
            l = MAKELPARAM(p.x, p.y);
        } else if (m == WM_PAINT) {
            PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps);
            if (g_primary) g_primary->Show();
            return 0;
        } else if (m == WM_ERASEBKGND) {
            return 1;
        } else if (m == WM_SIZE) {
            RECT c; GetClientRect(h, &c); ComputeDst(c.right, c.bottom);
            if (g_primary) g_primary->Show();
        }
    }
    return CallWindowProcA(g_oldProc, h, m, w, l);
}

// ------------------------------------------------------------------ IAT hooks in Dweep.exe (the window thinks the screen is 640x480)
static BOOL(WINAPI* R_GetCursorPos)(LPPOINT) = GetCursorPos;
static BOOL(WINAPI* R_SetCursorPos)(int, int) = SetCursorPos;
#include "../common/mouseaccel.h"

static bool Ours(HWND h) { return g_modeSet && h && h == g_hwnd; }

static BOOL WINAPI H_GetCursorPos(LPPOINT p) {
    BOOL ok = R_GetCursorPos(p);
    if (ok && g_modeSet && g_hwnd) *p = MouseGame();
    return ok;
}
static BOOL WINAPI H_SetCursorPos(int x, int y) {
    if (!g_modeSet || !g_hwnd) return R_SetCursorPos(x, y);
    MouseSetGame(x, y);
    return TRUE;
}
static DWORD WINAPI H_GetMessagePos() {
    DWORD v = GetMessagePos();
    if (!g_modeSet || !g_hwnd) return v;
    POINT p = MouseGame();
    return MAKELONG(p.x, p.y);
}
static int WINAPI H_GetSystemMetrics(int i) {
    if (g_modeSet) { if (i == SM_CXSCREEN) return g_mw; if (i == SM_CYSCREEN) return g_mh; }
    return GetSystemMetrics(i);
}
static BOOL WINAPI H_GetWindowRect(HWND h, LPRECT r) {
    if (Ours(h) && r) { *r = { 0, 0, g_mw, g_mh }; return TRUE; }
    return GetWindowRect(h, r);
}
static BOOL WINAPI H_GetClientRect(HWND h, LPRECT r) {
    if (Ours(h) && r) { *r = { 0, 0, g_mw, g_mh }; return TRUE; }
    return GetClientRect(h, r);
}
static BOOL WINAPI H_ClientToScreen(HWND h, LPPOINT p) { if (Ours(h)) return TRUE; return ClientToScreen(h, p); }
static BOOL WINAPI H_ScreenToClient(HWND h, LPPOINT p) { if (Ours(h)) return TRUE; return ScreenToClient(h, p); }
static BOOL WINAPI H_SetWindowPos(HWND h, HWND a, int x, int y, int cx, int cy, UINT f) {
    if (Ours(h)) return TRUE;
    return SetWindowPos(h, a, x, y, cx, cy, f);
}
static BOOL WINAPI H_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL rp) {
    if (Ours(h)) return TRUE;
    return MoveWindow(h, x, y, cx, cy, rp);
}
static BOOL WINAPI H_ClipCursor(const RECT* r) {
    if (!r || !g_modeSet || !g_hwnd) return ClipCursor(r);
    POINT a = { r->left, r->top }, b = { r->right, r->bottom };
    GameToClient(a.x, a.y); GameToClient(b.x, b.y);
    ClientToScreen(g_hwnd, &a); ClientToScreen(g_hwnd, &b);
    RECT q = { a.x, a.y, b.x, b.y };
    return ClipCursor(&q);
}

static void PatchIAT(HMODULE mod, const char* dll, const char* fn, void* repl) {
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char*)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, fn)) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD)repl;
            VirtualProtect(&iat->u1.Function, 4, old, &old);
        }
    }
}

static void InstallHooks() {
    HMODULE exe = GetModuleHandleA(NULL);
    PatchIAT(exe, "USER32.dll", "GetCursorPos", H_GetCursorPos);
    PatchIAT(exe, "USER32.dll", "SetCursorPos", H_SetCursorPos);
    PatchIAT(exe, "USER32.dll", "GetMessagePos", H_GetMessagePos);
    PatchIAT(exe, "USER32.dll", "GetSystemMetrics", H_GetSystemMetrics);
    PatchIAT(exe, "USER32.dll", "GetWindowRect", H_GetWindowRect);
    PatchIAT(exe, "USER32.dll", "GetClientRect", H_GetClientRect);
    PatchIAT(exe, "USER32.dll", "ClientToScreen", H_ClientToScreen);
    PatchIAT(exe, "USER32.dll", "ScreenToClient", H_ScreenToClient);
    PatchIAT(exe, "USER32.dll", "SetWindowPos", H_SetWindowPos);
    PatchIAT(exe, "USER32.dll", "MoveWindow", H_MoveWindow);
    PatchIAT(exe, "USER32.dll", "ClipCursor", H_ClipCursor);
}

// ------------------------------------------------------------------ DirectDraw object
struct DDraw : IDirectDraw {
    LONG ref = 1;

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectDraw) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    STDMETHOD(Compact)() { return DD_OK; }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) { return DDERR_UNSUPPORTED; }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) { return DDERR_UNSUPPORTED; }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) {
        if (!d || !out) return DDERR_INVALIDPARAMS;
        DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
        Surface* s;
        if (caps & DDSCAPS_PRIMARYSURFACE) {
            s = new Surface(g_mw, g_mh, CAPS_PRIMARY | (caps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX)));
            if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount)
                s->back = new Surface(g_mw, g_mh, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
            g_primary = s;
        } else {
            if (!(d->dwFlags & DDSD_WIDTH) || !(d->dwFlags & DDSD_HEIGHT) || !d->dwWidth || !d->dwHeight) return DDERR_INVALIDPARAMS;
            if (!(caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) caps |= DDSCAPS_SYSTEMMEMORY;
            s = new Surface(d->dwWidth, d->dwHeight, caps);
        }
        if (!s->bits) { Log("CreateDIBSection failed %dx%d", s->w, s->h); s->Release(); *out = NULL; return DDERR_OUTOFMEMORY; }
        if (d->dwFlags & DDSD_CKSRCBLT) { s->ck = d->ddckCKSrcBlt; s->hasCk = true; }
        *out = s;
        return DD_OK;
    }
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) { return DDERR_UNSUPPORTED; }
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
        if (!cb) return DDERR_INVALIDPARAMS;
        DDSURFACEDESC sd; ModeDesc(&sd, 640, 480); cb(&sd, ctx);
        return DD_OK;
    }
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }
    STDMETHOD(FlipToGDISurface)() { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDCAPS hal, LPDDCAPS hel) {
        LPDDCAPS both[2] = { hal, hel };
        for (int i = 0; i < 2; i++) {
            LPDDCAPS c = both[i];
            if (!c) continue;
            DWORD sz = c->dwSize ? c->dwSize : sizeof(DDCAPS_DX1);
            memset(c, 0, sz); c->dwSize = sz;
            c->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY;
            c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
            c->dwVidMemTotal = 64 << 20; c->dwVidMemFree = 64 << 20;
            c->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_FLIP;
        }
        return DD_OK;
    }
    static void ModeDesc(LPDDSURFACEDESC sd, int w, int h) {
        memset(sd, 0, sizeof *sd); sd->dwSize = sizeof *sd;
        sd->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        sd->dwWidth = w; sd->dwHeight = h; sd->lPitch = w * 2; sd->dwRefreshRate = 60;
        sd->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT); sd->ddpfPixelFormat.dwFlags = DDPF_RGB;
        sd->ddpfPixelFormat.dwRGBBitCount = 16;
        sd->ddpfPixelFormat.dwRBitMask = 0xF800; sd->ddpfPixelFormat.dwGBitMask = 0x07E0; sd->ddpfPixelFormat.dwBBitMask = 0x001F;
    }
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC d) {
        if (!d) return DDERR_INVALIDPARAMS;
        DWORD sz = d->dwSize; DDSURFACEDESC sd; ModeDesc(&sd, g_mw, g_mh);
        memcpy(d, &sd, min(sz ? sz : sizeof sd, (DWORD)sizeof sd)); d->dwSize = sz ? sz : sizeof sd;
        return DD_OK;
    }
    STDMETHOD(GetFourCCCodes)(LPDWORD n, LPDWORD) { if (n) *n = 0; return DD_OK; }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE* out) {
        if (!out) return DDERR_INVALIDPARAMS;
        if (!g_primary) return DDERR_NOTFOUND;
        g_primary->AddRef(); *out = g_primary; return DD_OK;
    }
    STDMETHOD(GetMonitorFrequency)(LPDWORD f) { if (f) *f = 60; return DD_OK; }
    STDMETHOD(GetScanLine)(LPDWORD l) { if (l) *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL b) { if (b) *b = TRUE; return DD_OK; }
    STDMETHOD(Initialize)(GUID*) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(RestoreDisplayMode)() { return DD_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND h, DWORD flags) {
        if (h && h != g_hwnd) {
            if (g_hwnd && g_oldProc) SetWindowLongA(g_hwnd, GWL_WNDPROC, (LONG)g_oldProc);
            DropDevice();
            g_hwnd = h;
            g_oldProc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)SubProc);
        }
        Log("SetCooperativeLevel hwnd=%p flags=%X", h, flags);
        return DD_OK;
    }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) {
        Log("SetDisplayMode %ux%ux%u (emulated; mode=%s filter=%d)", w, h, bpp, g_cfgWindow ? "window" : "fullscreen", g_cfgFilter);
        if (bpp != 16) return DDERR_UNSUPPORTEDMODE;
        g_mw = w; g_mh = h;
        PlaceWindow();
        g_modeSet = true;
        return DD_OK;
    }
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) { return DD_OK; }
};

// ------------------------------------------------------------------ exports
extern "C" HRESULT WINAPI Proxy_DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new DDraw();
    return DD_OK;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    if (cb) cb(NULL, (LPSTR)"Direct2D", (LPSTR)"display", ctx);
    return DD_OK;
}

#include "../common/regsave.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        SetProcessDPIAware();
        LoadConfig();
        { char ini[MAX_PATH]; sprintf(ini, "%sDweepD2D.ini", g_dir); MouseAccel_LoadConfig(ini); }
        InstallHooks();
        RegSave_Install(g_dir, PatchIAT, Log);
    }
    return TRUE;
}
