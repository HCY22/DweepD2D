// Software DirectDraw 1 for Dweep Gold. This is the CPU reference image: the game's original blitters keep
// drawing into these surfaces, while gfx.cpp mirrors every drawing call on the GPU at a higher resolution.
#include "dwgfx.h"

// ------------------------------------------------------------------ config + log
int g_cfgWindow = 0, g_cfgScale = 2, g_cfgFilter = 0, g_cfgVsync = 0, g_cfgStretch = 0;
int g_cfgRenderScale = 0, g_cfgVerify = 0, g_cfgGpu = 1;
char g_dir[MAX_PATH];

void Log(const char* f, ...) {
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
    GetPrivateProfileStringA("Render", "Scale", "auto", v, 64, ini);
    g_cfgRenderScale = atoi(v);
    if (g_cfgRenderScale < 0 || g_cfgRenderScale > 8) g_cfgRenderScale = 0;
    g_cfgVerify = GetPrivateProfileIntA("Render", "Verify", 0, ini);
    g_cfgGpu = GetPrivateProfileIntA("Render", "Gpu", 1, ini);
}

// ------------------------------------------------------------------ window state
HWND g_hwnd;
static WNDPROC g_oldProc;
bool g_modeSet;
int g_mw = 640, g_mh = 480;
RECT g_dst = { 0, 0, 640, 480 };
Surface* g_primary;

