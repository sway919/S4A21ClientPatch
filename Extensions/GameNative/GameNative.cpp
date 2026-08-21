#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "GameNativeApi.h"
#include "GameNativeClip.h"
#include "GameNativeHook.h"
#include "GameNativeLog.h"
#include "GameNativeNotice.h"

namespace
{
constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t kXuiLoaderAddress = 0x027E0540;
constexpr uintptr_t kXuiLoaderSehAddress = 0x02E9EBD0;
constexpr uintptr_t kXuiParserAddress = 0x02C45C50;
constexpr uintptr_t kWindowFactoryAddress = 0x023BD910;
constexpr size_t kHookLength = 5;
constexpr ULONGLONG kMaxXuiBytes = 16ull * 1024ull * 1024ull;
constexpr wchar_t kConfigFileName[] = L"GameNative.ini";
constexpr wchar_t kLogFileName[] = L"GameNative.log";
constexpr wchar_t kBuildTag[] = L"game-native";
constexpr int kAssignableWindowIds[] = {
    18, 21, 48, 75, 99,
    159, 170, 223, 272, 284, 287, 309, 314, 315, 316, 323, 324, 362,
    389, 414, 523, 536, 569, 588, 589, 598, 599, 612, 614, 617, 618, 649,
    772,
    835,
    838,
};
constexpr int kMaxAfterWindowId = 838;
constexpr size_t kMaxWindowIdOwnerLength = 128;

using XuiLoaderFn = bool(__cdecl*)(const wchar_t* virtualPath,
    void* parser);
using XuiParserFn = void*(__thiscall*)(void* parser, const char* text,
    void* encodingState, int encodingMode);
using WindowFactoryFn = void* (__cdecl*)(int windowId, void* manager);

struct MountEntry
{
    std::wstring virtualPath;
    std::wstring diskPath;
    DWORD flags = 0;
};

struct WindowIdReservation
{
    std::wstring owner;
    int afterWindowId = -1;
    std::vector<int> windowIds;
    ClientPatchWindowFactoryFn factory = nullptr;
    void* factoryContext = nullptr;
};

class SharedSrwLock
{
public:
    explicit SharedSrwLock(SRWLOCK& lock) : lock_(&lock)
    {
        AcquireSRWLockShared(lock_);
    }
    ~SharedSrwLock()
    {
        ReleaseSRWLockShared(lock_);
    }

    SharedSrwLock(const SharedSrwLock&) = delete;
    SharedSrwLock& operator=(const SharedSrwLock&) = delete;

private:
    SRWLOCK* lock_;
};

class ExclusiveSrwLock
{
public:
    explicit ExclusiveSrwLock(SRWLOCK& lock) : lock_(&lock)
    {
        AcquireSRWLockExclusive(lock_);
    }
    ~ExclusiveSrwLock()
    {
        ReleaseSRWLockExclusive(lock_);
    }

