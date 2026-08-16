#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "InlineHook.h"

namespace
{
using CreateMutexWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using FindWindowWFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
using FindWindowAFn = HWND(WINAPI*)(LPCSTR, LPCSTR);

CreateMutexWFn g_originalCreateMutexW = nullptr;
FindWindowWFn g_originalFindWindowW = nullptr;
FindWindowAFn g_originalFindWindowA = nullptr;
bool g_enabled = true;
bool g_debug = false;
std::wstring g_moduleDirectory;

void Log(const wchar_t* format, ...)
{
    if (!g_debug)
        return;

    wchar_t message[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);

    FILE* file = nullptr;
    const std::wstring path = g_moduleDirectory + L"\\MultiInstance.log";
    _wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8");
    if (file)
    {
        fwprintf(file, L"%s\n", message);
        fclose(file);
    }
    OutputDebugStringW(message);
}

bool IsTargetMutex(LPCWSTR name)
{
    return name != nullptr &&
        (_wcsicmp(name, L"dbefeuate_ccen_khxfor_lcar_blr") == 0 ||
            _wcsicmp(name, L"NeopleDNFClient") == 0);
}

bool IsForeignDnfWindow(HWND window)
{
    if (!window)
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;

    wchar_t path[MAX_PATH] = {};
    DWORD length = _countof(path);
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!ok)
        return false;

    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash ? slash + 1 : path;
    return _wcsicmp(name, L"DNF.exe") == 0;
}

HWND HideForeignDnf(HWND window)
{
    if (!IsForeignDnfWindow(window))
        return window;
    Log(L"[多开] 隐藏其他实例窗口 %p", window);
    return nullptr;
}

HANDLE WINAPI ProxyCreateMutexW(
    LPSECURITY_ATTRIBUTES attributes,
    BOOL initialOwner,
    LPCWSTR name)
{
    if (!g_originalCreateMutexW)
        return nullptr;
    if (!IsTargetMutex(name))
        return g_originalCreateMutexW(attributes, initialOwner, name);

    wchar_t uniqueName[128] = {};
    if (swprintf_s(uniqueName, _countof(uniqueName), L"%ls_%lu", name,
        static_cast<unsigned long>(GetCurrentProcessId())) < 0)
    {
        return g_originalCreateMutexW(attributes, initialOwner, name);
    }

    Log(L"[多开] 互斥体 %s 重命名为 %s", name, uniqueName);
    return g_originalCreateMutexW(attributes, initialOwner, uniqueName);
}

HWND WINAPI ProxyFindWindowW(LPCWSTR className, LPCWSTR windowName)
{
    if (!g_originalFindWindowW)
        return nullptr;
    return HideForeignDnf(g_originalFindWindowW(className, windowName));
}

HWND WINAPI ProxyFindWindowA(LPCSTR className, LPCSTR windowName)
{
    if (!g_originalFindWindowA)
        return nullptr;
    return HideForeignDnf(g_originalFindWindowA(className, windowName));
}

template <typename Fn>
bool InstallApiHook(const wchar_t* moduleName, const char* functionName,
    void* detour, Fn* original)
{
    HMODULE module = GetModuleHandleW(moduleName);
    if (!module)
        return false;

    FARPROC address = GetProcAddress(module, functionName);
    if (!address)
        return false;

    multi_instance::InlineHook hook;
    if (!hook.Install(reinterpret_cast<void*>(address), detour,
        reinterpret_cast<void**>(original)))
    {
        Log(L"[多开] Hook %hs 失败", functionName);
        return false;
    }
    return true;
}

bool InstallHooks()
{
    if (!InstallApiHook(L"kernel32.dll", "CreateMutexW",
        reinterpret_cast<void*>(&ProxyCreateMutexW), &g_originalCreateMutexW))
        return false;
    if (!InstallApiHook(L"user32.dll", "FindWindowW",
        reinterpret_cast<void*>(&ProxyFindWindowW), &g_originalFindWindowW))
        return false;
    InstallApiHook(L"user32.dll", "FindWindowA",
        reinterpret_cast<void*>(&ProxyFindWindowA), &g_originalFindWindowA);
    Log(L"[多开] 已 Hook CreateMutexW / FindWindow");
    return true;
}

void LoadConfig()
{
    const std::wstring path = g_moduleDirectory + L"\\MultiInstance.ini";
    g_enabled = GetPrivateProfileIntW(
        L"MultiInstance", L"Enabled", 1, path.c_str()) != 0;
    g_debug = GetPrivateProfileIntW(
        L"MultiInstance", L"Debug", 0, path.c_str()) != 0;
}

std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path))
        return L".";

    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}
}

extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit()
{
    LoadConfig();
    if (!g_enabled)
        return TRUE;
    return InstallHooks() ? TRUE : FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_moduleDirectory = ModuleDirectory(module);
    }
    return TRUE;
}
