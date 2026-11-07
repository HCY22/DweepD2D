// Keeps Dweep Gold's settings and level progress in DweepSave.ini next to the game instead of the registry.
//
// The game stores everything under HKLM\Software\Dexterity Software\Dweep (Windows redirects that to the per-user
// VirtualStore for a non-admin process) and tries to register the .dwp file type under HKEY_CLASSES_ROOT on every
// start. Its ADVAPI32 imports are redirected here:
//   - the Dweep key becomes a fake handle backed by the ini file: [Options] holds the DWORD values (Sound, Music,
//     Hints...), [Data] the string values (level progress strings such as "Original Game", and Path)
//   - every other key behaves as if access were denied, so nothing is ever written to the registry
// If DweepSave.ini is missing a new empty one is created; the game then fills in its defaults (nothing is read from
// the registry).
//
// Usage: #include this in one .cpp and call RegSave_Install(gameDir, PatchIAT) from DllMain; link advapi32.lib.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace regsave {

static char g_ini[MAX_PATH];
static void (*g_log)(const char*, ...);
static const HKEY FAKE = (HKEY)(ULONG_PTR)0x7D5E0001;
static const char* KEY = "Software\\Dexterity Software\\Dweep";
static const char* NUM = "Options";
static const char* STR = "Data";
static const char* MISSING = "\x01missing";

static bool IsSaveKey(HKEY root, LPCSTR sub) {
    if (root != HKEY_LOCAL_MACHINE || !sub) return false;
    while (*sub == '\\') sub++;
    size_t n = strlen(KEY);
    return !_strnicmp(sub, KEY, n) && (sub[n] == 0 || (sub[n] == '\\' && sub[n + 1] == 0));
}

static bool Read(const char* sec, LPCSTR name, char* buf, int size) {
    GetPrivateProfileStringA(sec, name ? name : "", MISSING, buf, size, g_ini);
    return strcmp(buf, MISSING) != 0;
}

static LSTATUS WINAPI H_RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
                                        const LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp) {
    if (!out) return ERROR_INVALID_PARAMETER;
    if (IsSaveKey(root, sub)) {
        *out = FAKE;
        if (disp) *disp = GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
        return ERROR_SUCCESS;
    }
    *out = NULL;                         // file association etc.: never written
    return ERROR_ACCESS_DENIED;
}

static LSTATUS WINAPI H_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    if (!out) return ERROR_INVALID_PARAMETER;
    if (IsSaveKey(root, sub)) { *out = FAKE; return ERROR_SUCCESS; }
    return RegOpenKeyExA(root, sub, opt, sam & ~(KEY_SET_VALUE | KEY_CREATE_SUB_KEY), out);    // read-only
}

static LSTATUS WINAPI H_RegCloseKey(HKEY k) {
    if (k == FAKE) return ERROR_SUCCESS;
    return RegCloseKey(k);
}

static LSTATUS WINAPI H_RegSetValueExA(HKEY k, LPCSTR name, DWORD res, DWORD type, const BYTE* data, DWORD cb) {
    if (k != FAKE) return ERROR_ACCESS_DENIED;
    const char* n = name ? name : "";
    char v[4096];
    if (type == REG_DWORD && data && cb >= 4) {
        sprintf(v, "%lu", *(const DWORD*)data);
        WritePrivateProfileStringA(STR, n, NULL, g_ini);
        WritePrivateProfileStringA(NUM, n, v, g_ini);
    } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        DWORD len = data ? min(cb, (DWORD)sizeof v - 1) : 0;
        memcpy(v, data, len);
        v[len] = 0;
        WritePrivateProfileStringA(NUM, n, NULL, g_ini);
        WritePrivateProfileStringA(STR, n, v, g_ini);
    } else {
        if (g_log) g_log("registry value \"%s\" of type %lu not saved", n, type);
        return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI H_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb) {
    if (k != FAKE) return RegQueryValueExA(k, name, res, type, data, cb);
    char v[4096];
    if (Read(NUM, name, v, sizeof v)) {
        DWORD x = strtoul(v, NULL, 10);
        if (type) *type = REG_DWORD;
        if (data) {
            if (!cb || *cb < 4) { if (cb) *cb = 4; return ERROR_MORE_DATA; }
            memcpy(data, &x, 4);
        }
        if (cb) *cb = 4;
        return ERROR_SUCCESS;
    }
    if (Read(STR, name, v, sizeof v)) {
        DWORD need = (DWORD)strlen(v) + 1;
        if (type) *type = REG_SZ;
        if (data) {
            if (!cb || *cb < need) { if (cb) *cb = need; return ERROR_MORE_DATA; }
            memcpy(data, v, need);
        }
        if (cb) *cb = need;
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

} // namespace regsave

static void RegSave_Install(const char* dir, void (*patch)(HMODULE, const char*, const char*, void*),
                            void (*log)(const char*, ...)) {
    using namespace regsave;
    g_log = log;
    sprintf(g_ini, "%sDweepSave.ini", dir);
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileA(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            const char hdr[] = "; Dweep Gold settings and level progress (stored here instead of the registry)\r\n";
            DWORD w; WriteFile(f, hdr, sizeof hdr - 1, &w, NULL); CloseHandle(f);
        }
        if (g_log) g_log("DweepSave.ini not found: created a new one (progress starts empty)");
    }
    HMODULE exe = GetModuleHandleA(NULL);
    patch(exe, "ADVAPI32.dll", "RegCreateKeyExA", (void*)H_RegCreateKeyExA);
    patch(exe, "ADVAPI32.dll", "RegOpenKeyExA", (void*)H_RegOpenKeyExA);
    patch(exe, "ADVAPI32.dll", "RegCloseKey", (void*)H_RegCloseKey);
    patch(exe, "ADVAPI32.dll", "RegSetValueExA", (void*)H_RegSetValueExA);
    patch(exe, "ADVAPI32.dll", "RegQueryValueExA", (void*)H_RegQueryValueExA);
}
