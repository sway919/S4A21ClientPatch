#include "PluginLoader.h"

#include <string>
#include <vector>

namespace
{
constexpr wchar_t kConfigName[] = L"GameGaurd.ini";
constexpr char kInitializeName[] = "ClientPatchPluginInit";
constexpr int kMaxPlugins = 64;
using PluginInit = BOOL(*)();

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

std::wstring ResolvePath(const std::wstring& configuredPath,
    const std::wstring& moduleDirectory)
{
    const bool absolute =
        (configuredPath.size() >= 2 && configuredPath[1] == L':') ||
        (configuredPath.size() >= 2 && configuredPath[0] == L'\\' &&
            configuredPath[1] == L'\\') ||
        (!configuredPath.empty() &&
            (configuredPath[0] == L'\\' || configuredPath[0] == L'/'));
    return absolute ? configuredPath : moduleDirectory + L"\\" + configuredPath;
}

std::wstring FileName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
}

namespace plugin_loader
{
void LoadConfiguredPlugins(HMODULE module)
{
    const std::wstring directory = ModuleDirectory(module);
    const std::wstring iniPath = directory + L"\\" + kConfigName;
    std::vector<wchar_t> entries(32768, L'\0');
    if (GetPrivateProfileSectionW(L"Plugins", entries.data(),
        static_cast<DWORD>(entries.size()), iniPath.c_str()) == 0)
        return;

    int pluginCount = 0;
    for (const wchar_t* entry = entries.data();
        *entry && pluginCount < kMaxPlugins;
        entry += wcslen(entry) + 1)
    {
        const wchar_t* separator = wcschr(entry, L'=');
        if (!separator || !separator[1])
            continue;

        ++pluginCount;
        const std::wstring configured(separator + 1);
        const std::wstring path = ResolvePath(configured, directory);
        HMODULE plugin = GetModuleHandleW(FileName(path).c_str());
        if (!plugin)
            plugin = LoadLibraryW(path.c_str());
        if (!plugin)
            continue;

        const auto initialize = reinterpret_cast<PluginInit>(
            GetProcAddress(plugin, kInitializeName));

        // Run ClientPatchPluginInit after DllMain.
        if (initialize)
            initialize();
    }
}
}
