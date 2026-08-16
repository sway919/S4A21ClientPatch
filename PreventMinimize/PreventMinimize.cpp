#include <windows.h>

// #37: minimize all windows
extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit()
{
    DWORD oldProt = 0;
    void* target = (void*)0x01203BBA;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProt))
        return FALSE;
    *(unsigned char*)target = 0xEB;
    VirtualProtect(target, 1, oldProt, &oldProt);
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
