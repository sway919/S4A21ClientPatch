#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>
#include <utility>

#include "ClientApi.h"
#include "../GameNative/GameNativeApi.h"
#include "EquipmentSwapModel.h"
#include "EquipmentSwapNativeUi.h"
#include "FileStore.h"

using equipment_swap::HotkeyBinding;
using equipment_swap::ProfileCollection;
using equipment_swap::ProfileSetting;
using equipment_swap::SlotSetting;
using equipment_swap::kProfileCount;
using equipment_swap::kSlotCount;
using equipment_swap::kSlotDefinitions;
using equipment_swap::ResetProfiles;

namespace
{
constexpr wchar_t kControllerClass[] =
    L"ClientPatchEquipmentSwapController";
constexpr wchar_t kConfigFileName[] = L"EquipmentSwap.ini";
constexpr wchar_t kPluginFolderName[] = L"EquipmentSwap";
constexpr wchar_t kDataDirectoryName[] = L"Data";
constexpr wchar_t kLayoutFileName[] = L"EquipmentSwap.xui";
// 客户端 PVF 的资源路径按大小写查找；现有 XUI 目录使用小写。
constexpr wchar_t kLayoutResourcePath[] = L"ui/equipmentswap.xui";
constexpr wchar_t kLogFileName[] = L"EquipmentSwap.log";
constexpr wchar_t kWindowIdOwner[] = L"ClientPatch.EquipmentSwap";
constexpr wchar_t kDispatchName[] =
    L"ClientPatch.EquipmentSwap.Native.Dispatch.1.0";

constexpr UINT kControllerTimerId = 1;
constexpr UINT kControllerLoadCharacter = WM_APP + 41;
constexpr UINT kControllerSaveProfile = WM_APP + 42;
constexpr ULONGLONG kMoveTimeoutMs = 2500;

enum DispatchCommand : WPARAM
{
    kDispatchToggleWindow = 1,
    kDispatchTick = 3,
    kDispatchApplyLoad = 4,
    kDispatchStoreError = 5,
};

struct PluginConfig
{
    bool enabled = false;
    bool debug = false;
    HotkeyBinding windowHotkey;
};

struct LoadResult
{
    unsigned int characterId = 0;
    ProfileCollection profiles;
    bool ok = false;
    std::wstring error;
};

struct SaveRequest
{
    unsigned int characterId = 0;
    int profileIndex = -1;
    ProfileSetting profile;
};

struct SwapJob
{
    bool active = false;
    bool waitingForUpdate = false;
    int nextOrderIndex = 0;
    int pendingSlot = -1;
    int pendingItemId = 0;
    int pendingQualitySeed = 0;
    bool pendingMatchQuality = false;
    int moved = 0;
    int alreadyEquipped = 0;
    int missing = 0;
    int rejected = 0;
    ULONGLONG nextActionTick = 0;
    ULONGLONG updateDeadline = 0;
    ULONGLONG pendingStartedTick = 0;
    ProfileSetting profile;
};

HMODULE g_module = nullptr;
std::wstring g_moduleDirectory;
std::wstring g_logPath;
PluginConfig g_config;
HWND g_controllerWindow = nullptr;
DWORD g_controllerThreadId = 0;
HWND g_gameWindow = nullptr;
DWORD g_gameThreadId = 0;
HHOOK g_gameThreadHook = nullptr;
HHOOK g_gameGetMessageHook = nullptr;
UINT g_dispatchMessage = 0;
equipment_swap::FileStore g_store;
bool g_storeReady = false;
LONG g_started = 0;
ClientPatchGameNativeApi g_native = {};
bool g_nativeBound = false;
bool g_nativeMissLogged = false;
bool g_loadNoticeSent = false;
bool g_externalLayoutMounted = false;
bool g_windowIdsReserved = false;
bool g_windowFactoryRegistered = false;

ProfileCollection g_profiles;
unsigned int g_characterId = 0;
unsigned int g_loadingId = 0;
std::wstring g_statusText = L"等待进入角色";
bool g_modelReady = false;
bool g_loading = false;
int g_selectedProfile = 0;
bool g_capturingHotkey = false;
bool g_hotkeyPending = false;
HotkeyBinding g_pendingHotkey;
ULONGLONG g_nextCharacterCheck = 0;
SwapJob g_swapJob;
SRWLOCK g_logLock = SRWLOCK_INIT;

void QueueGameCommand(WPARAM command, LPARAM argument = 0);
void HandleGameDispatch(WPARAM command, LPARAM argument);
bool InstallGameThreadHook();
void RefreshNativeUi();
void StopSwapJob(const std::wstring& text);

std::wstring Trim(const std::wstring& value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first]))
        ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1]))
        --last;
    return value.substr(first, last - first);
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

void Log(const wchar_t* format, ...);

bool EnsureGameNative()
{
    if (g_nativeBound)
        return true;

    g_native = {};
    g_nativeBound = ClientPatchBindGameNative(&g_native);
    if (g_nativeBound)
        return true;
    if (!g_nativeMissLogged)
    {
        g_nativeMissLogged = true;
        Log(L"[原生界面] GameNative.dll 未就绪");
    }
    return false;
}

INT NoticeColor()
{
    return g_native.chatRgb ? g_native.chatRgb(155, 200, 230) : 0;
}

void QueueOfficialNotice(const wchar_t* text)
{
    if (!text || !text[0] || !EnsureGameNative() || !g_native.postChatNotice)
        return;
    g_native.postChatNotice(text, NoticeColor());
    Log(L"[聊天] 已交 GameNative 公告：%s", text);
}

