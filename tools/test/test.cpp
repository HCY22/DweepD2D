// Smoke test for the Direct2D/XAudio2 DDraw.dll + DSOUND.dll (run from a folder holding both DLLs and a DweepD2D.ini).
#include <windows.h>
#include <ddraw.h>
#include <dsound.h>
#include <stdio.h>
#include <math.h>

int main() {
    HMODULE dd = LoadLibraryA("DDraw.dll"), ds = LoadLibraryA("DSOUND.dll");
    char p1[MAX_PATH] = "", p2[MAX_PATH] = "";
    GetModuleFileNameA(dd, p1, MAX_PATH); GetModuleFileNameA(ds, p2, MAX_PATH);
    printf("loaded %s\n       %s\n", p1, p2);
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(0); wc.lpszClassName = "t";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "t", "D2D test", WS_POPUP | WS_VISIBLE, 0, 0, 300, 300, 0, 0, 0, 0);
    auto ddc = (HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*))GetProcAddress(dd, "DirectDrawCreate");
    LPDIRECTDRAW d; printf("DirectDrawCreate %08X\n", ddc(0, &d, 0));
    printf("SetCooperativeLevel %08X\n", d->SetCooperativeLevel(h, 0x13));
    printf("SetDisplayMode %08X\n", d->SetDisplayMode(640, 480, 16));
    DDSURFACEDESC sd = { sizeof sd }; sd.dwFlags = DDSD_CAPS; sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    LPDIRECTDRAWSURFACE prim, back;
    printf("Create primary %08X\n", d->CreateSurface(&sd, &prim, 0));
    sd.dwFlags = 7; sd.dwWidth = 640; sd.dwHeight = 480; sd.ddsCaps.dwCaps = 0x840;
    printf("Create back %08X\n", d->CreateSurface(&sd, &back, 0));
    DDSURFACEDESC a = { sizeof a }, b = { sizeof b };
    back->Lock(0, &a, 1, 0);
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++)
        ((WORD*)((BYTE*)a.lpSurface + y * a.lPitch))[x] = (x < 320) ? 0xF800 : 0x001F;   // left red, right blue
    back->Unlock(0);
    HDC dc; back->GetDC(&dc); TextOutA(dc, 10, 10, "GDI text on back buffer", 23); back->ReleaseDC(dc);
    back->Lock(0, &a, 1, 0); prim->Lock(0, &b, 1, 0);
    for (int y = 0; y < 480; y++) memcpy((BYTE*)b.lpSurface + y * b.lPitch, (BYTE*)a.lpSurface + y * a.lPitch, 1280);
    prim->Unlock(0); back->Unlock(0);
    SetForegroundWindow(h);
    prim->Unlock(0);   // present again after coming to the front
    RECT wr;
    ((BOOL(WINAPI*)(HWND, LPRECT))GetProcAddress(GetModuleHandleA("user32.dll"), "GetWindowRect"))(h, &wr);
    printf("window now (%d,%d)-(%d,%d)\n", wr.left, wr.top, wr.right, wr.bottom);
    Sleep(300);
    HDC sdc = GetDC(0);
    POINT L = { wr.left + (wr.right - wr.left) / 4, (wr.top + wr.bottom) / 2 }, R = { wr.left + 3 * (wr.right - wr.left) / 4, L.y };
    printf("screen pixel left=%06X right=%06X (expect 0000FF / FF0000 as COLORREF)\n", GetPixel(sdc, L.x, L.y), GetPixel(sdc, R.x, R.y));
    ReleaseDC(0, sdc);

    // sound: one second 440 Hz, 16-bit mono 22050
    auto dsc = (HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN))GetProcAddress(ds, (LPCSTR)1);
    LPDIRECTSOUND snd; printf("DirectSoundCreate %08X\n", dsc(0, &snd, 0));
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    DSBUFFERDESC bd = { sizeof bd, 0x1000A, 22050, 0, &wf };
    LPDIRECTSOUNDBUFFER sb, dup; printf("CreateSoundBuffer %08X\n", snd->CreateSoundBuffer(&bd, &sb, 0));
    void* p; DWORD n; sb->Lock(0, 22050, &p, &n, 0, 0, 0);
    for (DWORD i = 0; i < n / 2; i++) ((short*)p)[i] = (short)(6000 * sin(i * 2 * 3.14159 * 440 / 22050));
    sb->Unlock(p, n, 0, 0);
    printf("Duplicate %08X\n", snd->DuplicateSoundBuffer(sb, &dup));
    DWORD st;
    printf("Play %08X\n", dup->Play(0, 0, 0));
    Sleep(200); dup->GetStatus(&st); DWORD pc; dup->GetCurrentPosition(&pc, 0);
    printf("status after 200ms=%u pos=%u\n", st, pc);
    Sleep(600);
    dup->Stop(); dup->SetCurrentPosition(0); dup->GetStatus(&st); printf("after Stop status=%u\n", st);
    dup->Play(0, 0, 0); Sleep(1300); dup->GetStatus(&st); printf("after end status=%u\n", st);
    dup->Release(); sb->Release(); snd->Release();
    prim->Release(); back->Release(); d->Release();
    DestroyWindow(h);
    printf("done\n");
    return 0;
}
