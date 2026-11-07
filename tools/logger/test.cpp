// Smoke test: load out\DDraw.dll / out\DSOUND.dll explicitly and exercise a few calls.
#include <windows.h>
#include <ddraw.h>
#include <dsound.h>
#include <stdio.h>

int main() {
    HMODULE dd = LoadLibraryA("out\\DDraw.dll");
    HMODULE ds = LoadLibraryA("out\\DSOUND.dll");
    if (!dd || !ds) { printf("load failed\n"); return 1; }
    auto ddc = (HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*))GetProcAddress(dd, "DirectDrawCreate");
    LPDIRECTDRAW d = 0;
    HRESULT hr = ddc(NULL, &d, NULL);
    printf("DirectDrawCreate %08X\n", hr);
    hr = d->SetCooperativeLevel(GetConsoleWindow(), DDSCL_NORMAL);
    printf("SetCooperativeLevel %08X\n", hr);
    DDSURFACEDESC sd = { sizeof sd };
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    sd.dwWidth = 64; sd.dwHeight = 32;
    LPDIRECTDRAWSURFACE s1 = 0, s2 = 0;
    printf("CreateSurface %08X\n", d->CreateSurface(&sd, &s1, NULL));
    printf("CreateSurface %08X\n", d->CreateSurface(&sd, &s2, NULL));
    RECT r = { 0, 0, 16, 16 };
    for (int i = 0; i < 3; i++) printf("BltFast %08X\n", s1->BltFast(4, 4, s2, &r, DDBLTFAST_NOCOLORKEY));
    DDSURFACEDESC ld = { sizeof ld };
    hr = s1->Lock(NULL, &ld, DDLOCK_WAIT, NULL);
    printf("Lock %08X pitch=%d\n", hr, ld.lPitch);
    if (SUCCEEDED(hr)) s1->Unlock(NULL);
    LPDIRECTDRAWSURFACE7 s7 = 0;
    printf("QI7 %08X\n", s1->QueryInterface(IID_IDirectDrawSurface7, (void**)&s7));
    if (s7) s7->Release();
    s2->Release(); s1->Release(); d->Release();

    auto dsc = (HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN))GetProcAddress(ds, (LPCSTR)1);
    LPDIRECTSOUND snd = 0;
    hr = dsc(NULL, &snd, NULL);
    printf("DirectSoundCreate %08X\n", hr);
    if (snd) {
        printf("SetCoop %08X\n", snd->SetCooperativeLevel(GetConsoleWindow() ? GetConsoleWindow() : GetDesktopWindow(), DSSCL_PRIORITY));
        WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
        DSBUFFERDESC bd = { sizeof bd, DSBCAPS_CTRLVOLUME, 22050, 0, &wf };
        LPDIRECTSOUNDBUFFER b = 0;
        printf("CreateSoundBuffer %08X\n", snd->CreateSoundBuffer(&bd, &b, NULL));
        if (b) { b->SetVolume(-500); b->Release(); }
        snd->Release();
    }
    printf("done\n");
    return 0;
}