    ExclusiveSrwLock(const ExclusiveSrwLock&) = delete;
    ExclusiveSrwLock& operator=(const ExclusiveSrwLock&) = delete;

private:
    SRWLOCK* lock_;
};

std::wstring g_moduleDirectory;
std::wstring g_logPath;
bool g_debug = false;
SRWLOCK g_mountLock = SRWLOCK_INIT;
SRWLOCK g_windowIdLock = SRWLOCK_INIT;
SRWLOCK g_logLock = SRWLOCK_INIT;
std::vector<MountEntry> g_mounts;
std::vector<WindowIdReservation> g_windowIdReservations;
XuiLoaderFn g_originalXuiLoader = nullptr;
WindowFactoryFn g_originalWindowFactory = nullptr;
LONG g_hookState = 0;

bool HookReady()
{
    return InterlockedCompareExchange(&g_hookState, 0, 0) == 1;
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

std::wstring NormalizeVirtualPath(const wchar_t* path)
{
    if (!path || !*path)
        return {};

    std::wstring normalized(path);
    std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
    while (!normalized.empty() && normalized.front() == L'/')
        normalized.erase(normalized.begin());
    for (wchar_t& value : normalized)
        value = static_cast<wchar_t>(towlower(value));
    return normalized;
}

bool IsValidVirtualXuiPath(const std::wstring& path)
{
    if (path.size() < 5 || path.compare(path.size() - 4, 4, L".xui") != 0 ||
        path.find(L':') != std::wstring::npos)
        return false;

    size_t segmentStart = 0;
    while (segmentStart <= path.size())
    {
        const size_t separator = path.find(L'/', segmentStart);
        const size_t segmentLength =
            (separator == std::wstring::npos ? path.size() : separator) -
                segmentStart;
        if (segmentLength == 0 ||
            (segmentLength == 2 && path.compare(segmentStart, 2, L"..") == 0))
            return false;
        if (separator == std::wstring::npos)
            break;
        segmentStart = separator + 1;
    }
    return true;
}

bool IsAbsoluteDiskPath(const wchar_t* path)
{
    if (!path || !*path)
        return false;
    return (path[0] == L'\\' && path[1] == L'\\') ||
        (path[0] != L'\0' && path[1] == L':' &&
            (path[2] == L'\\' || path[2] == L'/'));
}

bool ReadFileBytes(const std::wstring& path, std::vector<char>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = {};
    const bool sizeOk = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
        static_cast<ULONGLONG>(size.QuadPart) <= kMaxXuiBytes;
    if (!sizeOk)
    {
        CloseHandle(file);
        return false;
    }

    const size_t byteCount = static_cast<size_t>(size.QuadPart);
    bytes.assign(byteCount + 1, '\0');
    size_t readTotal = 0;
    while (readTotal < byteCount)
    {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            byteCount - readTotal, 1024 * 1024));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + readTotal, request, &read,
            nullptr) || read == 0)
        {
            CloseHandle(file);
            bytes.clear();
            return false;
        }
        readTotal += read;
    }
    CloseHandle(file);

    std::vector<char> normalized;
    normalized.reserve(byteCount + 1);
    for (size_t index = 0; index < byteCount; ++index)
    {
        if (bytes[index] == '\r')
        {
            normalized.push_back('\n');
            if (index + 1 < byteCount && bytes[index + 1] == '\n')
                ++index;
        }
        else
        {
            normalized.push_back(bytes[index]);
        }
    }
    normalized.push_back('\0');
    bytes.swap(normalized);
    return true;
}

bool FindMount(const wchar_t* virtualPath, MountEntry& result)
{
    const std::wstring normalizedPath = NormalizeVirtualPath(virtualPath);
    if (normalizedPath.empty())
        return false;

    SharedSrwLock lock(g_mountLock);
    const auto found = std::find_if(g_mounts.begin(), g_mounts.end(),
        [&normalizedPath](const MountEntry& entry) {
            return entry.virtualPath == normalizedPath;
        });
    const bool matched = found != g_mounts.end();
    if (matched)
        result = *found;
    return matched;
}

