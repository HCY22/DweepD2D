// GPU drawing layer for Dweep Gold.
//
// Every blitter of the game (CImageManager, 0x44f9a8) is detoured and drawn with Direct2D on GPU bitmaps that are
// S times larger; GDI text on the back buffer is drawn with DirectWrite. The window shows the GPU image.
// Normally the original CPU blitters no longer run for the screen images (only for the background, drawn once per
// level, and for asset images); side effects of the originals (dirty-rect list entries) are reproduced.
// With Verify the originals run first and their 640x480 RGB565 result is the reference image.
//
//   native   : CopyRect, Blend50, Tile, sprites (RLE16 / 8-bit / 50% / alpha), Rect, RectAlpha, dirty-rect copies, text
//              bevels, panel, gray, red tint, floor, lasers (all reimplemented with fills, additive blending and effects)
//   mirrored : only GDI text in verify mode: the CPU result is diffed against a snapshot and changed pixels uploaded.
//
// [Render] Verify=1 compares the whole frame with the CPU image, Verify=2 also checks every native primitive.
// F11 toggles the CPU image on screen, F12 writes both images to DweepD2D_cpu.bmp / DweepD2D_gpu.bmp.
#include "dwgfx.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <d2d1effects.h>
#include <mmsystem.h>
#include <vector>
#include <string>
#include <unordered_map>

// ------------------------------------------------------------------ game memory (Dweep.exe 32-bit, image base 0x400000)
#define IMG(k) ((BYTE*)0x44f9a8 + (k) * 0x38)
static inline WORD* ImgBits(int k) { return *(WORD**)(IMG(k) + 8); }
static inline BYTE* ImgRle8(int k) { return *(BYTE**)(IMG(k) + 0xc); }
static inline WORD* ImgPal(int k) { return *(WORD**)(IMG(k) + 0x10); }
static inline int ImgW(int k) { return *(int*)(IMG(k) + 0x1c); }
static inline int ImgPitch(int k) { return *(int*)(IMG(k) + 0x20); }
static inline int ImgH(int k) { return *(int*)(IMG(k) + 0x24); }
static inline Surface* ImgSurf(int k) {
    Surface* s = *(Surface**)(IMG(k) + 0x28);
    if (!s) s = *(Surface**)(k == 0 ? 0x44a2a0 : 0x44a2a4);
    return s;
}
static inline int* FrameDesc(int k, int g, int f) {      // {offset, w, h, hotX, hotY}
    BYTE** groups = *(BYTE***)(IMG(k) + 0x34);
    if (!groups || !groups[g]) return NULL;
    return (int*)(groups[g] + f * 0x14);
}
static const int NIMG = 43, NSCR = 5;    // 0 primary, 1 back, 2 background, 3 cursor save, 4 popup save

struct CpuImg { WORD* p; int pitch, w, h; };
static bool CpuOf(int k, CpuImg& c) {
    if (k < 0 || k >= NIMG) return false;
    if (k < 2) {
        Surface* s = ImgSurf(k);
        if (!s || !s->bits) return false;
        c = { (WORD*)s->bits, s->pitch / 2, s->w, s->h };
        return true;
    }
    WORD* b = ImgBits(k);
    if (!b) return false;
    c = { b, ImgPitch(k), ImgW(k), ImgH(k) };
    return c.w > 0 && c.h > 0 && c.pitch >= c.w;
}

// ------------------------------------------------------------------ GPU state
static ID3D11Device* g_d3d;
static ID2D1Factory1* g_f;
static ID2D1Device* g_dev;
static ID2D1DeviceContext* g_dc;
static IDWriteFactory* g_dw;
static IDXGISwapChain1* g_sc;
static ID2D1Bitmap1* g_scBmp;
static ID2D1SolidColorBrush* g_brush;
static ID2D1Bitmap1* g_stage;      // 640x480 upload staging
static ID2D1Bitmap1* g_read;       // CPU-readable copy for verification / screenshots
static ID2D1Bitmap1* g_tmp;        // for overlapping screen-to-screen copies and the gray effect
static ID2D1Effect* g_fxBright;
static ID2D1Effect* g_fxGray;
static ID2D1Bitmap1* g_laserH;     // 512x3: premultiplied green, alpha from the three laser tables
static ID2D1Bitmap1* g_laserV;     // 3x512: same, transposed
static UINT32 g_laserHash;
static bool g_devFailed, g_hooked, g_showCpu;
static int S = 1;
static UINT32 g_L[65536];          // RGB565 -> opaque BGRA

struct GImg { ID2D1Bitmap1* b; int w, h; };
static GImg G[NSCR];
struct Asset { ID2D1Bitmap1* b; ID2D1ImageBrush* brush; const WORD* bits; int w, h; };
static Asset A[NIMG];
struct FKey { int img, g, f, mode; UINT32 pal; bool operator==(const FKey& o) const { return img == o.img && g == o.g && f == o.f && mode == o.mode && pal == o.pal; } };
struct FKeyH { size_t operator()(const FKey& k) const { return k.img * 73856093u ^ k.g * 19349663u ^ k.f * 83492791u ^ k.mode * 2654435761u ^ k.pal; } };
struct FBmp { ID2D1Bitmap1* color; ID2D1Bitmap1* shadow; int w, h; };
static std::unordered_map<FKey, FBmp, FKeyH> g_frames;
struct TFmt { IDWriteTextFormat* fmt; };
static std::unordered_map<std::string, TFmt> g_fonts;

static bool g_ref;                 // CPU reference image kept up to date (only in verify mode)
static int g_depth;                // >0 while an original blitter runs inside a mirrored call
static bool g_open;                // BeginDraw active
static ID2D1Image* g_target;

// ------------------------------------------------------------------ statistics
enum Op { OP_COPY, OP_BLEND, OP_TILE, OP_BEVBTN, OP_BEVFR, OP_FROUT, OP_FRBLEND, OP_SPR16, OP_SPR8, OP_SPR8H, OP_SPR8A,
          OP_SPR8R, OP_RECT, OP_RECTA, OP_GRAY, OP_PANEL, OP_FLOOR, OP_LASER, OP_LASER2, OP_FLUSH, OP_ERASE, OP_TEXT,
          OP_AA, OP_N };
static const char* OPN[OP_N] = { "CopyRect", "Blend50", "Tile", "BevelBtn", "BevelFrame", "FrameOutline", "FrameBlend",
    "SpriteRLE16", "Sprite8", "Sprite8Half", "Sprite8Alpha", "Sprite8Red", "Rect", "RectAlpha", "GrayRect", "Panel",
    "Floor", "Laser", "Laser2", "DirtyFlush", "DirtyErase", "Text", "TextAA" };
static unsigned g_calls[OP_N], g_bad[OP_N], g_logged[OP_N];

// ------------------------------------------------------------------ device
static void ReleaseAll() {
    if (g_dc) { if (g_open) g_dc->EndDraw(); g_dc->SetTarget(NULL); }
    g_open = false; g_target = NULL;
    for (auto& g : G) SafeRelease(g.b);
    for (auto& a : A) { SafeRelease(a.brush); SafeRelease(a.b); }
    for (auto& f : g_frames) { SafeRelease(f.second.color); SafeRelease(f.second.shadow); }
    g_frames.clear();
    for (auto& t : g_fonts) SafeRelease(t.second.fmt);
    g_fonts.clear();
    SafeRelease(g_stage); SafeRelease(g_read); SafeRelease(g_tmp); SafeRelease(g_brush);
    SafeRelease(g_fxBright); SafeRelease(g_fxGray); SafeRelease(g_laserH); SafeRelease(g_laserV);
    SafeRelease(g_scBmp); SafeRelease(g_sc);
    SafeRelease(g_dc); SafeRelease(g_dev); SafeRelease(g_d3d);
}

static ID2D1Bitmap1* NewBmp(int w, int h, D2D1_BITMAP_OPTIONS o, const void* data = NULL, int pitch = 0) {
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(o, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* b = NULL;
    HRESULT hr = g_dc->CreateBitmap(D2D1::SizeU(w, h), data, pitch, &p, &b);
    if (FAILED(hr)) Log("CreateBitmap %dx%d opt %X failed %08X", w, h, o, hr);
    return b;
}

static bool EnsureDevice() {
    if (g_dc) return true;
    if (g_devFailed) return false;
    for (int i = 0; i < 65536; i++) {
        int r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        g_L[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    if (!S || S == 1) {
        if (g_cfgVerify) S = 1;
        else if (g_cfgRenderScale) S = g_cfgRenderScale;
        else {
            MONITORINFO mi = { sizeof mi };
            GetMonitorInfoA(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
            int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
            S = max((mw + 639) / 640, (mh + 479) / 480);
            S = max(1, min(6, S));
        }
    }
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_1 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 5, D3D11_SDK_VERSION, &g_d3d, NULL, NULL);
    if (FAILED(hr)) {
        Log("hardware D3D11 device failed %08X, using WARP", hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 5, D3D11_SDK_VERSION, &g_d3d, NULL, NULL);
    }
    if (FAILED(hr)) { Log("D3D11CreateDevice failed %08X", hr); g_devFailed = true; return false; }
    if (!g_f) {
        D2D1_FACTORY_OPTIONS fo = {};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&g_f);
        if (FAILED(hr)) { Log("D2D1CreateFactory failed %08X", hr); g_devFailed = true; return false; }
    }
    if (!g_dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    IDXGIDevice* dx = NULL;
    g_d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx);
    hr = dx ? g_f->CreateDevice(dx, &g_dev) : E_NOINTERFACE;
    SafeRelease(dx);
    if (SUCCEEDED(hr)) hr = g_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc);
    if (FAILED(hr)) { Log("D2D device failed %08X", hr); ReleaseAll(); g_devFailed = true; return false; }
    g_dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    g_dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_brush);
    g_stage = NewBmp(640, 480, D2D1_BITMAP_OPTIONS_NONE);
    Log("GPU renderer ready: scale %d (%dx%d), verify %d", S, 640 * S, 480 * S, g_cfgVerify);
    return g_brush && g_stage;
}

static bool g_lost, g_rebuild;
static void DeviceLost() {      // handled at the next present, never in the middle of a drawing call
    if (!g_lost) Log("GPU device lost, rebuilding from the CPU image");
    g_lost = true;
}

static void Close() {
    if (!g_open) return;
    g_open = false;
    HRESULT hr = g_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DeviceLost();
    else if (FAILED(hr)) Log("EndDraw failed %08X", hr);
}

static void Target(ID2D1Image* t) {
    if (g_open && g_target == t) return;
    Close();
    if (!g_dc) return;
    if (g_target != t) { g_dc->SetTarget(t); g_target = t; }
    g_dc->BeginDraw();
    g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g_open = true;
}