void RequestLoadNotice()
{
    if (g_loadNoticeSent)
        return;
    if (!EnsureGameNative())
        return;
    if (g_native.postLoadNotice)
        g_native.postLoadNotice(L"换装插件已载入", NoticeColor());
    else if (g_native.postChatNotice)
        g_native.postChatNotice(L"换装插件已载入", NoticeColor());
    else
        return;
    g_loadNoticeSent = true;
    Log(L"[聊天] 已交 GameNative 载入公告");
}

void RollbackExternalUiPreparation()
{
    if (g_windowFactoryRegistered && g_native.unregisterWindowFactory)
        g_native.unregisterWindowFactory(kWindowIdOwner);
    equipment_swap::native_ui::Uninstall();
    if (g_externalLayoutMounted && g_native.unmount)
        g_native.unmount(kLayoutResourcePath);
    if (g_windowIdsReserved && g_native.releaseWindowIds)
        g_native.releaseWindowIds(kWindowIdOwner);
    g_externalLayoutMounted = false;
    g_windowIdsReserved = false;
    g_windowFactoryRegistered = false;
}

bool PrepareExternalUi(const std::wstring& diskPath, int& windowId)
{
    if (!EnsureGameNative() || !g_native.mount || !g_native.unmount ||
        !g_native.reserveWindowIds || !g_native.releaseWindowIds ||
        !g_native.registerWindowFactory || !g_native.unregisterWindowFactory)
    {
        Log(L"[原生界面] GameNative 布局 API 不完整，无法挂载外部布局");
        return false;
    }

    if (!g_native.mount(kLayoutResourcePath, diskPath.c_str(),
        CLIENT_PATCH_GAME_NATIVE_MOUNT_STRICT))
    {
        Log(L"[原生界面] 注册外部 XUI 失败：%s", diskPath.c_str());
        return false;
    }
    g_externalLayoutMounted = true;

    int reservedWindowId = -1;
    if (!g_native.reserveWindowIds(kWindowIdOwner, 1,
        CLIENT_PATCH_WINDOW_ID_EQUIPMENT, &reservedWindowId))
    {
        Log(L"[原生界面] 无法申请装备栏之后的一个窗口 ID");
        RollbackExternalUiPreparation();
        return false;
    }
    g_windowIdsReserved = true;
    windowId = reservedWindowId;

    Log(L"[原生界面] 已通过 GameNative 挂载布局并申请窗口 ID %d：%s -> %s",
        windowId, kLayoutResourcePath, diskPath.c_str());
    return true;
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

std::wstring PluginContentDirectory()
{
    return g_moduleDirectory + L"\\" + kPluginFolderName;
}

void Log(const wchar_t* format, ...)
{
    if (!g_config.debug)
        return;
    wchar_t message[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);

    AcquireSRWLockExclusive(&g_logLock);
    FILE* file = nullptr;
    _wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8");
    if (file)
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fwprintf(file, L"[%02u:%02u:%02u.%03u] %s\n", now.wHour,
            now.wMinute, now.wSecond, now.wMilliseconds, message);
        fclose(file);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void SetStatus(const std::wstring& text)
{
    g_statusText = text;
    Log(L"[状态] %s", text.c_str());
    RefreshNativeUi();
}

UINT VirtualKeyFromToken(const std::wstring& token)
{
    if (token.size() == 1)
    {
        const SHORT value = VkKeyScanW(token[0]);
        return value == -1 ? 0 : static_cast<UINT>(LOBYTE(value));
    }
    if (token.size() >= 2 && (token[0] == L'f' || token[0] == L'F'))
    {
        const int number = _wtoi(token.c_str() + 1);
        return number >= 1 && number <= 24 ? VK_F1 + number - 1 : 0;
    }
    if (EqualsIgnoreCase(token, L"Space")) return VK_SPACE;
    if (EqualsIgnoreCase(token, L"Enter")) return VK_RETURN;
    if (EqualsIgnoreCase(token, L"Tab")) return VK_TAB;
    if (EqualsIgnoreCase(token, L"Esc") ||
        EqualsIgnoreCase(token, L"Escape")) return VK_ESCAPE;
    if (EqualsIgnoreCase(token, L"Insert")) return VK_INSERT;
    if (EqualsIgnoreCase(token, L"Delete")) return VK_DELETE;
    if (EqualsIgnoreCase(token, L"Home")) return VK_HOME;
    if (EqualsIgnoreCase(token, L"End")) return VK_END;
    if (EqualsIgnoreCase(token, L"PageUp")) return VK_PRIOR;
    if (EqualsIgnoreCase(token, L"PageDown")) return VK_NEXT;
    if (EqualsIgnoreCase(token, L"Up")) return VK_UP;
    if (EqualsIgnoreCase(token, L"Down")) return VK_DOWN;
    if (EqualsIgnoreCase(token, L"Left")) return VK_LEFT;
    if (EqualsIgnoreCase(token, L"Right")) return VK_RIGHT;
    return 0;
}

HotkeyBinding ParseHotkey(const std::wstring& text, bool& valid)
{
    valid = false;
    HotkeyBinding result;
    const std::wstring value = Trim(text);
    if (value.empty() || EqualsIgnoreCase(value, L"None") ||
        EqualsIgnoreCase(value, L"无"))
    {
        valid = true;
        return result;
    }

    size_t start = 0;
    while (start <= value.size())
    {
        const size_t end = value.find(L'+', start);
        const std::wstring token = Trim(value.substr(start,
            end == std::wstring::npos ? std::wstring::npos : end - start));
        if (EqualsIgnoreCase(token, L"Ctrl") ||
            EqualsIgnoreCase(token, L"Control"))
            result.modifiers |= MOD_CONTROL;
        else if (EqualsIgnoreCase(token, L"Alt"))
            result.modifiers |= MOD_ALT;
        else if (EqualsIgnoreCase(token, L"Shift"))
            result.modifiers |= MOD_SHIFT;
        else if (EqualsIgnoreCase(token, L"Win") ||
            EqualsIgnoreCase(token, L"Windows"))
            result.modifiers |= MOD_WIN;
        else if (!result.virtualKey)
            result.virtualKey = VirtualKeyFromToken(token);
        else
            return result;
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    valid = result.virtualKey != 0;
    return result;
}

std::wstring VirtualKeyName(UINT virtualKey)
{
    if ((virtualKey >= L'A' && virtualKey <= L'Z') ||
        (virtualKey >= L'0' && virtualKey <= L'9'))
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    switch (virtualKey)
    {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    default: break;
    }
    return L"按键";
}

std::wstring FormatHotkey(const HotkeyBinding& hotkey)
{
    if (!hotkey.IsValid())
        return L"未设置";
    std::wstring result;
    if (hotkey.modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (hotkey.modifiers & MOD_ALT) result += L"Alt+";
    if (hotkey.modifiers & MOD_SHIFT) result += L"Shift+";
    if (hotkey.modifiers & MOD_WIN) result += L"Win+";
    return result + VirtualKeyName(hotkey.virtualKey);
}

void RefreshNativeUi()
{
    equipment_swap::native_ui::UiState state;
    state.selectedProfile = g_selectedProfile;
    state.statusText = g_statusText;
    state.hotkeyPending = g_capturingHotkey || g_hotkeyPending;

    if (g_selectedProfile >= 0 && g_selectedProfile < kProfileCount)
    {
        const ProfileSetting& profile = g_profiles[g_selectedProfile];
        state.currentHotkey = FormatHotkey(g_hotkeyPending ?
            g_pendingHotkey : profile.hotkey);
        for (int index = 0; index < kSlotCount; ++index)
            state.itemIds[index] = profile.slots[index].itemId;
    }
    equipment_swap::native_ui::Refresh(state);
}

bool PostController(UINT message, LPARAM argument)
{
    return g_controllerWindow && PostMessageW(g_controllerWindow, message,
        0, argument);
}

template <typename T>
void PostOwnedController(UINT message, T* value)
{
    if (!PostController(message, reinterpret_cast<LPARAM>(value)))
        delete value;
}

void QueueSaveProfile(int profileIndex)
{
    if (!g_modelReady || g_characterId == 0 || profileIndex < 0 ||
        profileIndex >= kProfileCount)
        return;
    if (g_profiles[profileIndex].name.empty())
    {
        g_profiles[profileIndex].name =
            L"方案 " + std::to_wstring(profileIndex + 1);
    }
    auto* request = new SaveRequest;
    request->characterId = g_characterId;
    request->profileIndex = profileIndex;
    request->profile = g_profiles[profileIndex];
    PostOwnedController(kControllerSaveProfile, request);
}

bool ProfileHasItems(const ProfileSetting& profile)
{
    for (const SlotSetting& slot : profile.slots)
    {
        if (slot.itemId > 0)
            return true;
    }
    return false;
}

void RequestCharacterLoad(unsigned int characterId)
{
    if (characterId == 0 || (characterId == g_loadingId && g_loading))
        return;
    if (g_swapJob.active)
        StopSwapJob(L"角色已切换，换装任务已停止");
    g_loadingId = characterId;
    g_loading = true;
    g_modelReady = false;
    g_capturingHotkey = false;
    g_hotkeyPending = false;
    g_pendingHotkey = {};
    ResetProfiles(g_profiles);
    PostOwnedController(kControllerLoadCharacter,
        new unsigned int(characterId));
    const std::wstring name = equipment_swap::client_api::CurrentCharacterName();
    Log(L"[角色] 载入换装数据：ID=%u 名=%s", characterId,
        name.empty() ? L"(空)" : name.c_str());
    SetStatus(L"正在读取角色换装方案");
}

void ApplyLoadResult(LoadResult* result)
{
    if (!result)
        return;
    const bool sameCharacter = result->characterId == g_characterId;
    if (sameCharacter)
    {
        g_loading = false;
        g_loadingId = 0;
        if (result->ok)
        {
            g_profiles = result->profiles;
            g_modelReady = true;
            g_capturingHotkey = false;
            g_hotkeyPending = false;
            g_pendingHotkey = {};
            if (g_selectedProfile < 0 || g_selectedProfile >= kProfileCount)
                g_selectedProfile = 0;
            SetStatus(L"角色方案已载入");
        }
        else
        {
            ResetProfiles(g_profiles);
            g_modelReady = false;
            g_capturingHotkey = false;
            g_hotkeyPending = false;
            g_pendingHotkey = {};
            SetStatus(L"读取角色方案失败：" + result->error);
        }
    }
    delete result;
}

void MonitorCharacter()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextCharacterCheck)
        return;
    g_nextCharacterCheck = now + 500;

    const unsigned int characterId =
        equipment_swap::client_api::CurrentCharacterId();
    if (characterId == 0)
    {
        if (g_characterId != 0 || g_modelReady)
        {
            equipment_swap::native_ui::Close();
            g_characterId = 0;
            g_loadingId = 0;
            g_loading = false;
            g_modelReady = false;
            g_capturingHotkey = false;
            g_hotkeyPending = false;
            g_pendingHotkey = {};
            g_swapJob = {};
            ResetProfiles(g_profiles);
            SetStatus(L"等待进入角色");
        }
        return;
    }
    if (g_characterId != characterId)
    {
        equipment_swap::native_ui::Close();
        g_characterId = characterId;
        RequestCharacterLoad(characterId);
    }
}

bool FindItemSlot(const equipment_swap::SlotDefinition& definition,
    int itemId, int qualitySeed, bool matchQuality, int preferredSlot,
    int& result)
{
    auto matches = [&](int slot, bool requireQuality) -> bool
    {
        if (equipment_swap::client_api::ReadItemId(definition.sourceListType,
            slot) != itemId)
            return false;
        if (!requireQuality)
            return true;
        return equipment_swap::client_api::ReadItemQualitySeed(
            definition.sourceListType, slot) == qualitySeed;
    };

    if (matchQuality)
    {
        if (preferredSlot >= definition.sourceFirstSlot &&
            preferredSlot <= definition.sourceLastSlot &&
            matches(preferredSlot, true))
        {
            result = preferredSlot;
            return true;
        }
        for (int slot = definition.sourceFirstSlot;
            slot <= definition.sourceLastSlot; ++slot)
        {
            if (matches(slot, true))
            {
                result = slot;
                return true;
            }
        }
    }

    if (preferredSlot >= definition.sourceFirstSlot &&
        preferredSlot <= definition.sourceLastSlot &&
        matches(preferredSlot, false))
    {
        result = preferredSlot;
        return true;
    }
    for (int slot = definition.sourceFirstSlot;
        slot <= definition.sourceLastSlot; ++slot)
    {
        if (matches(slot, false))
        {
            result = slot;
            return true;
        }
    }
    return false;
}

void CaptureCurrentEquipment()
{
    if (!g_modelReady || !equipment_swap::client_api::IsReady())
    {
        SetStatus(L"客户端尚未就绪，无法读取当前穿戴");
        return;
    }

    ProfileSetting& profile = g_profiles[g_selectedProfile];
    int captured = 0;
    for (int index = 0; index < kSlotCount; ++index)
    {
        const auto& definition = kSlotDefinitions[index];
        SlotSetting setting = {};
        setting.itemId = equipment_swap::client_api::ReadEquippedItemId(
            definition.destinationSlot);
        if (setting.itemId > 0)
        {
            ++captured;
            if (equipment_swap::IsEquipmentDestinationSlot(
                definition.destinationSlot))
            {
                setting.qualitySeed =
                    equipment_swap::client_api::ReadEquippedQualitySeed(
                        definition.destinationSlot);
            }
            int sourceSlot = -1;
            if (FindItemSlot(definition, setting.itemId, setting.qualitySeed,
                setting.qualitySeed != 0, -1, sourceSlot))
                setting.preferredSourceSlot = static_cast<short>(sourceSlot);
        }
        profile.slots[index] = setting;
    }
    QueueSaveProfile(g_selectedProfile);
    SetStatus(L"已读取当前穿戴，记录 " + std::to_wstring(captured) +
        L" 件装备、时装、宠物或勋章");
}

void NotifySwap(const std::wstring& text)
{
    SetStatus(text);
    if (text.empty())
        return;
    std::wstring message = L"换装： " + text;
    if (message.size() > 191)
        message.resize(191);
    QueueOfficialNotice(message.c_str());
}

void ClearPendingSwap()
{
    g_swapJob.waitingForUpdate = false;
    g_swapJob.pendingSlot = -1;
    g_swapJob.pendingItemId = 0;
    g_swapJob.pendingQualitySeed = 0;
    g_swapJob.pendingMatchQuality = false;
    g_swapJob.pendingStartedTick = 0;
    g_swapJob.updateDeadline = 0;
}

std::wstring FormatSwapResult()
{
    std::wstring result = L"换装完成：更换 " +
        std::to_wstring(g_swapJob.moved) + L" 件，已穿戴 " +
        std::to_wstring(g_swapJob.alreadyEquipped) + L" 件，未找到 " +
        std::to_wstring(g_swapJob.missing) + L" 件";
    if (g_swapJob.rejected > 0)
    {
        result += L"，未更换 " + std::to_wstring(g_swapJob.rejected) +
            L" 件。换装校验未通过，可能在副本换装CD中";
    }
    return result;
}

void StopSwapJob(const std::wstring& text)
{
    g_swapJob = {};
    NotifySwap(text);
}

void StartSwapJob(int profileIndex)
{
    if (!g_modelReady || g_characterId == 0)
    {
        NotifySwap(L"角色方案尚未载入");
        return;
    }
    if (!equipment_swap::client_api::IsReady())
    {
        NotifySwap(L"客户端版本签名不匹配，已阻止换装");
        return;
    }
    if (g_swapJob.active)
    {
        NotifySwap(L"已有换装任务正在执行");
        return;
    }
    if (profileIndex < 0 || profileIndex >= kProfileCount)
        return;

    const ProfileSetting& profile = g_profiles[profileIndex];
    if (!ProfileHasItems(profile))
    {
        NotifySwap(L"该方案尚未读取任何穿戴");
        return;
    }

    g_swapJob = {};
    g_swapJob.active = true;
    g_swapJob.profile = profile;
    g_swapJob.nextActionTick = GetTickCount64();
    NotifySwap(L"正在执行“" + profile.name + L"”");
}

void ProcessSwapJob()
{
    if (!g_swapJob.active)
        return;
    const ULONGLONG now = GetTickCount64();
    if (equipment_swap::client_api::CurrentCharacterId() != g_characterId)
    {
        StopSwapJob(L"角色已切换，换装任务已停止");
        return;
    }

    if (g_swapJob.waitingForUpdate)
    {
        if (g_swapJob.pendingSlot < 0 || g_swapJob.pendingSlot >= kSlotCount)
        {
            ++g_swapJob.rejected;
            ClearPendingSwap();
            g_swapJob.nextActionTick = now;
        }
        else
        {
            const auto& definition =
                kSlotDefinitions[g_swapJob.pendingSlot];
            const int equippedId =
                equipment_swap::client_api::ReadEquippedItemId(
                    definition.destinationSlot);
            const bool idMatched = equippedId == g_swapJob.pendingItemId;
            const bool qualityMatched = !g_swapJob.pendingMatchQuality ||
                equipment_swap::client_api::ReadEquippedQualitySeed(
                    definition.destinationSlot) ==
                    g_swapJob.pendingQualitySeed;
            if (idMatched && qualityMatched)
            {
                const ULONGLONG elapsed = now -
                    g_swapJob.pendingStartedTick;
                ++g_swapJob.moved;
                Log(L"[换装] 原生穿戴事务已确认：%s，物品ID=%d，品质种子=%d，耗时=%llums",
                    definition.label, g_swapJob.pendingItemId,
                    g_swapJob.pendingQualitySeed,
                    static_cast<unsigned long long>(elapsed));
                ClearPendingSwap();
                g_swapJob.nextActionTick = now;
            }
            else if (now >= g_swapJob.updateDeadline)
            {
                ++g_swapJob.rejected;
                Log(L"[换装] 原生穿戴事务未能更新装备栏：%s，物品ID=%d",
                    definition.label, g_swapJob.pendingItemId);
                ClearPendingSwap();
                g_swapJob.nextActionTick = now;
            }
            else
            {
                return;
            }
        }
    }
    if (now < g_swapJob.nextActionTick)
        return;

    while (g_swapJob.nextOrderIndex < kSlotCount)
    {
        const int slotIndex =
            equipment_swap::kSwapSlotOrder[g_swapJob.nextOrderIndex++];
        const SlotSetting& setting = g_swapJob.profile.slots[slotIndex];
        const auto& definition = kSlotDefinitions[slotIndex];
        if (setting.itemId <= 0)
            continue;

        const bool matchQuality =
            equipment_swap::IsEquipmentDestinationSlot(
                definition.destinationSlot) &&
            setting.qualitySeed != 0;
        const int equipped = equipment_swap::client_api::ReadEquippedItemId(
            definition.destinationSlot);
        if (equipped == setting.itemId)
        {
            if (!matchQuality ||
                equipment_swap::client_api::ReadEquippedQualitySeed(
                    definition.destinationSlot) == setting.qualitySeed)
            {
                ++g_swapJob.alreadyEquipped;
                continue;
            }
        }

        int sourceSlot = -1;
        if (!FindItemSlot(definition, setting.itemId, setting.qualitySeed,
            matchQuality, setting.preferredSourceSlot, sourceSlot))
        {
            if (equipped == setting.itemId)
            {
                ++g_swapJob.alreadyEquipped;
                continue;
            }
            ++g_swapJob.missing;
            Log(L"[换装] 未找到%s，物品ID=%d，品质种子=%d", definition.label,
                setting.itemId, setting.qualitySeed);
            continue;
        }

        const int sourceQuality =
            equipment_swap::IsEquipmentDestinationSlot(
                definition.destinationSlot) ?
            equipment_swap::client_api::ReadItemQualitySeed(
                definition.sourceListType, sourceSlot) : 0;
        const bool sourceQualityMatched = matchQuality &&
            sourceQuality != 0 && sourceQuality == setting.qualitySeed;

        if (!equipment_swap::client_api::MoveItemNative(
            definition.sourceListType, sourceSlot,
            definition.destinationSlot))
        {
            ++g_swapJob.rejected;
            Log(L"[换装] 原生穿戴校验未通过：%s，来源列表=%u，来源槽=%d，目标槽=%d，物品ID=%d",
                definition.label, definition.sourceListType, sourceSlot,
                definition.destinationSlot, setting.itemId);
            continue;
        }
        Log(L"[换装] 已调用原生穿戴事务：%s，来源列表=%u，来源槽=%d，目标槽=%d，物品ID=%d，品质种子=%d，品质精确匹配=%d",
            definition.label, definition.sourceListType, sourceSlot,
            definition.destinationSlot, setting.itemId, setting.qualitySeed,
            sourceQualityMatched ? 1 : 0);
        g_swapJob.pendingSlot = slotIndex;
        g_swapJob.pendingItemId = setting.itemId;
        // 身上已是同模板、正在换另一件品质时，不能只认 ID，否则会立刻假成功。
        // 背包件品质读到 0 时仍按方案种子确认。
        g_swapJob.pendingQualitySeed = sourceQuality != 0 ?
            sourceQuality : setting.qualitySeed;
        g_swapJob.pendingMatchQuality = matchQuality;
        g_swapJob.waitingForUpdate = true;
        g_swapJob.pendingStartedTick = now;
        g_swapJob.updateDeadline = now + kMoveTimeoutMs;
        return;
    }

    StopSwapJob(FormatSwapResult());
}

void CommitPendingHotkey()
{
    if (!g_modelReady || g_selectedProfile < 0 ||
        g_selectedProfile >= kProfileCount || !g_hotkeyPending ||
        !g_pendingHotkey.IsValid())
        return;

    const HotkeyBinding hotkey = g_pendingHotkey;
    int replacedProfile = -1;
    for (int index = 0; index < kProfileCount; ++index)
    {
        if (index == g_selectedProfile)
            continue;
        const HotkeyBinding& existing = g_profiles[index].hotkey;
        if (existing.virtualKey == hotkey.virtualKey &&
            existing.modifiers == hotkey.modifiers)
        {
            g_profiles[index].hotkey = {};
            QueueSaveProfile(index);
            replacedProfile = index;
        }
    }
    g_profiles[g_selectedProfile].hotkey = hotkey;
    g_capturingHotkey = false;
    g_hotkeyPending = false;
    g_pendingHotkey = {};
    QueueSaveProfile(g_selectedProfile);
    if (replacedProfile >= 0)
        Log(L"[快捷键] 方案%d的重复绑定已由方案%d接管",
            replacedProfile + 1, g_selectedProfile + 1);
    SetStatus(L"快捷键已保存");
}

void FinishHotkeyCapture(const HotkeyBinding& hotkey)
{
    if (!g_modelReady || g_selectedProfile < 0 ||
        g_selectedProfile >= kProfileCount || !hotkey.IsValid())
        return;
    g_pendingHotkey = hotkey;
    g_hotkeyPending = true;
    g_capturingHotkey = false;
    SetStatus(L"按键已录入，点击保存");
}

bool IsGameInputWindow(HWND hwnd)
{
    return hwnd && g_gameWindow && IsWindow(g_gameWindow) &&
        (hwnd == g_gameWindow || IsChild(g_gameWindow, hwnd));
}

UINT CurrentModifierMask()
{
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)
        modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
        modifiers |= MOD_WIN;
    return modifiers;
}

bool MatchesHotkey(const HotkeyBinding& hotkey, UINT virtualKey)
{
    return hotkey.IsValid() && hotkey.virtualKey == virtualKey &&
        hotkey.modifiers == CurrentModifierMask();
}

void ConsumeKeyMessage(MSG* message)
{
    message->message = WM_NULL;
    message->wParam = 0;
    message->lParam = 0;
}

bool CaptureHotkeyMessage(MSG* message)
{
    if (!g_capturingHotkey || !message ||
        (message->message != WM_KEYDOWN &&
            message->message != WM_SYSKEYDOWN))
        return false;

    const UINT virtualKey = static_cast<UINT>(message->wParam);
    if (virtualKey != VK_SHIFT && virtualKey != VK_CONTROL &&
        virtualKey != VK_MENU && virtualKey != VK_LWIN &&
        virtualKey != VK_RWIN)
    {
        HotkeyBinding hotkey;
        hotkey.modifiers = CurrentModifierMask();
        hotkey.virtualKey = virtualKey;
        FinishHotkeyCapture(hotkey);
    }

    ConsumeKeyMessage(message);
    return true;
}

// 只处理游戏窗口（含子窗口）的按键消息，避免 RegisterHotKey 系统级抢键。
bool HandleBoundHotkeyMessage(MSG* message)
{
    if (!message || g_capturingHotkey ||
        (message->message != WM_KEYDOWN &&
            message->message != WM_SYSKEYDOWN))
        return false;
    // lParam bit30 = 前一次按键仍按下，跳过自动重复。
    if (message->lParam & (1u << 30))
        return false;

    const UINT virtualKey = static_cast<UINT>(message->wParam);
    if (MatchesHotkey(g_config.windowHotkey, virtualKey))
    {
        HandleGameDispatch(kDispatchToggleWindow, 0);
        ConsumeKeyMessage(message);
        return true;
    }

    if (!g_modelReady || g_characterId == 0)
        return false;

    for (int index = 0; index < kProfileCount; ++index)
    {
        if (!MatchesHotkey(g_profiles[index].hotkey, virtualKey))
            continue;
        if (!ProfileHasItems(g_profiles[index]))
            continue;
        StartSwapJob(index);
        ConsumeKeyMessage(message);
        return true;
    }
    return false;
}

void HandleNativeUiCommand(equipment_swap::native_ui::Command command,
    int argument)
{
    using equipment_swap::native_ui::Command;
    switch (command)
    {
    case Command::SelectProfile:
        if (g_modelReady && argument >= 0 && argument < kProfileCount)
        {
            g_selectedProfile = argument;
            g_capturingHotkey = false;
            g_hotkeyPending = false;
            g_pendingHotkey = {};
            SetStatus(L"");
        }
        break;
    case Command::RecordHotkey:
        if (g_modelReady)
        {
            if (g_hotkeyPending)
                CommitPendingHotkey();
            else if (!g_capturingHotkey)
            {
                g_capturingHotkey = true;
                SetStatus(L"请按下需要绑定的按键");
            }
        }
        break;
    case Command::CaptureEquipment:
        CaptureCurrentEquipment();
        break;
    case Command::ExecuteSwap:
        StartSwapJob(g_selectedProfile);
        break;
    case Command::ClearProfile:
        if (!g_modelReady)
            break;
        g_profiles[g_selectedProfile].slots = {};
        g_capturingHotkey = false;
        g_hotkeyPending = false;
        g_pendingHotkey = {};
        QueueSaveProfile(g_selectedProfile);
        SetStatus(L"当前方案已清空");
        break;
    case Command::Diagnostic:
        if (argument >= 1160 && argument < 1160 + kSlotCount)
            Log(L"[原生界面] 原生槽位热区 IControl 查找失败：ID=%d",
                argument - 1100);
        else if (argument >= 5000 && argument < 5000 + kSlotCount)
            Log(L"[原生界面] 槽位图标写入失败：槽位=%d", argument - 5000);
        else if (argument == 4000 + kSlotCount)
            Log(L"[原生界面] 已绑定全部 %d 个槽位热区", kSlotCount);
        else if (argument >= 2000 && argument < 2500)
        {
            const int stage = (argument - 2000) / 100;
            const int controlId = (argument - 2000) % 100;
            if (stage == 0)
                Log(L"[原生界面] 文本控件查找失败：ID=%d", controlId);
            else if (stage == 1)
                Log(L"[原生界面] 已写入文本字段 this+0x110：ID=%d",
                    controlId);
            else if (stage == 2)
                Log(L"[原生界面] 已走虚表+220 写字：ID=%d", controlId);
            else if (stage == 4)
                Log(L"[原生界面] 文本控件带绘制器 this+0x350：ID=%d",
                    controlId);
            else
                Log(L"[原生界面] 文本写入失败：ID=%d", controlId);
        }
        else if (argument == 2500)
            Log(L"[原生界面] 42ACB0 校验失败，文本字段写入已禁用");
        else if (argument == 8)
            Log(L"[原生界面] HasWindow=1，窗口已登记");
        else if (argument == 9)
            Log(L"[原生界面] HasWindow=0，窗口未进入显示列表");
        else
            Log(L"[原生界面] 工厂诊断码=%d 窗口=%p", argument,
                equipment_swap::native_ui::CurrentWindow());
        break;
    default:
        break;
    }
}


void HandleGameDispatch(WPARAM command, LPARAM argument)
{
    switch (command)
    {
    case kDispatchToggleWindow:
    {
        const unsigned int characterId =
            equipment_swap::client_api::CurrentCharacterId();
        if (characterId == 0)
        {
            SetStatus(L"尚未进入角色");
            equipment_swap::native_ui::Close();
            break;
        }
        if (g_characterId != characterId)
        {
            g_characterId = characterId;
            RequestCharacterLoad(characterId);
        }
        equipment_swap::native_ui::Toggle();
        break;
    }
    case kDispatchTick:
        RequestLoadNotice();
        MonitorCharacter();
        ProcessSwapJob();
        equipment_swap::native_ui::Poll();
        if (EnsureGameNative())
            g_native.refreshPluginClips();
        break;
    case kDispatchApplyLoad:
        ApplyLoadResult(reinterpret_cast<LoadResult*>(argument));
        break;
    case kDispatchStoreError:
    {
        auto* error = reinterpret_cast<std::wstring*>(argument);
        if (error)
        {
            SetStatus(L"保存换装数据失败：" + *error);
            delete error;
        }
        break;
    }
    default:
        break;
    }
}

BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter)
{
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() ||
        threadId == g_controllerThreadId || !IsWindowVisible(window) ||
        GetParent(window))
        return TRUE;
    RECT rect = {};
    if (!GetWindowRect(window, &rect))
        return TRUE;
    const long area = (rect.right - rect.left) *
        (rect.bottom - rect.top);
    auto* best = reinterpret_cast<std::pair<HWND, long>*>(parameter);
    if (area > best->second)
        *best = { window, area };
    return TRUE;
}