void ComputeDst(int cw, int ch) {
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

// ------------------------------------------------------------------ surfaces
static const DWORD CAPS_PRIMARY = DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;

Surface::Surface(int w_, int h_, DWORD caps_) : w(w_), h(h_), caps(caps_), primary(!!(caps_ & DDSCAPS_PRIMARYSURFACE)) {
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
Surface::~Surface() {
    if (back) back->Release();
    if (dc) { SelectObject(dc, oldBmp); DeleteDC(dc); }
    if (bmp) DeleteObject(bmp);
    if (g_primary == this) g_primary = NULL;
}
void Surface::Show() { if (primary) { GdiFlush(); Gfx_Present(this); } }
void Surface::Touched() { Gfx_SurfaceTouched(this); }

void Surface::FillDesc(LPDDSURFACEDESC d, bool withSurface) {
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

void Surface::Copy(const RECT& d, Surface* s, const RECT& r, bool key, DDCOLORKEY k) {
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

HRESULT Surface::QueryInterface(REFIID riid, LPVOID* out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface) { AddRef(); *out = this; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
ULONG Surface::AddRef() { return InterlockedIncrement(&ref); }
ULONG Surface::Release() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
HRESULT Surface::AddAttachedSurface(LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
HRESULT Surface::AddOverlayDirtyRect(LPRECT) { return DDERR_UNSUPPORTED; }
HRESULT Surface::Blt(LPRECT dr, LPDIRECTDRAWSURFACE src, LPRECT sr, DWORD flags, LPDDBLTFX fx) {
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
    Touched();
    Show();
    return DD_OK;
}
HRESULT Surface::BltBatch(LPDDBLTBATCH, DWORD, DWORD) { return DDERR_UNSUPPORTED; }
HRESULT Surface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT sr, DWORD trans) {
    if (!src) return DDERR_INVALIDPARAMS;
    Surface* s = (Surface*)src;
    RECT r = sr ? *sr : RECT{ 0, 0, s->w, s->h };
    RECT d = { (LONG)x, (LONG)y, (LONG)x + r.right - r.left, (LONG)y + r.bottom - r.top };
    Copy(d, s, r, (trans & DDBLTFAST_SRCCOLORKEY) && s->hasCk, s->ck);
    Touched();
    Show();
    return DD_OK;
}
HRESULT Surface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
HRESULT Surface::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    if (back && cb) { DDSURFACEDESC sd = { sizeof sd }; back->FillDesc(&sd, false); back->AddRef(); cb(back, &sd, ctx); }
    return DD_OK;
}
HRESULT Surface::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }
HRESULT Surface::Flip(LPDIRECTDRAWSURFACE, DWORD) {
    if (!back) return DDERR_NOTFLIPPABLE;
    memcpy(bits, back->bits, pitch * h);
    Touched();
    Show();
    return DD_OK;
}
HRESULT Surface::GetAttachedSurface(LPDDSCAPS, LPDIRECTDRAWSURFACE* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!back) { *out = NULL; return DDERR_NOTFOUND; }
    back->AddRef(); *out = back; return DD_OK;
}
HRESULT Surface::GetBltStatus(DWORD) { return DD_OK; }
HRESULT Surface::GetCaps(LPDDSCAPS c) { if (!c) return DDERR_INVALIDPARAMS; c->dwCaps = caps; return DD_OK; }
HRESULT Surface::GetClipper(LPDIRECTDRAWCLIPPER*) { return DDERR_NOCLIPPERATTACHED; }
HRESULT Surface::GetColorKey(DWORD flags, LPDDCOLORKEY k) {
    if (!k) return DDERR_INVALIDPARAMS;
    if (flags == DDCKEY_SRCBLT && hasCk) { *k = ck; return DD_OK; }
    return DDERR_NOCOLORKEY;
}
// Real DirectDraw hands out a fresh DC each time; keep text align / font / colors from leaking between calls
HRESULT Surface::GetDC(HDC* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    GdiFlush();
    if (dcDepth < 8) dcSaved[dcDepth] = SaveDC(dc);
    dcDepth++;
    *out = dc;
    return DD_OK;
}
HRESULT Surface::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT Surface::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::GetPalette(LPDIRECTDRAWPALETTE*) { return DDERR_NOPALETTEATTACHED; }
HRESULT Surface::GetPixelFormat(LPDDPIXELFORMAT pf) {
    if (!pf) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC sd = { sizeof sd }; FillDesc(&sd, false); *pf = sd.ddpfPixelFormat; return DD_OK;
}
HRESULT Surface::GetSurfaceDesc(LPDDSURFACEDESC d) {
    if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2))) return DDERR_INVALIDPARAMS;
    FillDesc(d, false); return DD_OK;
}
HRESULT Surface::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) { return DDERR_ALREADYINITIALIZED; }
HRESULT Surface::IsLost() { return DD_OK; }
HRESULT Surface::Lock(LPRECT r, LPDDSURFACEDESC d, DWORD, HANDLE) {
    if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2))) return DDERR_INVALIDPARAMS;
    GdiFlush();
    FillDesc(d, true);
    if (r) d->lpSurface = bits + r->top * pitch + r->left * 2;
    return DD_OK;
}
HRESULT Surface::ReleaseDC(HDC) {
    GdiFlush();
    if (dcDepth > 0) { dcDepth--; if (dcDepth < 8 && dcSaved[dcDepth]) RestoreDC(dc, dcSaved[dcDepth]); }
    if (primary) Touched();     // GDI on the back buffer is mirrored by the text hooks; on the primary it is not
    Show();
    return DD_OK;
}
HRESULT Surface::Restore() { return DD_OK; }
HRESULT Surface::SetClipper(LPDIRECTDRAWCLIPPER) { return DD_OK; }
HRESULT Surface::SetColorKey(DWORD flags, LPDDCOLORKEY k) {
    if (flags & DDCKEY_SRCBLT) { if (k) { ck = *k; if (!(flags & DDCKEY_COLORSPACE)) ck.dwColorSpaceHighValue = ck.dwColorSpaceLowValue; hasCk = true; } else hasCk = false; }
    return DD_OK;
}
HRESULT Surface::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::SetPalette(LPDIRECTDRAWPALETTE) { return DDERR_UNSUPPORTED; }
HRESULT Surface::Unlock(LPVOID) { Show(); return DD_OK; }
HRESULT Surface::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_NOTAOVERLAYSURFACE; }

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
            Gfx_Resize();
            if (g_primary) g_primary->Show();
        } else if (m == WM_KEYDOWN && (w == VK_F11 || w == VK_F12)) {
            Gfx_Key(w);
            return 0;
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

void PatchIAT(HMODULE mod, const char* dll, const char* fn, void* repl) {
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
            HWND old = g_hwnd;
            if (g_hwnd && g_oldProc) SetWindowLongA(g_hwnd, GWL_WNDPROC, (LONG)g_oldProc);
            g_hwnd = h;
            g_oldProc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)SubProc);
            Gfx_WindowChanged(old);
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
        Gfx_Init();
    }
    return TRUE;
}