static inline D2D1_RECT_F SR(int l, int t, int r, int b) { return D2D1::RectF((float)l * S, (float)t * S, (float)r * S, (float)b * S); }
static inline D2D1_COLOR_F C565(WORD v) {
    UINT32 c = g_L[v];
    return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1.f);
}
static inline bool HasClip(const RECT* c) { return c && (c->left || c->top || c->right || c->bottom); }
struct ClipScope {       // game clip rects are inclusive; all-zero means no clip
    bool on;
    ClipScope(const RECT* c) : on(HasClip(c)) {
        if (on) g_dc->PushAxisAlignedClip(SR(c->left, c->top, c->right + 1, c->bottom + 1), D2D1_ANTIALIAS_MODE_ALIASED);
    }
    ~ClipScope() { if (on) g_dc->PopAxisAlignedClip(); }
};

// ------------------------------------------------------------------ screen images
static void UploadRect(int k, RECT r, const WORD* before, int bpitch);

static ID2D1Bitmap1* Scr(int k) {
    if (k < 0 || k >= NSCR || !EnsureDevice()) return NULL;
    CpuImg c;
    if (!CpuOf(k, c)) return NULL;
    if (G[k].b && G[k].w == c.w && G[k].h == c.h) return G[k].b;
    Close();
    if (g_target == G[k].b && g_target) { g_dc->SetTarget(NULL); g_target = NULL; }
    SafeRelease(G[k].b);
    G[k].b = NewBmp(c.w * S, c.h * S, D2D1_BITMAP_OPTIONS_TARGET);
    if (!G[k].b) return NULL;
    G[k].w = c.w; G[k].h = c.h;
    UploadRect(k, RECT{ 0, 0, c.w, c.h }, NULL, 0);
    return G[k].b;
}

// copy CPU pixels of image k inside r (exclusive) to the GPU image; with a snapshot only the changed pixels go up
static std::vector<UINT32> g_up;
static void UploadRect(int k, RECT r, const WORD* before, int bpitch) {
    CpuImg c;
    if (!CpuOf(k, c) || !G[k].b) return;
    RECT img = { 0, 0, min(c.w, G[k].w), min(c.h, G[k].h) };
    RECT o;
    int ox = r.left, oy = r.top;
    if (!IntersectRect(&o, &r, &img)) return;
    int w = o.right - o.left, h = o.bottom - o.top;
    if (w > 640 || h > 480) return;
    g_up.resize(w * h);
    bool any = false;
    for (int y = 0; y < h; y++) {
        const WORD* s = c.p + (o.top + y) * c.pitch + o.left;
        const WORD* b = before ? before + (o.top - oy + y) * bpitch + (o.left - ox) : NULL;
        UINT32* d = &g_up[y * w];
        for (int x = 0; x < w; x++) {
            if (b && b[x] == s[x]) d[x] = 0;
            else { d[x] = g_L[s[x]]; any = true; }
        }
    }
    if (!any) return;
    Close();
    D2D1_RECT_U ur = D2D1::RectU(0, 0, w, h);
    g_stage->CopyFromMemory(&ur, g_up.data(), w * 4);
    Target(G[k].b);
    D2D1_RECT_F src = D2D1::RectF(0, 0, (float)w, (float)h);
    D2D1_RECT_F dst = SR(o.left, o.top, o.right, o.bottom);
    g_dc->DrawBitmap(g_stage, &dst, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src, NULL);
}

// snapshot of a screen image region before an original blitter runs
struct Snap { int k = -1; RECT r; std::vector<WORD> px; };
static bool Grab(Snap& s, int k, RECT r) {
    s.k = -1;
    CpuImg c;
    if (k < 0 || k >= NSCR || !Scr(k) || !CpuOf(k, c)) return false;
    RECT img = { 0, 0, c.w, c.h };
    if (!IntersectRect(&s.r, &r, &img)) return false;
    int w = s.r.right - s.r.left, h = s.r.bottom - s.r.top;
    s.px.resize(w * h);
    for (int y = 0; y < h; y++) memcpy(&s.px[y * w], c.p + (s.r.top + y) * c.pitch + s.r.left, w * 2);
    s.k = k;
    return true;
}
static void Put(Snap& s, bool force) {
    if (s.k < 0) return;
    UploadRect(s.k, s.r, force ? NULL : s.px.data(), s.r.right - s.r.left);
}

// ------------------------------------------------------------------ assets (raw 16-bit images and sprite frames)
static void InvalidateAsset(int k) {
    if (k >= NSCR && k < NIMG) { Close(); SafeRelease(A[k].brush); SafeRelease(A[k].b); }
}
static ID2D1Bitmap1* AssetBmp(int k) {
    if (k < NSCR || k >= NIMG) return NULL;
    CpuImg c;
    if (!CpuOf(k, c)) return NULL;
    Asset& a = A[k];
    // frame sheets (Icons, HeatTile, IceTile...) store one frame's size in w/h; the data holds all frames stacked
    int bytes = *(int*)(IMG(k) + 0x18);
    if (bytes / 2 / c.pitch > c.h) c.h = bytes / 2 / c.pitch;
    if (a.b && a.bits == c.p && a.w == c.w && a.h == c.h) return a.b;
    Close();
    SafeRelease(a.brush); SafeRelease(a.b);
    std::vector<UINT32> px(c.w * c.h);
    for (int y = 0; y < c.h; y++) for (int x = 0; x < c.w; x++) px[y * c.w + x] = g_L[c.p[y * c.pitch + x]];
    a.b = NewBmp(c.w, c.h, D2D1_BITMAP_OPTIONS_NONE, px.data(), c.w * 4);
    a.bits = c.p; a.w = c.w; a.h = c.h;
    return a.b;
}
static ID2D1Bitmap* SourceBmp(int k, float& scale) {
    if (k >= 0 && k < NSCR) { scale = (float)S; return Scr(k); }
    scale = 1.f;
    return AssetBmp(k);
}
static bool DstScreen(int dst) {
    if (dst >= 0 && dst < NSCR) return Scr(dst) != NULL;
    InvalidateAsset(dst);
    return false;
}

static UINT32 PalHash(const WORD* p) {
    UINT32 h = 2166136261u;
    for (int i = 0; i < 256; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}

// decode one sprite frame. rle16: rows of {u32 len16, cmds}; cmd = count | type<<16, type 2 = literal pixels.
// 8-bit: rows of {u32 lenBytes, cmds}; type 1 skip, 2 literal palette indices, anything else = shadow (darken 50%).
enum { FR_RLE16, FR_PAL8, FR_TINT };
static FBmp* Frame(int img, int g, int f, const WORD* pal, int mode) {
    if (img < 0 || img >= NIMG || (mode == FR_PAL8 && !pal)) return NULL;
    FKey key = { img, g, f, mode, mode == FR_PAL8 ? PalHash(pal) : 0 };
    auto it = g_frames.find(key);
    if (it != g_frames.end()) return &it->second;
    int* d = FrameDesc(img, g, f);
    if (!d) return NULL;
    int w = d[1], h = d[2];
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    if (g_frames.size() > 4000) {
        Close();
        for (auto& e : g_frames) { SafeRelease(e.second.color); SafeRelease(e.second.shadow); }
        g_frames.clear();
    }
    std::vector<UINT32> col(w * h, 0), sh;
    bool anyC = false, anyS = false;
    if (mode == FR_RLE16) {
        BYTE* row = (BYTE*)(ImgBits(img) + d[0]);
        for (int y = 0; y < h; y++) {
            int len = *(DWORD*)row & 0xffff;
            BYTE* end = row + len * 2;
            BYTE* p = row + 4;
            for (int x = 0; x < w && p + 4 <= end;) {
                DWORD cmd = *(DWORD*)p; p += 4;
                int n = cmd & 0xffff, t = cmd >> 16;
                if (t == 2) {
                    WORD* px = (WORD*)p;
                    for (int i = 0; i < n && x + i < w; i++) col[y * w + x + i] = g_L[px[i]];
                    p += n * 2; anyC = true;
                }
                x += n;
            }
            if (len <= 0) break;
            row = end;
        }
    } else {
        sh.assign(w * h, 0);
        BYTE* row = ImgRle8(img) + d[0];
        for (int y = 0; y < h; y++) {
            int len = *(DWORD*)row & 0xffff;
            BYTE* end = row + len;
            BYTE* p = row + 4;
            for (int x = 0; x < w && p + 4 <= end;) {
                DWORD cmd = *(DWORD*)p; p += 4;
                int n = cmd & 0xffff, t = cmd >> 16;
                if (t == 2) {
                    // FR_TINT: red +16 (premultiplied, drawn with additive blending) under every literal pixel
                    for (int i = 0; i < n && x + i < w; i++) col[y * w + x + i] = mode == FR_TINT ? 0xFF100000u : g_L[pal[p[i]]];
                    p += n; anyC = true;
                } else if (t != 1 && mode == FR_PAL8) {
                    for (int i = 0; i < n && x + i < w; i++) sh[y * w + x + i] = 0x80000000u;   // black, alpha 0.5
                    anyS = true;
                }
                x += n;
            }
            if (len <= 0) break;
            row = end;
        }
    }
    FBmp fb = { NULL, NULL, w, h };
    if (anyC) fb.color = NewBmp(w, h, D2D1_BITMAP_OPTIONS_NONE, col.data(), w * 4);
    if (anyS) fb.shadow = NewBmp(w, h, D2D1_BITMAP_OPTIONS_NONE, sh.data(), w * 4);
    return &(g_frames[key] = fb);
}

// ------------------------------------------------------------------ verification
static int g_tol = 16;
static bool EnsureRead() {
    if (g_read) return true;
    g_read = NewBmp(640 * S, 480 * S, (D2D1_BITMAP_OPTIONS)(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW));
    return g_read != NULL;
}

// compare GPU image k inside r with the CPU image; returns mismatching pixel count (S must be 1)
static int Compare(int k, RECT r, int* maxd, POINT* first, UINT32* gpx, UINT32* cpx) {
    CpuImg c;
    if (S != 1 || !G[k].b || !CpuOf(k, c) || !EnsureRead()) return 0;
    RECT img = { 0, 0, min(c.w, G[k].w), min(c.h, G[k].h) }, o;
    if (!IntersectRect(&o, &r, &img)) return 0;
    Close();
    D2D1_POINT_2U p = { 0, 0 };
    D2D1_RECT_U sr = D2D1::RectU(o.left, o.top, o.right, o.bottom);
    if (FAILED(g_read->CopyFromBitmap(&p, G[k].b, &sr))) return 0;
    D2D1_MAPPED_RECT m;
    if (FAILED(g_read->Map(D2D1_MAP_OPTIONS_READ, &m))) return 0;
    int bad = 0; *maxd = 0;
    for (int y = 0; y < o.bottom - o.top; y++) {
        const UINT32* gp = (const UINT32*)(m.bits + y * m.pitch);
        const WORD* cp = c.p + (o.top + y) * c.pitch + o.left;
        for (int x = 0; x < o.right - o.left; x++) {
            UINT32 a = gp[x], b = g_L[cp[x]];
            int d = 0;
            for (int s = 0; s < 24; s += 8) d = max(d, abs((int)((a >> s) & 255) - (int)((b >> s) & 255)));
            if (d > g_tol) {
                if (!bad) { *first = { o.left + x, o.top + y }; *gpx = a & 0xffffff; *cpx = b & 0xffffff; }
                bad++;
            }
            *maxd = max(*maxd, d);
        }
    }
    g_read->Unmap();
    return bad;
}

static void VerifyOp(Op op, int k, RECT r, const char* args) {
    if (g_cfgVerify < 2 || k < 0 || k >= NSCR) return;
    int md; POINT fp; UINT32 gp, cp;
    int bad = Compare(k, r, &md, &fp, &gp, &cp);
    if (!bad) return;
    g_bad[op]++;
    if (g_logged[op]++ < 40)
        Log("VERIFY %s(%s) img%d rect(%d,%d)-(%d,%d): %d px differ, max %d, first (%d,%d) gpu %06X cpu %06X",
            OPN[op], args, k, r.left, r.top, r.right, r.bottom, bad, md, fp.x, fp.y, gp, cp);
    UploadRect(k, r, NULL, 0);      // resync so one error does not cascade into later checks
}

static void WriteBmp(const char* name, int w, int h, const BYTE* bgra, int pitch) {
    char p[MAX_PATH]; sprintf(p, "%s%s", g_dir, name);
    FILE* fp = fopen(p, "wb"); if (!fp) return;
    int rb = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + rb * h), 0, 0, 54 };
    BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 24 };
    fwrite(&fh, sizeof fh, 1, fp); fwrite(&ih, sizeof ih, 1, fp);
    std::vector<BYTE> row(rb, 0);
    for (int y = h - 1; y >= 0; y--) {
        const UINT32* s = (const UINT32*)(bgra + y * pitch);
        for (int x = 0; x < w; x++) { row[x * 3] = s[x] & 255; row[x * 3 + 1] = (s[x] >> 8) & 255; row[x * 3 + 2] = (s[x] >> 16) & 255; }
        fwrite(row.data(), rb, 1, fp);
    }
    fclose(fp);
}