LRESULT CALLBACK GameCallWindowHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0)
    {
        const auto* call = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (call && call->message == g_dispatchMessage)
            HandleGameDispatch(call->wParam, call->lParam);
    }
    return CallNextHookEx(g_gameThreadHook, code, wParam, lParam);
}

LRESULT CALLBACK GameGetMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && wParam == PM_REMOVE && lParam)
    {
        auto* message = reinterpret_cast<MSG*>(lParam);
        if (message->message == g_dispatchMessage)
            HandleGameDispatch(message->wParam, message->lParam);
        else if (IsGameInputWindow(message->hwnd))
        {
            if (!CaptureHotkeyMessage(message))
                HandleBoundHotkeyMessage(message);
        }
    }
    return CallNextHookEx(g_gameGetMessageHook, code, wParam, lParam);
}

bool InstallGameThreadHook()
{
    if (g_gameThreadHook && g_gameGetMessageHook && g_gameWindow &&
        IsWindow(g_gameWindow))
        return true;

    if (g_gameThreadHook)
    {
        UnhookWindowsHookEx(g_gameThreadHook);
        g_gameThreadHook = nullptr;
    }
    if (g_gameGetMessageHook)
    {
        UnhookWindowsHookEx(g_gameGetMessageHook);
        g_gameGetMessageHook = nullptr;
    }

    std::pair<HWND, long> best = { nullptr, 0 };
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&best));
    if (!best.first)
        return false;

    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(best.first, &processId);
    if (!threadId || processId != GetCurrentProcessId())
        return false;

    HHOOK callHook = SetWindowsHookExW(WH_CALLWNDPROC,
        GameCallWindowHook, g_module, threadId);
    if (!callHook)
        return false;
    HHOOK messageHook = SetWindowsHookExW(WH_GETMESSAGE,
        GameGetMessageHook, g_module, threadId);
    if (!messageHook)
    {
        UnhookWindowsHookEx(callHook);
        return false;
    }

    g_gameWindow = best.first;
    g_gameThreadId = threadId;
    g_gameThreadHook = callHook;
    g_gameGetMessageHook = messageHook;
    Log(L"[客户端] 已接入游戏窗口线程：TID=%lu",
        static_cast<unsigned long>(threadId));
    return true;
}

