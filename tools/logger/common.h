// Shared vtable-hook logging machinery for the phase-1 logging proxies (x86 only).
// Every COM method of a hooked interface is redirected through a 10-byte stub:
//   push id ; jmp Common
// Common saves the call's arguments, swaps the return address for CommonRet, and jumps to the
// original method. CommonRet logs the call with its result, hooks any interface the call created,
// and returns to the real caller.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

struct IfaceDef { const char* name; int kind; int count; const char* const* methods; };
struct SlotInfo { const IfaceDef* iface; int slot; int nargs; void* orig; volatile LONG calls; char name[40]; };

#define MAXID 6000
#define STUBSZ 10
#define LIMIT 150      // detailed log lines per method; later calls are only counted

static SlotInfo g_slot[MAXID];
static int g_nid;
static BYTE* g_stubs;
static void* g_seen[512];
static int g_nseen;
static FILE* g_log;
static CRITICAL_SECTION g_cs;
static DWORD g_t0, g_lastSummary;

struct Buf { char* p; char* e; };
static void Ap(Buf& b, const char* f, ...) {
    if (b.p >= b.e) return;
    va_list v; va_start(v, f);
    int n = _vsnprintf(b.p, b.e - b.p, f, v);
    va_end(v);
    if (n < 0) { b.p = b.e; *(b.e - 1) = 0; } else b.p += n;
}

static void Log(const char* f, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    DWORD t = GetTickCount() - g_t0;
    fprintf(g_log, "%6u.%03u T%-5u ", t / 1000, t % 1000, GetCurrentThreadId());
    va_list v; va_start(v, f); vfprintf(g_log, f, v); va_end(v);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void LogOpen(const char* fname) {
    InitializeCriticalSection(&g_cs);
    g_t0 = g_lastSummary = GetTickCount();
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\');
    strcpy(s ? s + 1 : path, fname);
    g_log = fopen(path, "w");
    g_stubs = (BYTE*)VirtualAlloc(NULL, MAXID * STUBSZ, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    Log("log start, exe=%s", exe);
}

static void Summary(const char* why) {
    Log("---- call counts (%s) ----", why);
    for (int i = 0; i < g_nid; i++)
        if (g_slot[i].calls)
            Log("  %-26s %-24s %8ld", g_slot[i].iface->name, g_slot[i].name, g_slot[i].calls);
}

static void GuidStr(const GUID* g, char* out) {
    if (!g) { strcpy(out, "NULL"); return; }
    sprintf(out, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

extern "C" void Common();
extern "C" void CommonRet();

static void HookVtbl(void* obj, const IfaceDef* def) {
    if (!obj || !def) return;
    void** vt = *(void***)obj;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nseen; i++)
        if (g_seen[i] == vt) { LeaveCriticalSection(&g_cs); return; }
    if (g_nseen >= 512 || g_nid + def->count > MAXID) { LeaveCriticalSection(&g_cs); Log("!! hook table full"); return; }
    g_seen[g_nseen++] = vt;
    DWORD old;
    VirtualProtect(vt, def->count * 4, PAGE_READWRITE, &old);
    for (int i = 0; i < def->count; i++) {
        int id = g_nid++;
        SlotInfo& s = g_slot[id];
        s.iface = def; s.slot = i; s.orig = vt[i]; s.calls = 0;
        const char* m = def->methods[i];
        const char* sl = strchr(m, '/');
        size_t n = sl ? (size_t)(sl - m) : strlen(m);
        if (n > 39) n = 39;
        memcpy(s.name, m, n); s.name[n] = 0;
        s.nargs = sl ? atoi(sl + 1) : 0;
        BYTE* p = g_stubs + id * STUBSZ;
        p[0] = 0x68; *(DWORD*)(p + 1) = id;                          // push id
        p[5] = 0xE9; *(DWORD*)(p + 6) = (DWORD)&Common - (DWORD)(p + 10);  // jmp Common
        vt[i] = p;
    }
    VirtualProtect(vt, def->count * 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_stubs, MAXID * STUBSZ);
    LeaveCriticalSection(&g_cs);
    Log("hooked %s obj=%p vtbl=%p (%d methods)", def->name, obj, vt, def->count);
}

// per-DLL hooks
static void AfterCall(SlotInfo& s, DWORD* a, DWORD hr);
static void Decode(SlotInfo& s, DWORD* a, DWORD hr, Buf& b);

struct Pend { DWORD id, ret; DWORD a[9]; };
__declspec(thread) static Pend t_stk[128];
__declspec(thread) static int t_sp;

extern "C" void* __cdecl OnEnter(DWORD* p) {   // p[0]=id p[1]=return address p[2]=this p[3..]=args
    SlotInfo& s = g_slot[p[0]];
    if (t_sp < 128) {
        Pend& e = t_stk[t_sp++];
        e.id = p[0]; e.ret = p[1];
        memcpy(e.a, p + 2, 9 * 4);
        p[1] = (DWORD)&CommonRet;
    }
    return s.orig;
}

extern "C" DWORD __cdecl OnLeave(DWORD hr) {
    DWORD err = GetLastError();
    Pend e = t_stk[--t_sp];
    SlotInfo& s = g_slot[e.id];
    LONG c = InterlockedIncrement(&s.calls);
    AfterCall(s, e.a, hr);
    if (c <= LIMIT) {
        char line[1600];
        Buf b = { line, line + sizeof line };
        Ap(b, "%*s%s::%s(%08X", t_sp * 2, "", s.iface->name, s.name, e.a[0]);
        for (int i = 1; i <= s.nargs && i < 9; i++) Ap(b, ", %X", e.a[i]);
        Ap(b, ") -> %08X", hr);
        Decode(s, e.a, hr, b);
        if (c == LIMIT) Ap(b, "   [further calls only counted]");
        Log("%s", line);
    }
    DWORD now = GetTickCount();
    if (now - g_lastSummary > 60000) { g_lastSummary = now; Summary("periodic"); }
    SetLastError(err);
    return e.ret;
}

extern "C" __declspec(naked) void Common() {
    __asm {
        pushad
        lea eax, [esp + 32]
        push eax
        call OnEnter
        add esp, 4
        mov [esp + 28], eax      // saved eax slot <- original method
        popad
        add esp, 4               // drop id; stack is now exactly the caller's call frame
        jmp eax
    }
}

extern "C" __declspec(naked) void CommonRet() {
    __asm {
        push eax                 // placeholder for the real return address
        pushad
        push eax
        call OnLeave
        add esp, 4
        mov [esp + 32], eax
        popad                    // eax = method result again
        ret
    }
}