static void Screenshot() {
    CpuImg c;
    if (!CpuOf(0, c) || !G[0].b || !EnsureRead()) return;
    if (g_ref) {
        std::vector<UINT32> px(c.w * c.h);
        for (int y = 0; y < c.h; y++) for (int x = 0; x < c.w; x++) px[y * c.w + x] = g_L[c.p[y * c.pitch + x]];
        WriteBmp("DweepD2D_cpu.bmp", c.w, c.h, (BYTE*)px.data(), c.w * 4);
    }
    Close();
    D2D1_POINT_2U p = { 0, 0 };
    D2D1_RECT_U sr = D2D1::RectU(0, 0, G[0].w * S, G[0].h * S);
    if (SUCCEEDED(g_read->CopyFromBitmap(&p, G[0].b, &sr))) {
        D2D1_MAPPED_RECT m;
        if (SUCCEEDED(g_read->Map(D2D1_MAP_OPTIONS_READ, &m))) {
            WriteBmp("DweepD2D_gpu.bmp", G[0].w * S, G[0].h * S, m.bits, m.pitch);
            g_read->Unmap();
        }
    }
    Log("screenshots written (DweepD2D_cpu.bmp, DweepD2D_gpu.bmp)");
}

static void VerifyFrame() {
    if (S != 1 || !G[0].b) return;
    CpuImg c;
    if (!CpuOf(0, c)) return;
    int md; POINT fp; UINT32 gp, cp;
    int bad = Compare(0, RECT{ 0, 0, c.w, c.h }, &md, &fp, &gp, &cp);
    static int logged;
    if (bad && logged++ < 50) {
        Log("VERIFY frame: %d px differ, max %d, first (%d,%d) gpu %06X cpu %06X", bad, md, fp.x, fp.y, gp, cp);
        if (logged == 1) Screenshot();
    }
}

// ------------------------------------------------------------------ native GPU primitives
static void G_Copy(int src, int sx, int sy, int w, int h, int dst, int dx, int dy, float opacity) {
    if (w <= 0 || h <= 0 || !DstScreen(dst)) return;
    float ss;
    ID2D1Bitmap* sb = SourceBmp(src, ss);
    if (!sb) return;
    int sw = (int)(sb->GetPixelSize().width / ss), shh = (int)(sb->GetPixelSize().height / ss);
    int x0 = max(sx, 0), y0 = max(sy, 0), x1 = min(sx + w, sw), y1 = min(sy + h, shh);
    if (x1 <= x0 || y1 <= y0) return;
    dx += x0 - sx; dy += y0 - sy;
    if (src == dst) {           // overlapping copy inside one image
        Close();
        if (!g_tmp) g_tmp = NewBmp(640 * S, 480 * S, D2D1_BITMAP_OPTIONS_NONE);
        D2D1_POINT_2U p = { 0, 0 };
        D2D1_RECT_U r = D2D1::RectU(x0 * S, y0 * S, x1 * S, y1 * S);
        if (!g_tmp || FAILED(g_tmp->CopyFromBitmap(&p, (ID2D1Bitmap*)sb, &r))) return;
        sb = g_tmp; x1 -= x0; y1 -= y0; x0 = y0 = 0;
    }
    Target(G[dst].b);
    D2D1_RECT_F s = D2D1::RectF(x0 * ss, y0 * ss, x1 * ss, y1 * ss);
    D2D1_RECT_F d = SR(dx, dy, dx + (x1 - x0), dy + (y1 - y0));
    g_dc->DrawBitmap(sb, &d, opacity, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &s, NULL);
}

static void G_Tile(int src, int w, int h, int dst, int dx, int dy) {
    if (w <= 0 || h <= 0 || !DstScreen(dst)) return;
    ID2D1Bitmap1* a = AssetBmp(src);
    if (!a) return;
    if (!A[src].brush)      // the tile is w x h of the image (a frame sheet may be taller)
        g_dc->CreateImageBrush(a, D2D1::ImageBrushProperties(D2D1::RectF(0, 0, (float)ImgW(src), (float)ImgH(src)),
                               D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &A[src].brush);
    if (!A[src].brush) return;
    A[src].brush->SetTransform(D2D1::Matrix3x2F::Scale((float)S, (float)S) * D2D1::Matrix3x2F::Translation((float)dx * S, (float)dy * S));
    Target(G[dst].b);
    g_dc->FillRectangle(SR(dx, dy, dx + w, dy + h), A[src].brush);
}

enum { SPR_NORMAL, SPR_HALF, SPR_ALPHA };
static RECT SpriteRect(int img, int g, int f, int x, int y, const RECT* clip) {
    RECT r = { 0, 0, 0, 0 };
    int* d = FrameDesc(img, g, f);
    if (!d) return r;
    r = { x, y, x + d[1], y + d[2] };
    if (HasClip(clip)) { RECT c = { clip->left, clip->top, clip->right + 1, clip->bottom + 1 }; IntersectRect(&r, &r, &c); }
    return r;
}
static void G_Sprite(int img, int g, int f, int dst, int x, int y, const RECT* clip, const WORD* pal, int mode, int alpha) {
    if (!DstScreen(dst)) return;
    FBmp* fb = Frame(img, g, f, pal, pal ? FR_PAL8 : FR_RLE16);
    if (!fb) return;
    // the game intersects inclusive rects with IntersectRect, so a visible part only 1 pixel wide or high is dropped
    RECT sr = { x, y, x + fb->w - 1, y + fb->h - 1 }, o;
    if (HasClip(clip) ? !IntersectRect(&o, &sr, clip) : (!x && !y && !sr.right && !sr.bottom)) return;
    Target(G[dst].b);
    ClipScope cs(clip);
    D2D1_RECT_F d = SR(x, y, x + fb->w, y + fb->h);
    if (fb->shadow && mode != SPR_ALPHA) g_dc->DrawBitmap(fb->shadow, &d, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL, NULL);
    float op = mode == SPR_HALF ? 0.5f : mode == SPR_ALPHA ? alpha / 255.f : 1.f;
    if (fb->color && op > 0) g_dc->DrawBitmap(fb->color, &d, op, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL, NULL);
}

static void G_Rect(const RECT* r, WORD color, int filled, int dst, const RECT* clip, float opacity) {
    if (!r || !DstScreen(dst)) return;
    Target(G[dst].b);
    ClipScope cs(clip);
    g_brush->SetColor(C565(color));
    g_brush->SetOpacity(opacity);
    if (filled == 1) g_dc->FillRectangle(SR(r->left, r->top, r->right + 1, r->bottom + 1), g_brush);
    else {
        g_dc->FillRectangle(SR(r->left, r->top, r->right + 1, r->top + 1), g_brush);
        if (r->bottom != r->top) g_dc->FillRectangle(SR(r->left, r->bottom, r->right + 1, r->bottom + 1), g_brush);
        g_dc->FillRectangle(SR(r->left, r->top, r->left + 1, r->bottom + 1), g_brush);
        if (r->right != r->left) g_dc->FillRectangle(SR(r->right, r->top, r->right + 1, r->bottom + 1), g_brush);
    }
    g_brush->SetOpacity(1.f);
}

// dirty-rect manager: int count at +0x7d4, RECT* list at +4; rects are inclusive
static void G_DirtyCopy(int* mgr, int src, int dst, const RECT* bound) {
    int n = mgr[0x1f5];
    if (n <= 0 || n > 500 || !Scr(src) || !Scr(dst)) return;
    Target(G[dst].b);
    for (int i = 0; i < n; i++) {
        RECT r = *(RECT*)mgr[1 + i];
        if (bound) IntersectRect(&r, &r, bound);
        D2D1_RECT_F d = SR(r.left, r.top, r.right + 1, r.bottom + 1);
        g_dc->DrawBitmap(G[src].b, &d, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &d, NULL);
    }
}

// ------------------------------------------------------------------ effects (formerly CPU-mirrored)
// The game's lookup tables: darken = every channel halved; brighten = +7/31 R, +15/63 G, +7/31 B, saturating;
// gray = luminance R/3 + G/2 + B/6 of the brightened colour.

static inline void Fill(int x, int y, int w, int h) { if (w > 0 && h > 0) g_dc->FillRectangle(SR(x, y, x + w, y + h), g_brush); }
static void SetBlack(float a) { g_brush->SetColor(D2D1::ColorF(0, 0, 0)); g_brush->SetOpacity(a); }
static void BeginBright() {
    g_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    g_brush->SetColor(D2D1::ColorF(7 / 31.f, 15 / 63.f, 7 / 31.f, 1.f));
    g_brush->SetOpacity(1.f);
}
static void EndBlend() { g_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER); g_brush->SetOpacity(1.f); }