void QueueGameCommand(WPARAM command, LPARAM argument)
{
    if (!InstallGameThreadHook() || !g_gameWindow)
        return;

    if (!PostThreadMessageW(g_gameThreadId, g_dispatchMessage, command,
        argument))
    {
        Log(L"[原生界面] 向游戏线程投递命令失败：命令=%llu，错误=%lu",
            static_cast<unsigned long long>(command),
            static_cast<unsigned long>(GetLastError()));
    }
    else if (command != kDispatchTick)
    {
        Log(L"[原生界面] 已向游戏线程投递命令：命令=%llu",
            static_cast<unsigned long long>(command));
    }
}

void LoadCharacterOnController(unsigned int* characterId)
{
    if (!characterId)
        return;
    auto* result = new LoadResult;
    result->characterId = *characterId;
    result->ok = g_storeReady && g_store.LoadCharacter(
        *characterId, result->profiles, result->error);
    if (!result->ok && result->error.empty())
        result->error = L"换装数据目录尚未就绪";
    delete characterId;

    if (!InstallGameThreadHook() || !g_gameWindow)
    {
        delete result;
        return;
    }
    QueueGameCommand(kDispatchApplyLoad,
        reinterpret_cast<LPARAM>(result));
}

void SaveProfileOnController(SaveRequest* request)
{
    if (!request)
        return;
    std::wstring error;
    const bool ok = g_storeReady && g_store.SaveProfile(
        request->characterId, request->profileIndex,
        request->profile, error);
    delete request;
    if (!ok)
    {
        if (error.empty())
            error = L"换装数据目录尚未就绪";
        auto* message = new std::wstring(error);
        if (!InstallGameThreadHook() || !g_gameWindow)
            delete message;
        else
            QueueGameCommand(kDispatchStoreError,
                reinterpret_cast<LPARAM>(message));
    }
}

