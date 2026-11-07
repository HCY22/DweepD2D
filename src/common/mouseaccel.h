// Mouse speed for the scaled-up game screen.
//
// The original game switched the monitor to 640x480, so one step of the mouse moved the cursor one game pixel.
// Scaled to a big window or monitor the same hand movement covers far fewer game pixels and the mouse feels slow.
// With [Input] MouseSpeed the game cursor is kept as its own (fractional) position that moves `speed` times faster
// than the plain mapping, and the (hidden) Windows cursor is moved along with it so clicks, ClipCursor and leaving the
// window keep working:
//   auto = like the original full screen: one mouse step = one game pixel on both axes
//   1    = plain mapping (game cursor exactly under the Windows cursor, the old behaviour)
//   x    = that many times the plain mapping, e.g. 1.5
//
// Include after these are defined: g_hwnd, g_modeSet, g_dst, g_mw, g_mh, ClientToGame, GameToClient,
// R_GetCursorPos, R_SetCursorPos. Call MouseAccel_LoadConfig(ini) from DllMain.
#pragma once
#include <windows.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

static double g_cfgMouseSpeed = 0;           // 0 = auto
static double g_mx = -1, g_my = -1;          // game cursor in game pixels (fractional)
static POINT g_warp = { LONG_MIN, LONG_MIN };  // where the Windows cursor was last seen / put (screen pixels)

static void MouseAccel_LoadConfig(const char* ini) {
    char v[64];
    GetPrivateProfileStringA("Input", "MouseSpeed", "auto", v, sizeof v, ini);
    g_cfgMouseSpeed = _stricmp(v, "auto") ? atof(v) : 0;
    if (g_cfgMouseSpeed < 0) g_cfgMouseSpeed = 0;
}

// game pixels per screen pixel of real cursor movement, per axis.
// auto: exactly 1 on both axes (also with Aspect=stretch, where the plain mapping differs per axis)
static void MouseFactors(double& fx, double& fy) {
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    if (dw <= 0 || dh <= 0) { fx = fy = 1; return; }
    if (g_cfgMouseSpeed <= 0) { fx = fy = 1; return; }
    double m = max(0.1, min(16.0, g_cfgMouseSpeed));
    fx = m * g_mw / dw; fy = m * g_mh / dh;
}

static void MouseWarp() {
    POINT t = { (LONG)g_mx, (LONG)g_my };
    GameToClient(t.x, t.y);
    ClientToScreen(g_hwnd, &t);
    POINT s;
    if (R_GetCursorPos(&s) && (s.x != t.x || s.y != t.y)) R_SetCursorPos(t.x, t.y);
    g_warp = t;
}

// current game-space cursor position (call only while g_modeSet && g_hwnd)
static POINT MouseGame() {
    POINT s = { 0, 0 };
    R_GetCursorPos(&s);
    POINT c = s;
    ScreenToClient(g_hwnd, &c);
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    double fx, fy;
    MouseFactors(fx, fy);
    int dw = g_dst.right - g_dst.left, dh = g_dst.bottom - g_dst.top;
    bool plain = dw > 0 && dh > 0 && fabs(fx * dw / g_mw - 1) < 0.01 && fabs(fy * dh / g_mh - 1) < 0.01;
    bool accel = !plain && PtInRect(&cr, c) && GetForegroundWindow() == g_hwnd;
    if (!accel || g_mx < 0 || g_warp.x == LONG_MIN) {
        // plain mapping: also how the game cursor is picked up again when the Windows cursor comes back in
        LONG x = c.x, y = c.y;
        ClientToGame(x, y);
        g_mx = x + 0.5; g_my = y + 0.5;
        g_warp = accel ? s : POINT{ LONG_MIN, LONG_MIN };
        return { x, y };
    }
    int dx = s.x - g_warp.x, dy = s.y - g_warp.y;
    if (dx || dy) {
       g_mx = max(0.0, min(g_mw - 0.001, g_mx + dx * fx));
        g_my = max(0.0, min(g_mh - 0.001, g_my + dy * fy));
        MouseWarp();
    }
    return { (LONG)g_mx, (LONG)g_my };
}

// the game moved the cursor itself
static void MouseSetGame(int x, int y) {
    g_mx = max(0, min(g_mw - 1, x)) + 0.5;
    g_my = max(0, min(g_mh - 1, y)) + 0.5;
    MouseWarp();
}