// raised bevel b pixels wide with diagonal corners: top/left brightened, bottom/right darkened
static void Bevel(int x, int y, int w, int h, int b, bool bright) {
    for (int i = 0; i < b; i++) {
        if (bright) { Fill(x, y + i, w - i, 1); Fill(x, y + h - 1 - i, i, 1); }
        else { Fill(x + w - i, y + i, i, 1); Fill(x + i, y + h - 1 - i, w - i, 1); }
    }
    if (bright) Fill(x, y + b, b, h - 2 * b); else Fill(x + w - b, y + b, b, h - 2 * b);
}
static void RaisedBevel(int x, int y, int w, int h, int b) {
    SetBlack(0.5f); Bevel(x, y, w, h, b, false);
    BeginBright(); Bevel(x, y, w, h, b, true);
    EndBlend();
}

// source pixel offset of a frame inside a raw 16-bit sheet -> (x, y)
static bool FrameXY(int img, int g, int f, int& sx, int& sy) {
    int* d = FrameDesc(img, g, f);
    int pitch = ImgPitch(img);
    if (!d || pitch <= 0) return false;
    sx = d[0] % pitch; sy = d[0] / pitch;
    return true;
}

// 0040de10: button face from BtnBkg (image 6), bevel width b; pressed = texture unshifted + 1px dark border
static void G_BevelBtn(int w, int h, int dst, int x, int y, int b, int pressed) {
    if (w <= 0 || h <= 0 || !DstScreen(dst)) return;
    int o = pressed ? 0 : b;
    G_Copy(6, o, o, w, h, dst, x, y, 1.f);
    Target(G[dst].b);
    if (pressed) { SetBlack(0.5f); Fill(x, y, w, 1); Fill(x, y + 1, 1, h - 2); Fill(x + w - 1, y + 1, 1, h - 2); Fill(x, y + h - 1, w, 1); EndBlend(); }
    else RaisedBevel(x, y, w, h, b);
}
// 0040e210: frame of a raw sheet with a 2px bevel; pressed = dark top row and left column
static void G_BevelFrame(int src, int w, int h, int dst, int x, int y, int g, int f, int pressed) {
    int sx, sy;
    if (w <= 0 || h <= 0 || !DstScreen(dst) || !FrameXY(src, g, f, sx, sy)) return;
    G_Copy(src, sx, sy, w, h, dst, x, y, 1.f);
    Target(G[dst].b);
    if (pressed) { SetBlack(0.5f); Fill(x, y, w, 1); Fill(x, y + 1, 1, h - 1); EndBlend(); }
    else RaisedBevel(x, y, w, h, 2);
}
// 0040e4d0 / 0040e600: frame copied (or blended 50%) plus a green outline (opaque / 50%)
static void G_FrameOutline(int src, int w, int h, int dst, int x, int y, int g, int f, bool blend) {
    int sx, sy;
    if (w <= 0 || h <= 0 || !DstScreen(dst) || !FrameXY(src, g, f, sx, sy)) return;
    G_Copy(src, sx, sy, w, h, dst, x, y, blend ? 0.5f : 1.f);
    RECT r = { x - 1, y - 1, x + w, y + h };
    G_Rect(&r, 0x07E0, 0, dst, (RECT*)0x450320, blend ? 128 / 255.f : 1.f);
}

// clipped rect as the game computes it (IntersectRect on inclusive rects); false = nothing drawn
static bool GameClip(const RECT* r, const RECT* clip, RECT& o) {
    if (!HasClip(clip)) o = *r;
    else IntersectRect(&o, r, clip);
    return (o.left || o.right || o.top || o.bottom) && o.right >= o.left && o.bottom >= o.top;
}
// 004110b0: dialog panel = DialgBkg tiled inside, black top/left, white bottom/right, darkened interior
static void G_Panel(const RECT* r, int dst, const RECT* clip) {
    if (!r || !DstScreen(dst)) return;
    G_Tile(8, r->right - r->left - 1, r->bottom - r->top - 1, dst, r->left + 1, r->top + 1);
    RECT c;
    if (!GameClip(r, clip, c)) return;
    int W = c.right - c.left, H = c.bottom - c.top;
    Target(G[dst].b);
    SetBlack(1.f);
    Fill(c.left, c.top, W + 1, 1);
    if (H > 1) Fill(c.left, c.top + 1, 1, H - 1);
    SetBlack(0.5f);
    if (H > 1 && W > 1) Fill(c.left + 1, c.top + 1, W - 1, H - 1);
    g_brush->SetColor(D2D1::ColorF(1, 1, 1)); g_brush->SetOpacity(1.f);
    if (H > 1) Fill(W > 1 ? c.right : c.left + 1, c.top + 1, 1, H - 1);
    Fill(c.left, H > 1 ? c.bottom : c.top + 1, W + 1, 1);
}