bool ParseGameNative(void* parser, const std::vector<char>& bytes)
{
    if (!parser || bytes.empty())
        return false;

    __try
    {
        auto** vtable = *reinterpret_cast<void***>(parser);
        if (!vtable || reinterpret_cast<uintptr_t>(vtable[2]) !=
            GameNativeClientAddress(kXuiParserAddress))
            return false;

        auto parse = reinterpret_cast<XuiParserFn>(vtable[2]);
        return parse(parser, bytes.data(), nullptr, 0) != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool __cdecl HookXuiLoader(const wchar_t* virtualPath, void* parser)
{
    bool strictMount = false;
    bool parserCalled = false;
    try
    {
        if (g_debug)
            GameNativeLog(L"[load] %s", virtualPath ? virtualPath : L"<null>");

        MountEntry mount;
        if (!FindMount(virtualPath, mount))
            return g_originalXuiLoader &&
                g_originalXuiLoader(virtualPath, parser);
        strictMount =
            (mount.flags & CLIENT_PATCH_GAME_NATIVE_MOUNT_STRICT) != 0;

        std::vector<char> bytes;
        if (!ReadFileBytes(mount.diskPath, bytes))
        {
            GameNativeLog(L"[mount] read failed: %s -> %s", virtualPath,
                mount.diskPath.c_str());
            if (strictMount)
                return false;
            return g_originalXuiLoader &&
                g_originalXuiLoader(virtualPath, parser);
        }

        parserCalled = true;
        if (!ParseGameNative(parser, bytes))
        {
            GameNativeLog(L"[mount] parse failed: %s -> %s", virtualPath,
                mount.diskPath.c_str());
            return false;
        }

        int tileX = 11;
        int tileY = 11;
        GameNativeExtractTileOffset(bytes.data(), &tileX, &tileY);
        GameNativeNoteTileOffset(tileX, tileY);
        GameNativeExtractClippedLayout(bytes.data());
        GameNativeLog(L"[mount] loaded: %s -> %s tile=%d,%d", virtualPath,
            mount.diskPath.c_str(), tileX, tileY);
        return true;
    }
    catch (...)
    {
        GameNativeLog(L"[mount] unexpected failure: %s",
            virtualPath ? virtualPath : L"<null>");
        if (strictMount || parserCalled)
            return false;
        return g_originalXuiLoader &&
            g_originalXuiLoader(virtualPath, parser);
    }
}

BOOL MountXui(LPCWSTR virtualPath, LPCWSTR diskPath, DWORD flags)
{
    if (!HookReady() ||
        (flags & ~CLIENT_PATCH_GAME_NATIVE_MOUNT_STRICT) != 0)
        return FALSE;

    try
    {
        const std::wstring normalizedPath = NormalizeVirtualPath(virtualPath);
        if (!IsValidVirtualXuiPath(normalizedPath) ||
            !IsAbsoluteDiskPath(diskPath))
            return FALSE;

        MountEntry entry;
        entry.virtualPath = normalizedPath;
        entry.diskPath = diskPath;
        entry.flags = flags;

        {
            ExclusiveSrwLock lock(g_mountLock);
            const auto found = std::find_if(g_mounts.begin(), g_mounts.end(),
                [&normalizedPath](const MountEntry& current) {
                    return current.virtualPath == normalizedPath;
                });
            if (found == g_mounts.end())
                g_mounts.push_back(entry);
            else
                *found = entry;
        }

        GameNativeLog(L"[mount] registered: %s -> %s", normalizedPath.c_str(),
            diskPath);
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

BOOL UnmountXui(LPCWSTR virtualPath)
{
    if (!HookReady())
        return FALSE;

    try
    {
        const std::wstring normalizedPath = NormalizeVirtualPath(virtualPath);
        if (normalizedPath.empty())
            return FALSE;

        ExclusiveSrwLock lock(g_mountLock);
        const auto found = std::find_if(g_mounts.begin(), g_mounts.end(),
            [&normalizedPath](const MountEntry& current) {
                return current.virtualPath == normalizedPath;
            });
        if (found == g_mounts.end())
            return FALSE;
        g_mounts.erase(found);
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

bool IsValidWindowIdOwner(const std::wstring& owner)
{
    if (owner.empty() || owner.size() > kMaxWindowIdOwnerLength)
        return false;
    return std::none_of(owner.begin(), owner.end(), [](wchar_t value) {
        return iswspace(value) || iswcntrl(value);
    });
}

bool OwnerEquals(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

int WindowIdTierEnd(INT afterWindowId)
{
    if (afterWindowId < CLIENT_PATCH_WINDOW_ID_EQUIPMENT)
        return CLIENT_PATCH_WINDOW_ID_EQUIPMENT;
    if (afterWindowId < CLIENT_PATCH_WINDOW_ID_MID_POPUP)
        return CLIENT_PATCH_WINDOW_ID_MID_POPUP;
    if (afterWindowId < CLIENT_PATCH_WINDOW_ID_GAME_MENU)
        return CLIENT_PATCH_WINDOW_ID_GAME_MENU;
    if (afterWindowId < CLIENT_PATCH_WINDOW_ID_ABOVE_MENU)
        return CLIENT_PATCH_WINDOW_ID_ABOVE_MENU;
    if (afterWindowId < CLIENT_PATCH_WINDOW_ID_PLUGIN_POPUP_TOP_ANCHOR)
        return CLIENT_PATCH_WINDOW_ID_PLUGIN_POPUP_TOP_ANCHOR;
    return 839;
}

BOOL ReserveWindowIds(LPCWSTR owner, DWORD count, INT afterWindowId,
    INT* windowIds)
{
    if (!HookReady() || !owner || !windowIds || count == 0 ||
        count > _countof(kAssignableWindowIds) || afterWindowId < -1 ||
        afterWindowId > kMaxAfterWindowId)
        return FALSE;

    try
    {
        const std::wstring ownerKey(owner);
        if (!IsValidWindowIdOwner(ownerKey))
            return FALSE;
        const int tierEnd = WindowIdTierEnd(afterWindowId);

        std::vector<int> result;
        result.reserve(count);
        {
            ExclusiveSrwLock lock(g_windowIdLock);
            const auto existing = std::find_if(g_windowIdReservations.begin(),
                g_windowIdReservations.end(), [&ownerKey](
                    const WindowIdReservation& reservation) {
                    return OwnerEquals(reservation.owner, ownerKey);
                });
            if (existing != g_windowIdReservations.end())
            {
                if (existing->afterWindowId != afterWindowId ||
                    existing->windowIds.size() != count)
                    return FALSE;
                result = existing->windowIds;
            }
            else
            {
                for (const int candidate : kAssignableWindowIds)
                {
                    if (candidate <= afterWindowId || candidate >= tierEnd)
                        continue;
                    const bool reserved = std::any_of(
                        g_windowIdReservations.begin(),
                        g_windowIdReservations.end(),
                        [candidate](const WindowIdReservation& reservation) {
                            return std::find(reservation.windowIds.begin(),
                                reservation.windowIds.end(), candidate) !=
                                reservation.windowIds.end();
                        });
                    if (!reserved)
                        result.push_back(candidate);
                    if (result.size() == count)
                        break;
                }
                if (result.size() != count)
                    return FALSE;

                WindowIdReservation reservation;
                reservation.owner = ownerKey;
                reservation.afterWindowId = afterWindowId;
                reservation.windowIds = result;
                g_windowIdReservations.push_back(std::move(reservation));
            }

            std::copy(result.begin(), result.end(), windowIds);
        }

        GameNativeLog(L"[window-id] reserved: owner=%s count=%lu after=%d "
            L"tierEnd=%d first=%d",
            ownerKey.c_str(), static_cast<unsigned long>(count),
            afterWindowId, tierEnd, result.front());
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

BOOL ReleaseWindowIds(LPCWSTR owner)
{
    if (!HookReady() || !owner)
        return FALSE;

    try
    {
        const std::wstring ownerKey(owner);
        if (!IsValidWindowIdOwner(ownerKey))
            return FALSE;

        {
            ExclusiveSrwLock lock(g_windowIdLock);
            const auto found = std::find_if(g_windowIdReservations.begin(),
                g_windowIdReservations.end(), [&ownerKey](
                    const WindowIdReservation& reservation) {
                    return OwnerEquals(reservation.owner, ownerKey);
                });
            if (found == g_windowIdReservations.end())
                return FALSE;
            if (found->factory)
                return FALSE;
            g_windowIdReservations.erase(found);
        }
        GameNativeLog(L"[window-id] released: owner=%s", ownerKey.c_str());
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

BOOL RegisterWindowFactory(LPCWSTR owner,
    ClientPatchWindowFactoryFn factory, void* context)
{
    if (!HookReady() || !owner || !factory)
        return FALSE;

    try
    {
        const std::wstring ownerKey(owner);
        if (!IsValidWindowIdOwner(ownerKey))
            return FALSE;

        ExclusiveSrwLock lock(g_windowIdLock);
        const auto found = std::find_if(g_windowIdReservations.begin(),
            g_windowIdReservations.end(), [&ownerKey](
                const WindowIdReservation& reservation) {
                return OwnerEquals(reservation.owner, ownerKey);
            });
        if (found == g_windowIdReservations.end())
            return FALSE;
        if (found->factory)
            return found->factory == factory &&
                found->factoryContext == context ? TRUE : FALSE;
        found->factory = factory;
        found->factoryContext = context;
        GameNativeLog(L"[window-factory] registered: owner=%s",
            ownerKey.c_str());
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

BOOL UnregisterWindowFactory(LPCWSTR owner)
{
    if (!HookReady() || !owner)
        return FALSE;

    try
    {
        const std::wstring ownerKey(owner);
        if (!IsValidWindowIdOwner(ownerKey))
            return FALSE;

        ExclusiveSrwLock lock(g_windowIdLock);
        const auto found = std::find_if(g_windowIdReservations.begin(),
            g_windowIdReservations.end(), [&ownerKey](
                const WindowIdReservation& reservation) {
                return OwnerEquals(reservation.owner, ownerKey);
            });
        if (found == g_windowIdReservations.end() || !found->factory)
            return FALSE;
        found->factory = nullptr;
        found->factoryContext = nullptr;
        GameNativeLog(L"[window-factory] unregistered: owner=%s",
            ownerKey.c_str());
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

void* __cdecl HookWindowFactory(int windowId, void* manager)
{
    ClientPatchWindowFactoryFn factory = nullptr;
    void* context = nullptr;
    {
        SharedSrwLock lock(g_windowIdLock);
        const auto found = std::find_if(g_windowIdReservations.begin(),
            g_windowIdReservations.end(), [windowId](
                const WindowIdReservation& reservation) {
                return reservation.factory &&
                    std::find(reservation.windowIds.begin(),
                        reservation.windowIds.end(), windowId) !=
                        reservation.windowIds.end();
            });
        if (found != g_windowIdReservations.end())
        {
            factory = found->factory;
            context = found->factoryContext;
        }
    }

    if (factory)
    {
        // 工厂回调不能用 SEH 包返回值：Release 下 EAX 会变成 EXCEPTION_EXECUTE_HANDLER(1)。
        void* window = factory(windowId, manager, context);
        GameNativeLog(L"[factory] id=%d window=%p", windowId, window);
        if (reinterpret_cast<uintptr_t>(window) >= 0x10000)
            GameNativeNotePluginWindow(window, windowId);
        return window;
    }
    return g_originalWindowFactory ?
        g_originalWindowFactory(windowId, manager) : nullptr;
}

bool InstallWindowFactoryHook()
{
    const uintptr_t target = GameNativeClientAddress(kWindowFactoryAddress);
    static constexpr unsigned char kSignature[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    if (!GameNativeValidateBytes(target, kSignature, sizeof(kSignature)))
    {
        GameNativeLog(L"[hook] window factory signature mismatch at RVA 0x%08lX",
            static_cast<unsigned long>(kWindowFactoryAddress -
                kPreferredImageBase));
        return false;
    }

    void* trampoline = nullptr;
    if (!GameNativeInstallJump(target,
        reinterpret_cast<void*>(&HookWindowFactory), &trampoline))
        return false;
    g_originalWindowFactory = reinterpret_cast<WindowFactoryFn>(trampoline);
    GameNativeLog(L"[hook] window factory installed at RVA 0x%08lX",
        static_cast<unsigned long>(kWindowFactoryAddress -
            kPreferredImageBase));
    return true;
}

bool InstallXuiHook()
{
    const uintptr_t target = GameNativeClientAddress(kXuiLoaderAddress);
    static constexpr unsigned char kSignature[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    if (!GameNativeValidateBytes(target, kSignature, sizeof(kSignature)))
    {
        GameNativeLog(L"[hook] XUI loader signature mismatch at RVA 0x%08lX",
            static_cast<unsigned long>(kXuiLoaderAddress -
                kPreferredImageBase));
        return false;
    }
    static constexpr unsigned char kPostPrologueSignature[] = {
        0x68, 0, 0, 0, 0, 0x64, 0xA1, 0, 0, 0, 0,
        0x50, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57,
    };
    if (!GameNativeIsReadable(target + kHookLength,
        sizeof(kPostPrologueSignature)))
        return false;
    const auto* postPrologue = reinterpret_cast<const unsigned char*>(
        target + kHookLength);
    const uintptr_t sehAddress = *reinterpret_cast<const uintptr_t*>(
        postPrologue + 1);
    if (postPrologue[0] != kPostPrologueSignature[0] ||
        sehAddress != GameNativeClientAddress(kXuiLoaderSehAddress) ||
        std::memcmp(postPrologue + 5, kPostPrologueSignature + 5,
            sizeof(kPostPrologueSignature) - 5) != 0)
    {
        GameNativeLog(L"[hook] XUI loader body signature mismatch");
        return false;
    }

    void* trampoline = nullptr;
    if (!GameNativeInstallJump(target,
        reinterpret_cast<void*>(&HookXuiLoader), &trampoline))
        return false;
    g_originalXuiLoader = reinterpret_cast<XuiLoaderFn>(trampoline);
    GameNativeLog(L"[hook] installed %s at RVA 0x%08lX", kBuildTag,
        static_cast<unsigned long>(kXuiLoaderAddress -
            kPreferredImageBase));
    return true;
}

void LoadConfig()
{
    const std::wstring path = g_moduleDirectory + L"\\" + kConfigFileName;
    g_debug = GetPrivateProfileIntW(L"General", L"Debug", 0,
        path.c_str()) != 0;
}

}

void GameNativeLog(const wchar_t* format, ...)
{
    wchar_t message[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);

    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
    if (!g_debug)
        return;

    AcquireSRWLockExclusive(&g_logLock);
    FILE* file = nullptr;
    _wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8");
    if (file)
    {
        fwprintf(file, L"%s\n", message);
        fclose(file);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

extern "C" BOOL ClientPatchGetGameNativeApi(
    DWORD requestedVersion, ClientPatchGameNativeApi* api)
{
    if (!api ||
        requestedVersion != CLIENT_PATCH_GAME_NATIVE_API_VERSION ||
        api->cbSize < sizeof(ClientPatchGameNativeApi))
        return FALSE;

    api->version = CLIENT_PATCH_GAME_NATIVE_API_VERSION;
    api->cbSize = sizeof(ClientPatchGameNativeApi);
    api->mount = &MountXui;
    api->unmount = &UnmountXui;
    api->reserveWindowIds = &ReserveWindowIds;
    api->releaseWindowIds = &ReleaseWindowIds;
    api->registerWindowFactory = &RegisterWindowFactory;
    api->unregisterWindowFactory = &UnregisterWindowFactory;
    api->chatReady = &GameNativeChatReady;
    api->postChatNotice = &GameNativePostChatNotice;
    api->chatRgb = &GameNativeChatRgb;
    api->refreshPluginClips = &GameNativeRefreshPluginClips;
    api->postLoadNotice = &GameNativePostLoadNotice;
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit()
{
    LoadConfig();
    GameNativeStartNotice();
    const LONG previous = InterlockedCompareExchange(&g_hookState, -1, 0);
    if (previous != 0)
        return TRUE;

    if (!InstallWindowFactoryHook() || !InstallXuiHook())
    {
        InterlockedExchange(&g_hookState, 2);
        GameNativeLog(L"[init] layout hooks failed; chat notice is still available");
        return TRUE;
    }
    InterlockedExchange(&g_hookState, 1);
    GameNativeStartClipSupport();
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_moduleDirectory = ModuleDirectory(module);
        g_logPath = g_moduleDirectory + L"\\" + kLogFileName;
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        GameNativeStopClipSupport();
        GameNativeStopNotice();
    }
    return TRUE;
}