LRESULT CALLBACK ControllerProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
        if (InstallGameThreadHook())
            QueueGameCommand(kDispatchTick);
        return 0;
    case kControllerLoadCharacter:
        LoadCharacterOnController(reinterpret_cast<unsigned int*>(lParam));
        return 0;
    case kControllerSaveProfile:
        SaveProfileOnController(reinterpret_cast<SaveRequest*>(lParam));
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI ControllerThreadProc(LPVOID)
{
    g_controllerThreadId = GetCurrentThreadId();
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = g_module;
    windowClass.lpfnWndProc = ControllerProc;
    windowClass.lpszClassName = kControllerClass;
    RegisterClassExW(&windowClass);

    g_dispatchMessage = RegisterWindowMessageW(kDispatchName);
    Log(L"[配置] 插件已启用，窗口快捷键：%s，派发消息=%u",
        FormatHotkey(g_config.windowHotkey).c_str(), g_dispatchMessage);
    const bool clientReady = equipment_swap::client_api::ValidateClient();
    if (!clientReady)
        Log(L"[客户端] 关键函数签名不匹配，物品读取和换装已禁用");
    else
        Log(L"[客户端] 原生穿戴事务校验通过");
    if (EnsureGameNative() && g_native.postChatNotice)
        Log(L"[客户端] 聊天公告走 GameNative");
    else
        Log(L"[客户端] GameNative 未绑定，换装聊天公告暂不可用");

    const std::wstring contentDirectory = PluginContentDirectory();
    CreateDirectoryW(contentDirectory.c_str(), nullptr);
    const std::wstring layoutFilePath = contentDirectory + L"\\UI\\" +
        kLayoutFileName;
    const bool layoutFileAvailable =
        GetFileAttributesW(layoutFilePath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!layoutFileAvailable)
    {
        Log(L"[原生界面] XUI 部署文件不存在：%s", layoutFilePath.c_str());
    }
    else
    {
        Log(L"[原生界面] XUI 部署文件已找到：%s，资源路径：%s",
            layoutFilePath.c_str(), kLayoutResourcePath);
    }
    int windowId = -1;
    const bool externalUiPrepared = layoutFileAvailable &&
        PrepareExternalUi(layoutFilePath, windowId);
    if (!externalUiPrepared)
    {
        Log(L"[原生界面] 外部 XUI 或窗口 ID 前置条件不满足，未安装窗口工厂分流");
    }
    else if (!equipment_swap::native_ui::Install(kLayoutResourcePath,
        windowId, HandleNativeUiCommand))
    {
        Log(L"[原生界面] 安装 %d 窗口工厂分流失败", windowId);
        RollbackExternalUiPreparation();
    }
    else if (!g_native.registerWindowFactory(kWindowIdOwner,
        &equipment_swap::native_ui::WindowFactory, nullptr))
    {
        Log(L"[原生界面] 向宿主注册 %d 窗口工厂失败", windowId);
        RollbackExternalUiPreparation();
    }
    else
    {
        g_windowFactoryRegistered = true;
        Log(L"[原生界面] 已安装 %d 窗口工厂分流", windowId);
    }

    HWND controller = CreateWindowExW(0, kControllerClass, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, g_module, nullptr);
    if (!controller)
    {
        RollbackExternalUiPreparation();
        return 0;
    }
    g_controllerWindow = controller;

    if (g_config.windowHotkey.IsValid())
    {
        Log(L"[快捷键] 窗口快捷键仅在游戏内生效：%s",
            FormatHotkey(g_config.windowHotkey).c_str());
    }

    std::wstring storeError;
    g_storeReady = g_store.Open(contentDirectory + L"\\" +
        kDataDirectoryName, storeError);
    if (!g_storeReady)
        Log(L"[存储] 打开换装数据目录失败：%s", storeError.c_str());
    else
        Log(L"[存储] 换装数据目录：%s\\%s", contentDirectory.c_str(),
            kDataDirectoryName);

    SetTimer(controller, kControllerTimerId, 50, nullptr);
    InstallGameThreadHook();

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    KillTimer(controller, kControllerTimerId);
    if (g_gameThreadHook)
        UnhookWindowsHookEx(g_gameThreadHook);
    if (g_gameGetMessageHook)
        UnhookWindowsHookEx(g_gameGetMessageHook);
    g_gameThreadHook = nullptr;
    g_gameGetMessageHook = nullptr;
    g_store.Close();
    g_storeReady = false;
    g_controllerWindow = nullptr;
    DestroyWindow(controller);
    return 0;
}

