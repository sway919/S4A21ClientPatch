#include "mem.h"
#include "iat_table.h"
#include "fake_txsafe.h"
#include <intrin.h>

//#pragma comment(lib, "psapi.lib")
//#pragma comment(lib, "user32.lib")

// ============================================================
// Exports
// ============================================================
extern "C" {
    __declspec(dllexport) void** CreateObj(char type) { return CreateObj_Impl(type); }
    __declspec(dllexport) void ClearRSInfo() {}
    __declspec(dllexport) void PutRSInfo() {}
    __declspec(dllexport) void Test() {}
    __declspec(dllexport) const char* ijlErrorStr(int e) { return "OK"; }
    __declspec(dllexport) void ijlFree(void* p) {}
    __declspec(dllexport) int ijlGetLibVersion() { return 0; }
    __declspec(dllexport) int ijlInit(void* p) { return 0; }
    __declspec(dllexport) int ijlRead(void* p, int f) { return 0; }
    __declspec(dllexport) int ijlWrite(void* p, int f) { return 0; }
}


// ============================================================
// Repair EXE IAT
// ============================================================
#define Log

static void RepairIAT()
{
    int ok = 0, fail = 0;
    HMODULE modCache[64] = {};
    const char* modNames[64] = {};
    int modCount = 0;

    for (int i = 0; i < _countof(g_iatTable); i++)
    {
        HMODULE hmod = NULL;
        for (int j = 0; j < modCount; j++)
        {
            if (_stricmp(modNames[j], g_iatTable[i].mod) == 0) { hmod = modCache[j]; break; }
        }
        if (!hmod)
        {
            hmod = GetModuleHandleA(g_iatTable[i].mod);
            if (!hmod) hmod = LoadLibraryA(g_iatTable[i].mod);
            if (hmod && modCount < 64) { modCache[modCount] = hmod; modNames[modCount] = g_iatTable[i].mod; modCount++; }
        }
        if (!hmod) { Log("[IAT] Module not found: %s", g_iatTable[i].mod); fail++; continue; }

        FARPROC func = GetProcAddress(hmod, g_iatTable[i].func);
        if (!func) { Log("[IAT] Func not found: %s!%s", g_iatTable[i].mod, g_iatTable[i].func); fail++; continue; }

        DWORD target = g_iatTable[i].addr;
        DWORD oldProt;
        VirtualProtect((void*)target, 4, PAGE_EXECUTE_READWRITE, &oldProt);
        *(DWORD*)target = (DWORD)func;
        VirtualProtect((void*)target, 4, oldProt, &oldProt);
        ok++;
    }
    Log("[IAT] Repair done: %d ok, %d fail (of %d)", ok, fail, _countof(g_iatTable));
}


// ============================================================
// HOOK ___security_init_cookie
// ============================================================
#define XCookie 0x02C865DE

void ExeCookie()
{
    *(uintptr_t*)_AddressOfReturnAddress() = XCookie;
    mem::patch(XCookie, { 0x8B, 0xFF, 0x55, 0x8B, 0xEC });

    RepairIAT();
    LoadLibraryA("GameGaurd.dll");
}

DWORD WINAPI ThreadMain(LPVOID lpParam)
{
    do
    {
        _mm_pause();
    } while (*(DWORD*)XCookie != 0x8B55FF8B);
    mem::callhook(XCookie, (uintptr_t)(ExeCookie));
    return 0;
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)ThreadMain, nullptr, NULL, nullptr);
    }
    return TRUE;
}