static bool EnsureEffects() {
    if (g_fxGray) return true;
    if (FAILED(g_dc->CreateEffect(CLSID_D2D1ColorMatrix, &g_fxBright)) || FAILED(g_dc->CreateEffect(CLSID_D2D1ColorMatrix, &g_fxGray))) {
        Log("ColorMatrix effect unavailable"); SafeRelease(g_fxBright); return false;
    }
    D2D1_MATRIX_5X4_F b = D2D1::Matrix5x4F(1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1,  7 / 31.f, 15 / 63.f, 7 / 31.f, 0);
    g_fxBright->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, b);
    g_fxBright->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
    const float r = 1 / 3.f, gg = 1 / 2.f, bb = 1 / 6.f;
    D2D1_MATRIX_5X4_F m = D2D1::Matrix5x4F(r, r, r, 0,  gg, gg, gg, 0,  bb, bb, bb, 0,  0, 0, 0, 1,  0, 0, 0, 0);
    g_fxGray->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
    g_fxGray->SetInputEffect(0, g_fxBright);
    return true;
}
// 00410fd0: gray out a rectangle (brighten, then luminance)
static void G_Gray(const RECT* r, int dst, const RECT* clip) {
    RECT c;
    if (!r || !DstScreen(dst) || !GameClip(r, clip, c) || !EnsureEffects()) return;
    c.right++; c.bottom++;
    RECT img = { 0, 0, G[dst].w, G[dst].h };
    if (!IntersectRect(&c, &c, &img)) return;
    if (!g_tmp) g_tmp = NewBmp(640 * S, 480 * S, D2D1_BITMAP_OPTIONS_NONE);
    if (!g_tmp) return;
    Close();
    D2D1_POINT_2U p0 = { 0, 0 };
    D2D1_RECT_U ur = D2D1::RectU(c.left * S, c.top * S, c.right * S, c.bottom * S);
    if (FAILED(g_tmp->CopyFromBitmap(&p0, G[dst].b, &ur))) return;
    g_fxBright->SetInput(0, g_tmp);
    Target(G[dst].b);
    D2D1_POINT_2F at = D2D1::Point2F((float)c.left * S, (float)c.top * S);
    D2D1_RECT_F ir = D2D1::RectF(0, 0, (float)(c.right - c.left) * S, (float)(c.bottom - c.top) * S);
    g_dc->DrawImage(g_fxGray, &at, &ir, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
}

// 0040f8a0: red +16 under the sprite's literal pixels
static void G_SpriteRed(int img, int g, int f, int dst, int x, int y, const RECT* clip) {
    if (!DstScreen(dst)) return;
    FBmp* fb = Frame(img, g, f, NULL, FR_TINT);
    if (!fb || !fb->color) return;
    RECT sr = { x, y, x + fb->w - 1, y + fb->h - 1 }, o;
    if (HasClip(clip) ? !IntersectRect(&o, &sr, clip) : (!x && !y && !sr.right && !sr.bottom)) return;
    Target(G[dst].b);
    ClipScope cs(clip);
    D2D1_RECT_F d = SR(x, y, x + fb->w, y + fb->h);
    g_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    g_dc->DrawBitmap(fb->color, &d, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL, NULL);
    g_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

// 00411310: the 16x10 floor. Each 40x40 cell = texture + black top/left line, bright inner edge, dark bottom/right.
// toBg == 0: only heat/ice cells (animated) into the back buffer; else every cell into the background, then copied.
static void AddDirty(int l, int t, int r, int b, int flag);
static void G_Floor(int toBg, bool dirty) {
    int dst = toBg ? 2 : 1;
    if (!DstScreen(dst)) return;
    int lvl = *(int*)0x4515cc;
    BYTE* grid = (BYTE*)(*(int*)0x4515d8 - 0xf8 + lvl * 0x1e8);
    DWORD now = timeGetTime();
    struct Cell { int x, y; };
    Cell cells[160]; int n = 0;
    for (int r = 0; r < 10; r++) for (int c = 0; c < 16; c++) {
        int v = grid[r * 16 + c], img;
        if (v == 1) continue;
        if (v == 2) img = 0x12;
        else if (v == 3) img = 0x13;
        else if (!toBg) continue;
        else if (v == 4) img = 0x14;
        else img = 0xd + (*(int*)0x4515bc ? *(int*)0x4515b8 : (int)((unsigned)(lvl - 1) % 5u));
        int W = ImgW(img), H = ImgH(img), pitch = ImgPitch(img);
        if (W <= 0 || H <= 0 || pitch <= 0) continue;
        int cx = c * 40, cy = r * 40;
        int off = (cy % H) * W + cx % W;
        if (img == 0x12 || img == 0x13) {
            int fn = *(int*)(IMG(img) + 0x30);
            if (fn < 2) fn = 2;
            int k = (int)(fn - (now / 100) % (unsigned)(fn * 2 - 2)) - 1;
            int* d = FrameDesc(img, 0, abs(k));
            if (d) off += d[0];
        }
        G_Copy(img, off % pitch, off / pitch, 40, 40, dst, cx, cy, 1.f);
        cells[n++] = { cx, cy };
        if (dirty && !toBg) AddDirty(cx, cy, cx + 39, cy + 39, 0);
    }
    if (n) {
        Target(G[dst].b);
        SetBlack(1.f);
        for (int i = 0; i < n; i++) { Fill(cells[i].x, cells[i].y, 40, 1); Fill(cells[i].x, cells[i].y + 1, 1, 39); }
        SetBlack(0.5f);
        for (int i = 0; i < n; i++) { Fill(cells[i].x + 39, cells[i].y + 2, 1, 37); Fill(cells[i].x + 1, cells[i].y + 39, 39, 1); }
        BeginBright();
        for (int i = 0; i < n; i++) { Fill(cells[i].x + 1, cells[i].y + 1, 39, 1); Fill(cells[i].x + 1, cells[i].y + 2, 1, 37); }
        EndBlend();
    }
    if (toBg) {
        RECT* pf = (RECT*)0x44f998;
        G_Copy(2, 0, 0, pf->right - pf->left + 1, pf->bottom - pf->top + 1, 1, 0, 0, 1.f);
    }
}

// add a rect to the game's dirty list (0x44a2c8) exactly like the original effect functions do
static void AddDirty(int l, int t, int r, int b, int flag) {
    typedef void*(__cdecl* NewFn)(unsigned);
    typedef void(__fastcall* AddFn)(void*, void*, void*);
    int* p = (int*)((NewFn)0x42f95c)(0x14);         // the game's operator new: the list frees with its delete
    if (!p) return;
    p[0] = l; p[1] = t; p[2] = r; p[3] = b; p[4] = flag;
    ((AddFn)0x4040e0)((void*)0x44a2c8, NULL, p);
}

// lasers: 5-pixel-wide green beams; alpha of the centre / +-1 / +-2 lines comes from three 256-entry tables
// (this+4, +0x104, +0x204) indexed by (phase at this+0xaa4 + position) & 0xff
static bool LaserStrips(BYTE* t) {
    UINT32 h = 2166136261u;
    for (int i = 0; i < 768; i++) { h ^= t[4 + i]; h *= 16777619u; }
    if (g_laserH && h == g_laserHash) return true;
    Close();
    SafeRelease(g_laserH); SafeRelease(g_laserV);
    std::vector<UINT32> hs(512 * 3), vs(3 * 512);
    for (int k = 0; k < 3; k++) for (int i = 0; i < 512; i++) {
        UINT32 a = t[4 + k * 0x100 + (i & 255)];
        hs[k * 512 + i] = vs[i * 3 + k] = (a << 24) | (a << 8);
    }
    g_laserH = NewBmp(512, 3, D2D1_BITMAP_OPTIONS_NONE, hs.data(), 512 * 4);
    g_laserV = NewBmp(3, 512, D2D1_BITMAP_OPTIONS_NONE, vs.data(), 3 * 4);
    g_laserHash = h;
    return g_laserH && g_laserV;
}
static void LaserH(int phase, int x0, int yc, int len, int base) {
    int k = (phase + base) & 255;
    for (int d = -2; d <= 2; d++) {
        D2D1_RECT_F src = D2D1::RectF((float)k, (float)abs(d), (float)(k + len), (float)abs(d) + 1);
        D2D1_RECT_F dst = SR(x0, yc + d, x0 + len, yc + d + 1);
        g_dc->DrawBitmap(g_laserH, &dst, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src, NULL);
    }
}
static void LaserV(int phase, int xc, int y0, int len, int base) {
    int k = (phase + base) & 255;
    for (int d = -2; d <= 2; d++) {
        D2D1_RECT_F src = D2D1::RectF((float)abs(d), (float)k, (float)abs(d) + 1, (float)(k + len));
        D2D1_RECT_F dst = SR(xc + d, y0, xc + d + 1, y0 + len);
        g_dc->DrawBitmap(g_laserV, &dst, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src, NULL);
    }
}
// 00413150 (mask: 8 left half, 2 right half, 1 upper half, 4 lower half) and 00415440 (short stub below the cell)
static void LaserDirty(int cx, int cy, int mask, bool stub) {
    if (stub) { AddDirty(cx + 18, cy + 7, cx + 22, cy + 39, 1); return; }
    bool l = mask & 8, r = mask & 2, u = mask & 1, d = mask & 4;
    if (l || r) AddDirty(l ? cx : cx + 20, cy + 10, r ? cx + 39 : cx + 19, cy + 14, 1);
    if (u || d) AddDirty(cx + 18, u ? cy : cy + 12, cx + 22, d ? cy + 39 : cy + 11, 1);
}
static void G_Laser(void* self, int col, int row, int mask, bool stub, bool dirty) {
    BYTE* t = (BYTE*)self;
    int ph = *(int*)(t + 0xaa4), cx = col * 40, cy = row * 40;
    if (dirty) LaserDirty(cx, cy, mask, stub);
    if (!DstScreen(1) || !LaserStrips(t)) return;
    Target(G[1].b);
    if (stub) { LaserV(ph, cx + 20, cy + 33, 7, cy + cx + 20); return; }
    if (mask & 8) LaserH(ph, cx, cy + 12, 20, cy + 12 + cx);
    if (mask & 2) LaserH(ph, cx + 20, cy + 12, 20, cy + 12 + cx);
    if (mask & 1) LaserV(ph, cx + 20, cy, 12, cy + cx + 20);
    if (mask & 4) LaserV(ph, cx + 20, cy + 12, 28, cy + cx + 20);
}

// ------------------------------------------------------------------ text (GDI on the back buffer -> DirectWrite)
static TFmt* FontFor(HDC dc, const TEXTMETRICA& tm) {
    HFONT hf = (HFONT)GetCurrentObject(dc, OBJ_FONT);
    LOGFONTA lf = {};
    GetObjectA(hf, sizeof lf, &lf);
    char key[160];
    sprintf(key, "%s|%d|%d|%d|%d", lf.lfFaceName, tm.tmHeight, tm.tmInternalLeading, lf.lfWeight, lf.lfItalic);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return &it->second;
    // resolve GDI face names such as "Arial Bold" to a DirectWrite family + weight
    LOGFONTW lw = {};
    memcpy(&lw, &lf, offsetof(LOGFONTW, lfFaceName));
    MultiByteToWideChar(CP_ACP, 0, lf.lfFaceName[0] ? lf.lfFaceName : "Arial", -1, lw.lfFaceName, LF_FACESIZE);
    WCHAR face[LF_FACESIZE];
    wcscpy(face, lw.lfFaceName);
    DWRITE_FONT_WEIGHT weight = (DWRITE_FONT_WEIGHT)max(1, min(999, (int)(lf.lfWeight ? lf.lfWeight : 400)));
    DWRITE_FONT_STYLE style = lf.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    static IDWriteGdiInterop* gi;
    if (!gi) g_dw->GetGdiInterop(&gi);
    for (int attempt = 0; attempt < 2 && gi; attempt++) {
        IDWriteFont* font = NULL;
        if (SUCCEEDED(gi->CreateFontFromLOGFONT(&lw, &font)) && font) {
            IDWriteFontFamily* fam = NULL;
            IDWriteLocalizedStrings* names = NULL;
            if (SUCCEEDED(font->GetFontFamily(&fam)) && SUCCEEDED(fam->GetFamilyNames(&names))) {
                UINT32 idx = 0; BOOL ex = FALSE;
                names->FindLocaleName(L"en-us", &idx, &ex);
                names->GetString(ex ? idx : 0, face, LF_FACESIZE);
                weight = font->GetWeight(); style = font->GetStyle(); stretch = font->GetStretch();
            }
            SafeRelease(names); SafeRelease(fam); font->Release();
            break;
        }
        // not a family name: strip a style suffix ("Arial Bold" -> "Arial", bold)
        WCHAR* sp = wcsrchr(lw.lfFaceName, L' ');
        if (!sp) break;
        if (!_wcsicmp(sp + 1, L"Bold")) lw.lfWeight = FW_BOLD;
        else if (!_wcsicmp(sp + 1, L"Italic")) lw.lfItalic = TRUE;
        else break;
        *sp = 0;
        wcscpy(face, lw.lfFaceName);
        weight = (DWRITE_FONT_WEIGHT)lw.lfWeight; style = lw.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    }
    float em = (float)(tm.tmHeight - tm.tmInternalLeading) * S;
    TFmt t = { NULL };
    HRESULT hr = g_dw->CreateTextFormat(face, NULL, weight, style, stretch, em, L"en-us", &t.fmt);
    if (FAILED(hr)) { Log("CreateTextFormat %s failed %08X", lf.lfFaceName, hr); return NULL; }
    t.fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Log("font %s -> DirectWrite \"%S\" weight %d em %.1f", key, face, (int)weight, em);
    return &(g_fonts[key] = t);
}

// one line of text, GDI reference point (x, y) with GDI text alignment flags
static void G_TextLine(HDC dc, const TEXTMETRICA& tm, TFmt* f, const char* s, int n, float x, int y, UINT align, int ul = -1) {
    if (n <= 0) return;
    std::wstring w(n, L' ');
    int wn = MultiByteToWideChar(1252, 0, s, n, &w[0], n);       // the game's strings are Western: shows the (c) sign
    IDWriteTextLayout* lay = NULL;
    if (FAILED(g_dw->CreateTextLayout(w.c_str(), wn, f->fmt, 1e6f, 1e6f, &lay))) return;
    if (ul >= 0 && ul < wn) lay->SetUnderline(TRUE, DWRITE_TEXT_RANGE{ (UINT32)ul, 1 });
    DWRITE_TEXT_METRICS m;
    DWRITE_LINE_METRICS lm;
    UINT32 cnt = 0;
    lay->GetMetrics(&m);
    lay->GetLineMetrics(&lm, 1, &cnt);
    float tw = m.widthIncludingTrailingWhitespace;
    float ox = (align & 6) == 6 ? x * S - tw / 2 : (align & 6) == 2 ? x * S - tw : x * S;
    float by = (align & TA_BASELINE) == TA_BASELINE ? (float)y : (align & TA_BOTTOM) ? (float)(y - tm.tmDescent) : (float)(y + tm.tmAscent);
    COLORREF c = GetTextColor(dc);
    g_brush->SetColor(D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f));
    g_dc->DrawTextLayout(D2D1::Point2F(ox, by * S - lm.baseline), lay, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    lay->Release();
}

static bool BackDC(HDC dc) {
    Surface* s = ImgSurf(1);
    return g_hooked && g_depth == 0 && s && s->dc == dc && Scr(1);
}

static void G_TextOut(HDC dc, int x, int y, const char* s, int n, const RECT* clip) {
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    TFmt* f = FontFor(dc, tm);
    if (!f) return;
    UINT al = GetTextAlign(dc);
    Target(G[1].b);
    if (clip) g_dc->PushAxisAlignedClip(SR(clip->left, clip->top, clip->right, clip->bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    G_TextLine(dc, tm, f, s, n, (float)x, y, al);
    if (clip) g_dc->PopAxisAlignedClip();
}

static void G_DrawText(HDC dc, const char* s, int n, RECT rc, UINT fmt) {
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    TFmt* f = FontFor(dc, tm);
    if (!f) return;
    std::string str(s, n);
    int ul = -1;
    if (!(fmt & DT_NOPREFIX)) {
        std::string o;
        for (size_t i = 0; i < str.size(); i++) {
            if (str[i] == '&' && i + 1 < str.size()) { i++; if (str[i] != '&' && ul < 0) ul = (int)o.size(); }
            o += str[i];
        }
        str = o;
    }
    std::vector<std::string> lines;
    if (fmt & DT_SINGLELINE) {
        for (char& ch : str) if (ch == '\r' || ch == '\n') ch = ' ';
        lines.push_back(str);
    } else {
        int width = rc.right - rc.left;
        size_t p = 0;
        while (p <= str.size()) {
            size_t e = str.find('\n', p);
            if (e == std::string::npos) e = str.size();
            std::string para = str.substr(p, e - p);
            if (!para.empty() && para.back() == '\r') para.pop_back();
            if (!(fmt & DT_WORDBREAK)) lines.push_back(para);
            else {
                std::string line;
                size_t q = 0;
                bool first = true;
                while (q <= para.size()) {
                    size_t sp = para.find(' ', q);
                    if (sp == std::string::npos) sp = para.size();
                    std::string word = para.substr(q, sp - q);
                    std::string test = first ? word : line + " " + word;
                    SIZE sz = {};
                    GetTextExtentPoint32A(dc, test.c_str(), (int)test.size(), &sz);
                    if (!first && sz.cx > width) { lines.push_back(line); line = word; }
                    else line = test;
                    first = false;
                    q = sp + 1;
                }
                lines.push_back(line);
            }
            p = e + 1;
        }
    }
    int lh = tm.tmHeight + ((fmt & DT_EXTERNALLEADING) ? tm.tmExternalLeading : 0);
    int y = rc.top;
    if (fmt & DT_SINGLELINE) {
        if (fmt & DT_VCENTER) y = rc.top + (rc.bottom - rc.top - lh) / 2;
        else if (fmt & DT_BOTTOM) y = rc.bottom - lh;
    }
    Target(G[1].b);
    bool clip = !(fmt & DT_NOCLIP);
    if (clip) g_dc->PushAxisAlignedClip(SR(rc.left, rc.top, rc.right, rc.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (lines.size() != 1) ul = -1;
    for (auto& l : lines) {
        if (fmt & DT_CENTER) G_TextLine(dc, tm, f, l.c_str(), (int)l.size(), (rc.left + rc.right) / 2.f, y, TA_CENTER | TA_TOP, ul);
        else if (fmt & DT_RIGHT) G_TextLine(dc, tm, f, l.c_str(), (int)l.size(), (float)rc.right, y, TA_RIGHT | TA_TOP, ul);
        else G_TextLine(dc, tm, f, l.c_str(), (int)l.size(), (float)rc.left, y, TA_LEFT | TA_TOP, ul);
        y += lh;
    }
    if (clip) g_dc->PopAxisAlignedClip();
}

// In verify mode text is mirrored from the CPU (GDI) result so the comparisons stay meaningful.
template <class F> static auto MirrorAll(int k, F call) -> decltype(call()) {
    Snap s;
    CpuImg c;
    if (CpuOf(k, c)) Grab(s, k, RECT{ 0, 0, c.w, c.h });
    g_depth++;
    auto r = call();
    g_depth--;
    Put(s, false);
    return r;
}

static BOOL WINAPI H_TextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
    if (!BackDC(dc)) return TextOutA(dc, x, y, s, n);
    g_calls[OP_TEXT]++;
    if (g_cfgVerify) return MirrorAll(1, [&] { return TextOutA(dc, x, y, s, n); });
    BOOL r = g_ref ? TextOutA(dc, x, y, s, n) : TRUE;
    G_TextOut(dc, x, y, s, n, NULL);
    return r;
}
static BOOL WINAPI H_ExtTextOutA(HDC dc, int x, int y, UINT o, const RECT* rc, LPCSTR s, UINT n, const INT* dx) {
    if (!BackDC(dc)) return ExtTextOutA(dc, x, y, o, rc, s, n, dx);
    g_calls[OP_TEXT]++;
    if (g_cfgVerify) return MirrorAll(1, [&] { return ExtTextOutA(dc, x, y, o, rc, s, n, dx); });
    BOOL r = g_ref ? ExtTextOutA(dc, x, y, o, rc, s, n, dx) : TRUE;
    if ((o & ETO_OPAQUE) && rc) {
        COLORREF c = GetBkColor(dc);
        Target(G[1].b);
        g_brush->SetColor(D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f));
        g_dc->FillRectangle(SR(rc->left, rc->top, rc->right, rc->bottom), g_brush);
    }
    if (s && n) G_TextOut(dc, x, y, s, (int)n, (o & ETO_CLIPPED) ? rc : NULL);
    return r;
}
static int WINAPI H_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT rc, UINT fmt) {
    if ((fmt & DT_CALCRECT) || !rc || !s || !BackDC(dc)) return DrawTextA(dc, s, n, rc, fmt);
    g_calls[OP_TEXT]++;
    if (g_cfgVerify) return MirrorAll(1, [&] { return DrawTextA(dc, s, n, rc, fmt); });
    RECT r0 = *rc;
    int r;
    if (g_ref) r = DrawTextA(dc, s, n, rc, fmt);
    else {      // same return value without rasterizing: text height, or offset of its bottom when centred
        RECT m = r0;
        r = DrawTextA(dc, s, n, &m, fmt | DT_CALCRECT);
        if (fmt & DT_SINGLELINE) {
            int rh = r0.bottom - r0.top;
            if (fmt & DT_BOTTOM) r = rh;
            else if (fmt & DT_VCENTER) r = (rh - r) / 2 + r;
        }
    }
    G_DrawText(dc, s, n < 0 ? (int)strlen(s) : n, r0, fmt);
    return r;
}

// ------------------------------------------------------------------ detours
// The blitters are __thiscall with callee-cleaned stack args; a __fastcall function with a dummy EDX has the same ABI.
#define TC __fastcall
typedef int(TC* F0)(void*, void*);
typedef int(TC* F1)(void*, void*, int);
typedef int(TC* F2)(void*, void*, int, int);
typedef int(TC* F3)(void*, void*, int, int, int);
typedef int(TC* F4)(void*, void*, int, int, int, int);
typedef int(TC* F5)(void*, void*, int, int, int, int, int);
typedef int(TC* F6)(void*, void*, int, int, int, int, int, int);
typedef int(TC* F7)(void*, void*, int, int, int, int, int, int, int);
typedef int(TC* F8)(void*, void*, int, int, int, int, int, int, int, int);
typedef int(TC* F9)(void*, void*, int, int, int, int, int, int, int, int, int);

static F8 O_CopyRect, O_Blend50, O_FrameOutline, O_FrameBlend, O_Sprite8, O_Sprite8Half;
static F6 O_Tile;
static F7 O_BevelBtn, O_SpriteRle;
static F8 O_Sprite8Red;
static F9 O_BevelFrame, O_Sprite8Alpha;
static F5 O_Rect;
static F6 O_RectAlpha;
static F3 O_Gray, O_Panel, O_Laser;
static F4 O_TextAA;
static F1 O_Floor;
static F2 O_Laser2;
static F0 O_DirtyFlush, O_DirtyErase;

#define GPU (g_hooked && g_depth == 0 && EnsureDevice())
#define CALL(...) (g_depth++, r = __VA_ARGS__, g_depth--, r)
// Without the CPU reference the original only runs for the background (image 2, drawn once per level: it is kept
// so the GPU images can be rebuilt after a device reset) and for drawing into asset images.
static bool SkipCpu(int dst) { return !g_ref && dst >= 0 && dst < NSCR && dst != 2 && Scr(dst) != NULL; }
#define ORIG(dst, ...) (SkipCpu(dst) ? (r = 0) : CALL(__VA_ARGS__))
static char g_args[160];
#define ARGS(...) (g_cfgVerify >= 2 ? (sprintf(g_args, __VA_ARGS__), g_args) : "")

static RECT Rc(int x, int y, int w, int h, int m = 0) { return RECT{ x - m, y - m, x + w + m, y + h + m }; }
static RECT RcIncl(const RECT* r, const RECT* clip, int m = 0) {
    RECT o = { r->left - m, r->top - m, r->right + 1 + m, r->bottom + 1 + m };
    if (HasClip(clip)) { RECT c = { clip->left, clip->top, clip->right + 1, clip->bottom + 1 }; IntersectRect(&o, &o, &c); }
    return o;
}

// run the original with mirroring of up to two screen-image regions
static int Mirror(Op op, int k1, RECT r1, int k2, RECT r2, bool force, int (*call)(void*), void* ctx) {
    g_calls[op]++;
    Snap a, b;
    if (k1 >= NSCR) InvalidateAsset(k1); else Grab(a, k1, r1);
    if (k2 >= NSCR) InvalidateAsset(k2); else if (k2 >= 0) Grab(b, k2, r2);
    g_depth++;
    int r = call(ctx);
    g_depth--;
    Put(a, force); Put(b, force);
    return r;
}
#define MIRROR(op, k1, r1, k2, r2, force, expr) \
    do { auto fn = [&]() { return expr; }; \
         r = Mirror(op, k1, r1, k2, r2, force, [](void* c) { return (*(decltype(fn)*)c)(); }, &fn); } while (0)

static int TC H_CopyRect(void* t, void*, int src, int sx, int sy, int w, int h, int dst, int dx, int dy) {
    int r;
    if (!GPU) return O_CopyRect(t, 0, src, sx, sy, w, h, dst, dx, dy);
    g_calls[OP_COPY]++;
    ORIG(dst, O_CopyRect(t, 0, src, sx, sy, w, h, dst, dx, dy));
    G_Copy(src, sx, sy, w, h, dst, dx, dy, 1.f);
    VerifyOp(OP_COPY, dst, Rc(dx, dy, w, h), ARGS("src %d (%d,%d) %dx%d", src, sx, sy, w, h));
    return r;
}
static int TC H_Blend50(void* t, void*, int src, int sx, int sy, int w, int h, int dst, int dx, int dy) {
    int r;
    if (!GPU) return O_Blend50(t, 0, src, sx, sy, w, h, dst, dx, dy);
    g_calls[OP_BLEND]++;
    ORIG(dst, O_Blend50(t, 0, src, sx, sy, w, h, dst, dx, dy));
    G_Copy(src, sx, sy, w, h, dst, dx, dy, 0.5f);
    VerifyOp(OP_BLEND, dst, Rc(dx, dy, w, h), ARGS("src %d (%d,%d) %dx%d", src, sx, sy, w, h));
    return r;
}
static int TC H_Tile(void* t, void*, int src, int w, int h, int dst, int dx, int dy) {
    int r;
    if (!GPU) return O_Tile(t, 0, src, w, h, dst, dx, dy);
    g_calls[OP_TILE]++;
    ORIG(dst, O_Tile(t, 0, src, w, h, dst, dx, dy));
    G_Tile(src, w, h, dst, dx, dy);
    VerifyOp(OP_TILE, dst, Rc(dx, dy, w, h), ARGS("src %d %dx%d", src, w, h));
    return r;
}
static int TC H_SpriteRle(void* t, void*, int img, int g, int f, int dst, int x, int y, int clip) {
    int r;
    if (!GPU) return O_SpriteRle(t, 0, img, g, f, dst, x, y, clip);
    g_calls[OP_SPR16]++;
    ORIG(dst, O_SpriteRle(t, 0, img, g, f, dst, x, y, clip));
    G_Sprite(img, g, f, dst, x, y, (RECT*)clip, NULL, SPR_NORMAL, 255);
    VerifyOp(OP_SPR16, dst, SpriteRect(img, g, f, x, y, (RECT*)clip), ARGS("img %d g%d f%d", img, g, f));
    return r;
}
static const WORD* PalOf(int img, int pal) { return pal ? (const WORD*)pal : ImgPal(img); }   // NULL = no palette, skip
static int TC H_Sprite8(void* t, void*, int img, int g, int f, int dst, int x, int y, int clip, int pal) {
    int r;
    if (!GPU) return O_Sprite8(t, 0, img, g, f, dst, x, y, clip, pal);
    g_calls[OP_SPR8]++;
    ORIG(dst, O_Sprite8(t, 0, img, g, f, dst, x, y, clip, pal));
    if (PalOf(img, pal)) G_Sprite(img, g, f, dst, x, y, (RECT*)clip, PalOf(img, pal), SPR_NORMAL, 255);
    VerifyOp(OP_SPR8, dst, SpriteRect(img, g, f, x, y, (RECT*)clip), ARGS("img %d g%d f%d pal %X", img, g, f, pal));
    return r;
}
static int TC H_Sprite8Half(void* t, void*, int img, int g, int f, int dst, int x, int y, int clip, int pal) {
    int r;
    if (!GPU) return O_Sprite8Half(t, 0, img, g, f, dst, x, y, clip, pal);
    g_calls[OP_SPR8H]++;
    ORIG(dst, O_Sprite8Half(t, 0, img, g, f, dst, x, y, clip, pal));
    if (PalOf(img, pal)) G_Sprite(img, g, f, dst, x, y, (RECT*)clip, PalOf(img, pal), SPR_HALF, 128);
    VerifyOp(OP_SPR8H, dst, SpriteRect(img, g, f, x, y, (RECT*)clip), ARGS("img %d g%d f%d", img, g, f));
    return r;
}
static int TC H_Sprite8Alpha(void* t, void*, int img, int g, int f, int dst, int x, int y, int clip, int alpha, int pal) {
    int r;
    if (!GPU) return O_Sprite8Alpha(t, 0, img, g, f, dst, x, y, clip, alpha, pal);
    g_calls[OP_SPR8A]++;
    ORIG(dst, O_Sprite8Alpha(t, 0, img, g, f, dst, x, y, clip, alpha, pal));
    if ((alpha & 0xff) && PalOf(img, pal)) G_Sprite(img, g, f, dst, x, y, (RECT*)clip, PalOf(img, pal), SPR_ALPHA, alpha & 0xff);
    VerifyOp(OP_SPR8A, dst, SpriteRect(img, g, f, x, y, (RECT*)clip), ARGS("img %d g%d f%d a%d", img, g, f, alpha));
    return r;
}
static int TC H_Rect(void* t, void*, int rect, int color, int filled, int dst, int clip) {
    int r;
    if (!GPU) return O_Rect(t, 0, rect, color, filled, dst, clip);
    g_calls[OP_RECT]++;
    ORIG(dst, O_Rect(t, 0, rect, color, filled, dst, clip));
    G_Rect((RECT*)rect, (WORD)color, filled, dst, (RECT*)clip, 1.f);
    VerifyOp(OP_RECT, dst, RcIncl((RECT*)rect, (RECT*)clip), ARGS("color %04X filled %d", color & 0xffff, filled));
    return r;
}
static int TC H_RectAlpha(void* t, void*, int rect, int color, int filled, int dst, int clip, int alpha) {
    int r;
    if (!GPU) return O_RectAlpha(t, 0, rect, color, filled, dst, clip, alpha);
    g_calls[OP_RECTA]++;
    ORIG(dst, O_RectAlpha(t, 0, rect, color, filled, dst, clip, alpha));
    G_Rect((RECT*)rect, (WORD)color, filled, dst, (RECT*)clip, (alpha & 0xff) / 255.f);
    VerifyOp(OP_RECTA, dst, RcIncl((RECT*)rect, (RECT*)clip), ARGS("color %04X filled %d a%d", color & 0xffff, filled, alpha));
    return r;
}
static int TC H_DirtyFlush(void* t, void*) {
    int r;
    if (!GPU) return O_DirtyFlush(t, 0);
    g_calls[OP_FLUSH]++;
    G_DirtyCopy((int*)t, 1, 0, NULL);      // before the original: it frees the rect list
    if (SkipCpu(0)) {                       // only the list cleanup (00404020) of the original
        ((void(__fastcall*)(void*))0x404020)(t);
        return 0;
    }
    return CALL(O_DirtyFlush(t, 0));
}
static int TC H_DirtyErase(void* t, void*) {
    int r;
    if (!GPU) return O_DirtyErase(t, 0);
    g_calls[OP_ERASE]++;
    G_DirtyCopy((int*)t, 2, 1, (RECT*)0x44f998);
    if (SkipCpu(1)) return 0;               // pure pixel copy, nothing else to do
    return CALL(O_DirtyErase(t, 0));
}

// formerly mirrored effects, now drawn natively as well
static int TC H_BevelBtn(void* t, void*, int w, int h, int dst, int x, int y, int bw, int pressed) {
    int r;
    if (!GPU) return O_BevelBtn(t, 0, w, h, dst, x, y, bw, pressed);
    g_calls[OP_BEVBTN]++;
    ORIG(dst, O_BevelBtn(t, 0, w, h, dst, x, y, bw, pressed));
    G_BevelBtn(w, h, dst, x, y, bw, pressed);
    VerifyOp(OP_BEVBTN, dst, Rc(x, y, w, h), ARGS("%dx%d bevel %d pressed %d", w, h, bw, pressed));
    return r;
}
static int TC H_BevelFrame(void* t, void*, int src, int w, int h, int dst, int x, int y, int g, int f, int pressed) {
    int r;
    if (!GPU) return O_BevelFrame(t, 0, src, w, h, dst, x, y, g, f, pressed);
    g_calls[OP_BEVFR]++;
    ORIG(dst, O_BevelFrame(t, 0, src, w, h, dst, x, y, g, f, pressed));
    G_BevelFrame(src, w, h, dst, x, y, g, f, pressed);
    VerifyOp(OP_BEVFR, dst, Rc(x, y, w, h), ARGS("src %d g%d f%d %dx%d pressed %d", src, g, f, w, h, pressed));
    return r;
}
static int TC H_FrameOutline(void* t, void*, int src, int w, int h, int dst, int x, int y, int g, int f) {
    int r;
    if (!GPU) return O_FrameOutline(t, 0, src, w, h, dst, x, y, g, f);
    g_calls[OP_FROUT]++;
    ORIG(dst, O_FrameOutline(t, 0, src, w, h, dst, x, y, g, f));
    G_FrameOutline(src, w, h, dst, x, y, g, f, false);
    VerifyOp(OP_FROUT, dst, Rc(x, y, w, h, 1), ARGS("src %d g%d f%d %dx%d", src, g, f, w, h));
    return r;
}
static int TC H_FrameBlend(void* t, void*, int src, int w, int h, int dst, int x, int y, int g, int f) {
    int r;
    if (!GPU) return O_FrameBlend(t, 0, src, w, h, dst, x, y, g, f);
    g_calls[OP_FRBLEND]++;
    ORIG(dst, O_FrameBlend(t, 0, src, w, h, dst, x, y, g, f));
    G_FrameOutline(src, w, h, dst, x, y, g, f, true);
    VerifyOp(OP_FRBLEND, dst, Rc(x, y, w, h, 1), ARGS("src %d g%d f%d %dx%d", src, g, f, w, h));
    return r;
}
static int TC H_Sprite8Red(void* t, void*, int img, int g, int f, int dst, int x, int y, int clip, int pal) {
    int r;
    if (!GPU) return O_Sprite8Red(t, 0, img, g, f, dst, x, y, clip, pal);
    g_calls[OP_SPR8R]++;
    ORIG(dst, O_Sprite8Red(t, 0, img, g, f, dst, x, y, clip, pal));
    G_SpriteRed(img, g, f, dst, x, y, (RECT*)clip);
    VerifyOp(OP_SPR8R, dst, SpriteRect(img, g, f, x, y, (RECT*)clip), ARGS("img %d g%d f%d", img, g, f));
    return r;
}
static int TC H_Gray(void* t, void*, int rect, int dst, int clip) {
    int r;
    if (!GPU) return O_Gray(t, 0, rect, dst, clip);
    g_calls[OP_GRAY]++;
    ORIG(dst, O_Gray(t, 0, rect, dst, clip));
    G_Gray((RECT*)rect, dst, (RECT*)clip);
    VerifyOp(OP_GRAY, dst, RcIncl((RECT*)rect, (RECT*)clip), "");
    return r;
}
static int TC H_Panel(void* t, void*, int rect, int dst, int clip) {
    int r;
    if (!GPU) return O_Panel(t, 0, rect, dst, clip);
    g_calls[OP_PANEL]++;
    ORIG(dst, O_Panel(t, 0, rect, dst, clip));
    G_Panel((RECT*)rect, dst, (RECT*)clip);
    VerifyOp(OP_PANEL, dst, RcIncl((RECT*)rect, NULL), "");
    return r;
}
static int TC H_Floor(void* t, void*, int toBg) {
    int r;
    if (!GPU) return O_Floor(t, 0, toBg);
    g_calls[OP_FLOOR]++;
    bool skip = SkipCpu(toBg ? 2 : 1);
    if (skip) r = 0; else CALL(O_Floor(t, 0, toBg));
    G_Floor(toBg, skip);
    if (toBg) VerifyOp(OP_FLOOR, 2, Rc(0, 0, 640, 400), "bg");
    VerifyOp(OP_FLOOR, 1, Rc(0, 0, 640, 400), toBg ? "bg->back" : "back");
    return r;
}
static int TC H_Laser(void* t, void*, int col, int row, int mask) {
    int r;
    if (!GPU) return O_Laser(t, 0, col, row, mask);
    g_calls[OP_LASER]++;
    bool skip = SkipCpu(1);
    if (skip) r = 0; else CALL(O_Laser(t, 0, col, row, mask));
    G_Laser(t, col, row, mask & 0xff, false, skip);
    VerifyOp(OP_LASER, 1, Rc(col * 40, row * 40, 40, 40, 2), ARGS("cell %d,%d mask %X", col, row, mask & 0xff));
    return r;
}
static int TC H_Laser2(void* t, void*, int col, int row) {
    int r;
    if (!GPU) return O_Laser2(t, 0, col, row);
    g_calls[OP_LASER2]++;
    bool skip = SkipCpu(1);
    if (skip) r = 0; else CALL(O_Laser2(t, 0, col, row));
    G_Laser(t, col, row, 0, true, skip);
    VerifyOp(OP_LASER2, 1, Rc(col * 40, row * 40, 40, 40, 2), ARGS("cell %d,%d", col, row));
    return r;
}
// 00410630 smooths the GDI text pixels of the CPU image; the GPU text is already anti-aliased by DirectWrite
static int TC H_TextAA(void* t, void*, int rect, int color, int dst, int clip) {
    int r;
    if (!GPU) return O_TextAA(t, 0, rect, color, dst, clip);
    if (SkipCpu(dst)) return 0;
    if (!g_cfgVerify) return CALL(O_TextAA(t, 0, rect, color, dst, clip));
    MIRROR(OP_AA, dst, RcIncl((RECT*)rect, (RECT*)clip, 2), -1, RECT{}, false, O_TextAA(t, 0, rect, color, dst, clip));
    return r;
}

struct Detour { DWORD addr; int n; const char* sig; void* fn; void** orig; const char* name; };
#define D(a, n, sig, h, o) { a, n, sig, (void*)h, (void**)&o, #h }
static Detour g_det[] = {
    D(0x0040dab0, 5, "8B44240453", H_CopyRect, O_CopyRect),
    D(0x0040db60, 7, "83EC088B44240C", H_Blend50, O_Blend50),
    D(0x0040dcf0, 7, "83EC148B442418", H_Tile, O_Tile),
    D(0x0040de10, 5, "8B54240C53", H_BevelBtn, O_BevelBtn),
    D(0x0040e210, 5, "8B44240453", H_BevelFrame, O_BevelFrame),
    D(0x0040e4d0, 6, "8B4424048BD1", H_FrameOutline, O_FrameOutline),
    D(0x0040e600, 7, "83EC248B442428", H_FrameBlend, O_FrameBlend),
    D(0x0040e820, 7, "83EC248B442428", H_SpriteRle, O_SpriteRle),
    D(0x0040eb00, 7, "83EC2C8B442430", H_Sprite8, O_Sprite8),
    D(0x0040ee20, 7, "83EC3C8B442440", H_Sprite8Half, O_Sprite8Half),
    D(0x0040f320, 5, "83EC4C5355", H_Sprite8Alpha, O_Sprite8Alpha),
    D(0x0040f8a0, 7, "83EC288B44242C", H_Sprite8Red, O_Sprite8Red),
    D(0x0040fca0, 7, "83EC1C8B44242C", H_Rect, O_Rect),
    D(0x0040fec0, 7, "83EC208B442430", H_RectAlpha, O_RectAlpha),
    D(0x00410fd0, 7, "83EC148B44241C", H_Gray, O_Gray),
    D(0x004110b0, 5, "83EC145355", H_Panel, O_Panel),
    D(0x00411310, 5, "83EC2C5355", H_Floor, O_Floor),
    D(0x00413150, 5, "83EC688BC1", H_Laser, O_Laser),
    D(0x00415440, 7, "83EC588B542460", H_Laser2, O_Laser2),
    D(0x00404160, 8, "83EC20A1E8F94400", H_DirtyFlush, O_DirtyFlush),
    D(0x00404260, 8, "83EC30A120FA4400", H_DirtyErase, O_DirtyErase),
    D(0x00410630, 7, "83EC3C8B442444", H_TextAA, O_TextAA),
};

static bool SigOk(const Detour& d) {
    const BYTE* p = (const BYTE*)d.addr;
    for (int i = 0; i < d.n; i++) {
        unsigned v; sscanf(d.sig + i * 2, "%2x", &v);
        if (p[i] != v) return false;
    }
    return true;
}

void Gfx_Init() {
    if (!g_cfgGpu) { Log("GPU renderer disabled (Render/Gpu=0)"); return; }
    int count = sizeof g_det / sizeof g_det[0];
    g_ref = g_cfgVerify != 0;
    for (int i = 0; i < count; i++) {
        if (IsBadReadPtr((void*)g_det[i].addr, g_det[i].n) || !SigOk(g_det[i])) {
            Log("unexpected code at %08X (%s): not the supported Dweep.exe, GPU renderer off", g_det[i].addr, g_det[i].name);
            return;
        }
    }
    BYTE* pool = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pool) return;
    for (int i = 0; i < count; i++) {
        Detour& d = g_det[i];
        BYTE* a = (BYTE*)d.addr;
        BYTE* tr = pool + i * 32;
        memcpy(tr, a, d.n);
        tr[d.n] = 0xE9;
        *(DWORD*)(tr + d.n + 1) = (DWORD)(a + d.n) - (DWORD)(tr + d.n + 5);
        *d.orig = tr;
        DWORD old;
        VirtualProtect(a, d.n, PAGE_EXECUTE_READWRITE, &old);
        a[0] = 0xE9;
        *(DWORD*)(a + 1) = (DWORD)d.fn - (DWORD)(a + 5);
        for (int j = 5; j < d.n; j++) a[j] = 0x90;
        VirtualProtect(a, d.n, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    HMODULE exe = GetModuleHandleA(NULL);
    PatchIAT(exe, "GDI32.dll", "TextOutA", H_TextOutA);
    PatchIAT(exe, "GDI32.dll", "ExtTextOutA", H_ExtTextOutA);
    PatchIAT(exe, "USER32.dll", "DrawTextA", H_DrawTextA);
    g_hooked = true;
    Log("GPU renderer: %d blitters detoured", count);
}

// ------------------------------------------------------------------ swap chain + present
static void DropSwapChain() {
    Close();
    if (g_dc && g_target == g_scBmp && g_scBmp) { g_dc->SetTarget(NULL); g_target = NULL; }
    SafeRelease(g_scBmp);
    SafeRelease(g_sc);
}

static bool EnsureSwapChain(int cw, int ch) {
    if (g_sc) {
        DXGI_SWAP_CHAIN_DESC1 d;
        g_sc->GetDesc1(&d);
        if ((int)d.Width != cw || (int)d.Height != ch) {
            Close();
            if (g_target == g_scBmp) { g_dc->SetTarget(NULL); g_target = NULL; }
            SafeRelease(g_scBmp);
            HRESULT hr = g_sc->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) { Log("ResizeBuffers failed %08X", hr); DropSwapChain(); }
        }
    }
    if (!g_sc) {
        IDXGIDevice* dx = NULL; IDXGIAdapter* ad = NULL; IDXGIFactory2* fac = NULL;
        g_d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx);
        if (dx) dx->GetAdapter(&ad);
        if (ad) ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
        SafeRelease(ad); SafeRelease(dx);
        if (!fac) { Log("no IDXGIFactory2"); return false; }
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = cw; d.Height = ch;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = fac->CreateSwapChainForHwnd(g_d3d, g_hwnd, &d, NULL, NULL, &g_sc);
        if (FAILED(hr)) { d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; hr = fac->CreateSwapChainForHwnd(g_d3d, g_hwnd, &d, NULL, NULL, &g_sc); }
        if (FAILED(hr)) { d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; d.BufferCount = 1; hr = fac->CreateSwapChainForHwnd(g_d3d, g_hwnd, &d, NULL, NULL, &g_sc); }
        if (SUCCEEDED(hr)) fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        fac->Release();
        if (FAILED(hr)) { Log("CreateSwapChainForHwnd failed %08X", hr); return false; }
        Log("swap chain %dx%d effect %d", cw, ch, d.SwapEffect);
    }
    if (!g_scBmp) {
        IDXGISurface* s = NULL;
        g_sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&s);
        D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        HRESULT hr = s ? g_dc->CreateBitmapFromDxgiSurface(s, &p, &g_scBmp) : E_FAIL;
        SafeRelease(s);
        if (FAILED(hr)) { Log("CreateBitmapFromDxgiSurface failed %08X", hr); return false; }
    }
    return true;
}

void Gfx_Present(Surface* prim) {
    if (g_lost) { ReleaseAll(); g_lost = false; g_rebuild = true; }  // screen images are recreated from the CPU copies
    if (!g_hwnd || IsIconic(g_hwnd) || !EnsureDevice()) return;
    RECT c;
    GetClientRect(g_hwnd, &c);
    if (c.right <= 0 || c.bottom <= 0) return;
    ComputeDst(c.right, c.bottom);
    if (!EnsureSwapChain(c.right, c.bottom)) return;

    static unsigned frames;
    frames++;
    if (g_cfgVerify && g_hooked && frames % 30 == 0) VerifyFrame();
    if (frames % 1800 == 0) {
        char line[1024]; int o = 0;
        for (int i = 0; i < OP_N; i++)
            if (g_calls[i]) o += sprintf(line + o, " %s=%u%s", OPN[i], g_calls[i], g_bad[i] ? "!" : "");
        Log("frame %u calls:%s", frames, line);
    }

    if (g_rebuild && g_hooked && !g_ref) {    // after a device reset: background (kept on the CPU) -> back -> screen
        g_rebuild = false;
        RECT* pf = (RECT*)0x44f998;
        if (Scr(2) && Scr(1) && Scr(0)) {
            G_Copy(2, 0, 0, pf->right - pf->left + 1, pf->bottom - pf->top + 1, 1, 0, 0, 1.f);
            G_Copy(1, 0, 0, 640, 480, 0, 0, 0, 1.f);
        }
    }
    bool cpu = (g_showCpu && g_ref) || !g_hooked;
    ID2D1Bitmap* src = NULL;
    float sw, sh;
    if (cpu) {
        if (!prim || !prim->bits) return;
        int w = min(prim->w, 640), h = min(prim->h, 480);
        g_up.resize(w * h);
        for (int y = 0; y < h; y++) {
            const WORD* s = (const WORD*)(prim->bits + y * prim->pitch);
            for (int x = 0; x < w; x++) g_up[y * w + x] = g_L[s[x]];
        }
        Close();
        D2D1_RECT_U ur = D2D1::RectU(0, 0, w, h);
        g_stage->CopyFromMemory(&ur, g_up.data(), w * 4);
        src = g_stage; sw = (float)w; sh = (float)h;
    } else {
        src = Scr(0);
        if (!src) return;
        sw = (float)G[0].w * S; sh = (float)G[0].h * S;
    }
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    D2D1_INTERPOLATION_MODE im;
    if (fabs(dw - sw) < 0.5f && fabs(dh - sh) < 0.5f) im = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    else if (g_cfgFilter == 1) im = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    else if (dw < sw || dh < sh) im = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    else im = D2D1_INTERPOLATION_MODE_LINEAR;

    Target(g_scBmp);
    g_dc->Clear(D2D1::ColorF(0, 0, 0));
    D2D1_RECT_F d = D2D1::RectF((float)g_dst.left, (float)g_dst.top, (float)g_dst.right, (float)g_dst.bottom);
    D2D1_RECT_F s = D2D1::RectF(0, 0, sw, sh);
    g_dc->DrawBitmap(src, &d, 1.f, im, &s, NULL);
    Close();
    if (!g_sc) return;
    HRESULT hr = g_sc->Present(g_cfgVsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) DeviceLost();
}

void Gfx_SurfaceTouched(Surface* s) {
    if (!g_hooked || !g_dc || !g_ref) return;     // without the reference the CPU pixels are stale
    for (int k = 0; k < 2; k++)
        if (ImgSurf(k) == s && G[k].b) UploadRect(k, RECT{ 0, 0, s->w, s->h }, NULL, 0);
}

void Gfx_WindowChanged(HWND) { if (g_dc) DropSwapChain(); }
void Gfx_Resize() {}

void Gfx_Key(WPARAM vk) {
    if (vk == VK_F11 && !g_ref && g_hooked) Log("F11: no CPU image without Render/Verify");
    else if (vk == VK_F11) { g_showCpu = !g_showCpu; Log("showing %s image", g_showCpu ? "CPU" : "GPU"); }
    else if (vk == VK_F12 && g_dc) Screenshot();
    if (g_primary) g_primary->Show();
}