bool LoadPluginConfig()
{
    const std::wstring path = g_moduleDirectory + L"\\" + kConfigFileName;
    g_config.debug = GetPrivateProfileIntW(L"General", L"Debug", 0,
        path.c_str()) != 0;
    g_config.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 0,
        path.c_str()) != 0;

    wchar_t hotkeyText[128] = {};
    GetPrivateProfileStringW(L"General", L"WindowHotkey", L"End",
        hotkeyText, _countof(hotkeyText), path.c_str());
    bool valid = false;
    g_config.windowHotkey = ParseHotkey(hotkeyText, valid);
    if (!valid)
    {
        Log(L"[配置] WindowHotkey 无法识别，改用 End");
        g_config.windowHotkey = { 0, VK_END };
    }
    return true;
}
}

extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit()
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
        return TRUE;
    if (!LoadPluginConfig() || !g_config.enabled)
        return TRUE;

    HANDLE thread = CreateThread(nullptr, 0, ControllerThreadProc,
        nullptr, 0, nullptr);
    if (!thread)
    {
        InterlockedExchange(&g_started, 0);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_module = module;
        g_moduleDirectory = ModuleDirectory(module);
        g_logPath = g_moduleDirectory + L"\\" + kLogFileName;
        ResetProfiles(g_profiles);
    }
    return TRUE;
}
