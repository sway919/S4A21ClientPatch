#define NOMINMAX
#include "EquipmentSwapNativeUi.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace equipment_swap::native_ui
{
namespace
{
constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t kAllocateAddress = 0x027C42E0;
constexpr uintptr_t kWideStringConstructorAddress = 0x0042B800;
// wstring::assign(const wchar_t*, size)。42B800 构造成功后走这里写入内容。
constexpr uintptr_t kWideStringAssignAddress = 0x0042ACB0;
constexpr uintptr_t kWindowConstructorAddress = 0x01A26E80;
constexpr uintptr_t kControlLookupAddress = 0x01A26BB0;
constexpr uintptr_t kWindowManagerGlobalAddress = 0x03A5C9B8;
// 02304C70：OpenWindow thunk，thiscall(manager, windowId, 0, 0)。
// 023060F0 只 get-or-create，不登记显示。
constexpr uintptr_t kOpenWindowAddress = 0x02304C70;
constexpr uintptr_t kCloseWindowAddress = 0x02304790;
constexpr uintptr_t kHasWindowAddress = 0x02301820;
constexpr uintptr_t kBaseEventVtableAddress = 0x032F9D24;
constexpr uintptr_t kItemControlVtableAddress = 0x031F5864;
constexpr uintptr_t kItemControlDrawAddress = 0x00EF4060;
constexpr uintptr_t kItemControlSetItemAddress = 0x00EF35D0;
constexpr uintptr_t kItemControlRebuildAddress = 0x00EF3E20;
constexpr size_t kWindowSize = 0x1A8;
constexpr size_t kEventVtableOffset = 0x17C;
constexpr int kMaxWindowId = 838;
constexpr int kButtonClickEvent = 13;

constexpr int kProfileTabGroup = 100;
constexpr int kProfileTabFirst = 101;
constexpr int kRecordHotkeyButton = 205;
constexpr int kSaveHotkeyButton = 206;
constexpr int kCaptureEquipmentButton = 202;
constexpr int kExecuteSwapButton = 203;
constexpr int kClearProfileButton = 204;
constexpr int kCurrentHotkeyLabel = 20;
constexpr int kStatusLabel = 50;
constexpr int kSlotControlFirst = 60;
constexpr size_t kItemControlDrawVtableSlot = 9;
// A21 CNUIControlText 虚表 0x0360881C。槽 54 是 0 参 getter
//（`mov eax,[ecx+0x358]; ret`），不能当写字函数调用。
// 按钮等控件写字走虚表 +220（槽 55）；文本控件虚表在槽 54 结束。
// 显示字符串在 this+0x110，绘制对象在 this+0xE8。
constexpr uintptr_t kControlTextVtableAddress = 0x0360881C;
constexpr uintptr_t kControlTextDrawAddress = 0x027CC060;
constexpr uintptr_t kTextLayoutVtableAddress = 0x0360FDBC;
constexpr uintptr_t kTextLayoutDtorAddress = 0x0282B8A0;
constexpr size_t kControlTextDrawSlot = 9;
constexpr size_t kControlTextWriteSlot = 55;
constexpr size_t kControlTextStringOffset = 0x110;
constexpr size_t kControlTextLayoutOffset = 0xE8;
constexpr size_t kControlVisibleSlot = 2;

using AllocateFn = void* (__cdecl*)(size_t size);
using WideStringConstructorFn = void* (__thiscall*)(void* value,
    const wchar_t* text);
using WideStringAssignFn = void* (__thiscall*)(void* value,
    const wchar_t* text, unsigned int length);
using WindowConstructorFn = void* (__thiscall*)(void* window, void* manager,
    int windowId, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7);
using OpenWindowFn = void* (__thiscall*)(void* manager, int windowId,
    int argument, int flag);
using CloseWindowFn = int(__thiscall*)(void* manager, int windowId,
    int reason, int argument);
using HasWindowFn = bool(__thiscall*)(void* manager, int windowId);
using ControlLookupFn = void* (__thiscall*)(void* window, void* result,
    int controlId);
using ControlTextWriteFn = int(__thiscall*)(void* control,
    const wchar_t* text);
using ControlVisibleFn = int(__thiscall*)(void* control, bool visible);
using ItemControlSetItemFn = int(__thiscall*)(void* control, int itemId);
using ItemControlRebuildFn = int(__thiscall*)(void* control, int unused);

uintptr_t ClientAddress(uintptr_t preferredAddress);
bool IsPlausibleWindow(void* window);

CommandHandler g_handler = nullptr;
std::wstring g_layoutPath;
int g_windowId = -1;
void* g_window = nullptr;
// 工厂返回值走全局，避免 Release + __try 把 EAX 弄成 EXCEPTION_EXECUTE_HANDLER(1)。
void* g_factoryWindow = nullptr;
void* g_eventVtable[1] = {};
bool g_installed = false;
bool g_textAssignReady = false;
UiState g_pendingState;
bool g_hasPendingState = false;
unsigned char g_textDiagnosticStages[256] = {};

struct ClientSharedPtr
{
    void* object = nullptr;
    void* controlBlock = nullptr;
};

struct SlotControlBinding
{
    ClientSharedPtr itemControl;
    int itemId = -1;
};

std::array<SlotControlBinding, kSlotCount> g_slotControls = {};
std::array<unsigned char, kSlotCount> g_slotDiagnosticStages = {};
bool g_slotsReadyReported = false;
std::wstring g_lastHotkeyText;
std::wstring g_lastStatusText;
int g_lastEditVisible = -1;
int g_lastSaveVisible = -1;

bool GetControl(int controlId, ClientSharedPtr& result);
void Dispatch(Command command, int argument);
const wchar_t* ClientWideCStr(void* value);
bool IsClientCodeAddress(const void* pointer);
bool LooksLikeThiscallOneArg(const void* pointer);
void ReportTextDiagnostic(int controlId, int stage);

bool MatchesVtable(void* object, uintptr_t vtableAddress,
    size_t slot, uintptr_t functionAddress)
{
    if (!object)
        return false;
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(object);
        return reinterpret_cast<uintptr_t>(vtable) ==
                ClientAddress(vtableAddress) &&
            reinterpret_cast<uintptr_t>(vtable[slot]) ==
                ClientAddress(functionAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

const wchar_t* ClientWideCStr(void* value)
{
    if (!value)
        return L"";
    auto* fields = static_cast<uint32_t*>(value);
    if (fields[6] < 8)
        return reinterpret_cast<const wchar_t*>(fields + 1);
    return reinterpret_cast<const wchar_t*>(fields[1]);
}

bool IsClientCodeAddress(const void* pointer)
{
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return address >= base + 0x1000 && address < base + 0x03C00000;
}

bool LooksLikeThiscallOneArg(const void* pointer)
{
    if (!IsClientCodeAddress(pointer))
        return false;
    bool matched = false;
    __try
    {
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        if (bytes[0] != 0x55 || bytes[1] != 0x8B || bytes[2] != 0xEC)
            return false;
        for (int index = 3; index < 96; ++index)
        {
            if (bytes[index] == 0xC2 && bytes[index + 1] == 0x04 &&
                bytes[index + 2] == 0x00)
            {
                matched = true;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        matched = false;
    }
    return matched;
}

void ReportTextDiagnostic(int controlId, int stage)
{
    if (controlId < 0 || controlId >= 256 || stage < 0 || stage > 7)
        return;
    const unsigned char mask = static_cast<unsigned char>(1u << stage);
    if (g_textDiagnosticStages[controlId] & mask)
        return;
    g_textDiagnosticStages[controlId] =
        static_cast<unsigned char>(g_textDiagnosticStages[controlId] | mask);
    Dispatch(Command::Diagnostic, 2000 + stage * 100 + controlId);
}

void ReleaseClientSharedPtr(ClientSharedPtr& value)
{
    void* block = value.controlBlock;
    value.object = nullptr;
    value.controlBlock = nullptr;
    if (!block)
        return;
    __try
    {
        auto* uses = reinterpret_cast<volatile long*>(
            static_cast<unsigned char*>(block) + 4);
        if (InterlockedDecrement(uses) == 0)
        {
            auto** vtable = *reinterpret_cast<void***>(block);
            reinterpret_cast<void(__thiscall*)(void*)>(vtable[0])(block);
            auto* weaks = reinterpret_cast<volatile long*>(
                static_cast<unsigned char*>(block) + 8);
            if (InterlockedDecrement(weaks) == 0)
                reinterpret_cast<void(__thiscall*)(void*)>(vtable[1])(block);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void ReleaseSlotControls()
{
    for (SlotControlBinding& binding : g_slotControls)
    {
        ReleaseClientSharedPtr(binding.itemControl);
        binding.itemId = -1;
    }
    g_slotDiagnosticStages.fill(0);
    g_slotsReadyReported = false;
    g_lastHotkeyText.clear();
    g_lastStatusText.clear();
    g_lastEditVisible = -1;
    g_lastSaveVisible = -1;
    std::memset(g_textDiagnosticStages, 0, sizeof(g_textDiagnosticStages));
}

bool BindSlotControl(int slotIndex)
{
    if (!g_window || slotIndex < 0 || slotIndex >= kSlotCount)
        return false;

    SlotControlBinding& binding = g_slotControls[slotIndex];
    if (binding.itemControl.object)
        return true;

    ClientSharedPtr itemControl;
    if (!GetControl(kSlotControlFirst + slotIndex, itemControl) ||
        !itemControl.object)
    {
        ReleaseClientSharedPtr(itemControl);
        if (g_slotDiagnosticStages[slotIndex] != 4)
        {
            g_slotDiagnosticStages[slotIndex] = 4;
            Dispatch(Command::Diagnostic,
                1100 + kSlotControlFirst + slotIndex);
        }
        return false;
    }
    binding.itemControl = itemControl;
    itemControl = {};
    g_slotDiagnosticStages[slotIndex] = 0;
    return true;
}

int TabIndexFromControl(int controlId)
{
    const int index = controlId - kProfileTabFirst;
    if (index >= 0 && index < kProfileCount)
        return index;
    return -1;
}

bool GetControl(int controlId, ClientSharedPtr& result)
{
    result = {};
    if (!g_window)
        return false;
    __try
    {
        auto lookup = reinterpret_cast<ControlLookupFn>(
            ClientAddress(kControlLookupAddress));
        lookup(g_window, &result, controlId);
        return result.object != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = {};
        return false;
    }
}

bool ApplySlotIcons(const UiState& state)
{
    if (!g_window)
        return false;

    bool allBound = true;
    for (int index = 0; index < kSlotCount; ++index)
    {
        if (!BindSlotControl(index))
        {
            allBound = false;
            continue;
        }

        SlotControlBinding& binding = g_slotControls[index];
        const int itemId = state.itemIds[index] > 0 ?
            state.itemIds[index] : 0;
        if (binding.itemId == itemId)
            continue;
        __try
        {
            void* control = binding.itemControl.object;
            if (!MatchesVtable(control, kItemControlVtableAddress,
                    kItemControlDrawVtableSlot, kItemControlDrawAddress))
            {
                allBound = false;
                if (g_slotDiagnosticStages[index] != 7)
                {
                    g_slotDiagnosticStages[index] = 7;
                    Dispatch(Command::Diagnostic, 5000 + index);
                }
                continue;
            }

            auto setItem = reinterpret_cast<ItemControlSetItemFn>(
                ClientAddress(kItemControlSetItemAddress));
            auto rebuild = reinterpret_cast<ItemControlRebuildFn>(
                ClientAddress(kItemControlRebuildAddress));
            setItem(control, itemId > 0 ? itemId : -1);
            rebuild(control, 0);
            binding.itemId = itemId;
            // 空槽 ItemControl 仍参与绘制；setItem(-1)+rebuild 后对空槽隐藏。
            auto** vtable = *reinterpret_cast<void***>(control);
            auto setVisible = reinterpret_cast<ControlVisibleFn>(
                vtable[kControlVisibleSlot]);
            setVisible(control, itemId > 0);
            g_slotDiagnosticStages[index] = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            allBound = false;
            if (g_slotDiagnosticStages[index] != 7)
            {
                g_slotDiagnosticStages[index] = 7;
                Dispatch(Command::Diagnostic, 5000 + index);
            }
        }
    }
    if (allBound && !g_slotsReadyReported)
    {
        g_slotsReadyReported = true;
        Dispatch(Command::Diagnostic, 4000 + kSlotCount);
    }
    return allBound;
}

bool SetControlText(int controlId, const std::wstring& text)
{
    ClientSharedPtr control;
    if (!GetControl(controlId, control) || !control.object)
    {
        ReportTextDiagnostic(controlId, 0);
        return false;
    }

    bool updated = false;
    __try
    {
        auto* bytes = static_cast<unsigned char*>(control.object);
        auto** vtable = *reinterpret_cast<void***>(control.object);
        const bool isTextControl = MatchesVtable(control.object,
            kControlTextVtableAddress, kControlTextDrawSlot,
            kControlTextDrawAddress);

        if (isTextControl && g_textAssignReady)
        {
            auto assign = reinterpret_cast<WideStringAssignFn>(
                ClientAddress(kWideStringAssignAddress));
            void* value = bytes + kControlTextStringOffset;
            assign(value, text.c_str(),
                static_cast<unsigned int>(text.size()));
            void* layout = bytes + kControlTextLayoutOffset;
            if (MatchesVtable(layout, kTextLayoutVtableAddress, 0,
                    kTextLayoutDtorAddress))
            {
                *reinterpret_cast<const wchar_t**>(
                    static_cast<unsigned char*>(layout) + 8) =
                    ClientWideCStr(value);
                *reinterpret_cast<uint32_t*>(
                    static_cast<unsigned char*>(layout) + 12) = 1;
            }
            if (*reinterpret_cast<void**>(bytes + 0x350))
                ReportTextDiagnostic(controlId, 4);
            if (reinterpret_cast<uintptr_t>(vtable[13]) ==
                ClientAddress(0x027C9200))
            {
                reinterpret_cast<void(__thiscall*)(void*)>(
                    vtable[13])(control.object);
            }
            updated = true;
            ReportTextDiagnostic(controlId, 1);
        }
        else
        {
            void* write = vtable[kControlTextWriteSlot];
            if (LooksLikeThiscallOneArg(write))
            {
                auto setText = reinterpret_cast<ControlTextWriteFn>(write);
                setText(control.object, text.c_str());
                updated = true;
                ReportTextDiagnostic(controlId, 2);
            }
        }

        if (!updated)
            ReportTextDiagnostic(controlId, 3);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        updated = false;
        ReportTextDiagnostic(controlId, 3);
    }
    ReleaseClientSharedPtr(control);
    return updated;
}

bool SetControlVisible(int controlId, bool visible)
{
    ClientSharedPtr control;
    if (!GetControl(controlId, control) || !control.object)
        return false;
    bool updated = false;
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(control.object);
        auto setVisible = reinterpret_cast<ControlVisibleFn>(
            vtable[kControlVisibleSlot]);
        setVisible(control.object, visible);
        updated = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        updated = false;
    }
    ReleaseClientSharedPtr(control);
    return updated;
}

void ApplyPendingState()
{
    if (!g_window || !g_hasPendingState)
        return;

    const UiState state = g_pendingState;
    bool hotkeyUpdated = state.currentHotkey == g_lastHotkeyText;
    if (!hotkeyUpdated && SetControlText(kCurrentHotkeyLabel,
        state.currentHotkey))
    {
        g_lastHotkeyText = state.currentHotkey;
        hotkeyUpdated = true;
    }

    const int editVisible = state.hotkeyPending ? 0 : 1;
    bool editButtonUpdated = editVisible == g_lastEditVisible;
    if (!editButtonUpdated &&
        SetControlVisible(kRecordHotkeyButton, editVisible != 0))
    {
        g_lastEditVisible = editVisible;
        editButtonUpdated = true;
    }

    const int saveVisible = state.hotkeyPending ? 1 : 0;
    bool saveButtonUpdated = saveVisible == g_lastSaveVisible;
    if (!saveButtonUpdated &&
        SetControlVisible(kSaveHotkeyButton, saveVisible != 0))
    {
        g_lastSaveVisible = saveVisible;
        saveButtonUpdated = true;
    }

    bool statusUpdated = state.statusText == g_lastStatusText;
    if (!statusUpdated && SetControlText(kStatusLabel, state.statusText))
    {
        g_lastStatusText = state.statusText;
        statusUpdated = true;
    }

    const bool slotsBound = ApplySlotIcons(state);
    g_hasPendingState = !(hotkeyUpdated && editButtonUpdated &&
        saveButtonUpdated && statusUpdated && slotsBound);
}

uintptr_t ClientAddress(uintptr_t preferredAddress)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base + (preferredAddress - kPreferredImageBase);
}

bool IsPlausibleWindow(void* window)
{
    return reinterpret_cast<uintptr_t>(window) >= 0x10000;
}

bool ValidateBytes(uintptr_t preferredAddress,
    const unsigned char* expected, size_t length)
{
    const auto* address = reinterpret_cast<const unsigned char*>(
        ClientAddress(preferredAddress));
    __try
    {
        for (size_t index = 0; index < length; ++index)
        {
            if (address[index] != expected[index])
                return false;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* WindowManager()
{
    __try
    {
        return *reinterpret_cast<void**>(
            ClientAddress(kWindowManagerGlobalAddress));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void Dispatch(Command command, int argument = 0)
{
    if (g_handler)
        g_handler(command, argument);
}

int __fastcall HandleControlEvent(void*, void*, int controlId, int eventType,
    int argument, int)
{
    const int tabIndex = TabIndexFromControl(controlId);
    if (eventType != kButtonClickEvent)
        return 0;
    if (tabIndex >= 0 && tabIndex != g_pendingState.selectedProfile)
        Dispatch(Command::SelectProfile, tabIndex);
    else if (controlId == kProfileTabGroup &&
        argument >= 0 && argument < kProfileCount &&
        argument != g_pendingState.selectedProfile)
    {
        Dispatch(Command::SelectProfile, argument);
    }

    switch (controlId)
    {
    case kRecordHotkeyButton:
    case kSaveHotkeyButton:
        Dispatch(Command::RecordHotkey);
        break;
    case kCaptureEquipmentButton:
        Dispatch(Command::CaptureEquipment);
        break;
    case kExecuteSwapButton:
        Dispatch(Command::ExecuteSwap);
        break;
    case kClearProfileButton:
        Dispatch(Command::ClearProfile);
        break;
    default:
        break;
    }
    return 0;
}

void* CreatePluginWindow(void* manager)
{
    auto allocate = reinterpret_cast<AllocateFn>(
        ClientAddress(kAllocateAddress));
    auto constructString = reinterpret_cast<WideStringConstructorFn>(
        ClientAddress(kWideStringConstructorAddress));
    auto constructWindow = reinterpret_cast<WindowConstructorFn>(
        ClientAddress(kWindowConstructorAddress));

    void* window = allocate(kWindowSize);
    if (!window)
    {
        Dispatch(Command::Diagnostic, 2);
        return nullptr;
    }

    std::array<uint32_t, 8> layoutString = {};
    constructString(layoutString.data(), g_layoutPath.c_str());
    constructWindow(window, manager, g_windowId,
        layoutString[0], layoutString[1], layoutString[2], layoutString[3],
        layoutString[4], layoutString[5], layoutString[6], layoutString[7]);

    auto* bytes = static_cast<unsigned char*>(window);
    *reinterpret_cast<void**>(bytes + kEventVtableOffset) = g_eventVtable;
    g_window = window;
    g_factoryWindow = window;
    ApplyPendingState();
    Dispatch(Command::Diagnostic, 3);
    return window;
}

}

void* __cdecl WindowFactory(int windowId, void* manager, void*)
{
    if (!g_installed || windowId != g_windowId)
        return nullptr;

    Dispatch(Command::Diagnostic, 1);
    g_factoryWindow = nullptr;
    CreatePluginWindow(manager);
    return g_factoryWindow;
}

bool Install(const wchar_t* layoutPath, int windowId,
    CommandHandler handler)
{
    if (g_installed)
        return windowId == g_windowId;
    if (!layoutPath || !*layoutPath || windowId < 0 ||
        windowId > kMaxWindowId || !handler)
        return false;

    static constexpr unsigned char kAllocate[] = {
        0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08,
    };
    static constexpr unsigned char kWideString[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    static constexpr unsigned char kWindowCtor[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    static constexpr unsigned char kGetControl[] = {
        0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x89, 0x84, 0x01, 0x00, 0x00,
    };
    static constexpr unsigned char kOpen[] = {
        0xE9,
    };
    static constexpr unsigned char kHas[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x3D, 0x8B, 0x03, 0x00, 0x00,
    };
    static constexpr unsigned char kSetItem[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x89, 0x81, 0x50, 0x03, 0x00, 0x00,
    };
    static constexpr unsigned char kRebuild[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    static constexpr unsigned char kBaseEvent[] = {
        0x33, 0xC0, 0xC2, 0x10, 0x00,
    };
    static constexpr unsigned char kWideStringAssign[] = {
        0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4D, 0x08,
    };

    if (!ValidateBytes(kAllocateAddress, kAllocate, sizeof(kAllocate)) ||
        !ValidateBytes(kWideStringConstructorAddress, kWideString,
            sizeof(kWideString)) ||
        !ValidateBytes(kWindowConstructorAddress, kWindowCtor,
            sizeof(kWindowCtor)) ||
        !ValidateBytes(kControlLookupAddress, kGetControl,
            sizeof(kGetControl)) ||
        !ValidateBytes(kOpenWindowAddress, kOpen, sizeof(kOpen)) ||
        !ValidateBytes(kHasWindowAddress, kHas, sizeof(kHas)) ||
        !ValidateBytes(kItemControlSetItemAddress, kSetItem,
            sizeof(kSetItem)) ||
        !ValidateBytes(kItemControlRebuildAddress, kRebuild,
            sizeof(kRebuild)))
        return false;

    bool handlerOk = false;
    __try
    {
        const auto handler = *reinterpret_cast<const unsigned char**>(
            ClientAddress(kBaseEventVtableAddress));
        handlerOk = handler &&
            handler[0] == kBaseEvent[0] && handler[1] == kBaseEvent[1] &&
            handler[2] == kBaseEvent[2] && handler[3] == kBaseEvent[3] &&
            handler[4] == kBaseEvent[4];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        handlerOk = false;
    }
    if (!handlerOk)
        return false;

    g_textAssignReady = ValidateBytes(kWideStringAssignAddress,
        kWideStringAssign, sizeof(kWideStringAssign));
    std::memset(g_textDiagnosticStages, 0, sizeof(g_textDiagnosticStages));

    g_eventVtable[0] = reinterpret_cast<void*>(&HandleControlEvent);
    g_layoutPath = layoutPath;
    g_windowId = windowId;
    g_handler = handler;
    g_installed = true;
    if (!g_textAssignReady)
        Dispatch(Command::Diagnostic, 2500);
    return true;
}

void Uninstall()
{
    if (!g_installed || g_window)
        return;
    ReleaseSlotControls();
    g_layoutPath.clear();
    g_windowId = -1;
    g_handler = nullptr;
    g_installed = false;
}

bool IsOpen()
{
    if (!g_installed)
        return false;
    void* manager = WindowManager();
    if (!manager)
        return false;
    __try
    {
        auto hasWindow = reinterpret_cast<HasWindowFn>(
            ClientAddress(kHasWindowAddress));
        const bool open = hasWindow(manager, g_windowId);
        if (!open)
        {
            ReleaseSlotControls();
            g_window = nullptr;
        }
        return open;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ReleaseSlotControls();
        g_window = nullptr;
        return false;
    }
}

void Poll()
{
    if (!g_installed || !g_window || !IsOpen())
        return;
    if (g_hasPendingState)
        ApplyPendingState();
}

void* CurrentWindow()
{
    return g_window;
}

void Refresh(const UiState& state)
{
    if (state.selectedProfile != g_pendingState.selectedProfile)
    {
        for (SlotControlBinding& binding : g_slotControls)
            binding.itemId = -1;
    }
    g_pendingState = state;
    g_hasPendingState = true;
    if (g_window)
        ApplyPendingState();
}

void Toggle()
{
    if (!g_installed)
        return;
    if (IsOpen())
    {
        Close();
        return;
    }

    void* manager = WindowManager();
    if (!manager)
    {
        Dispatch(Command::Diagnostic, 4);
        return;
    }
    __try
    {
        auto openWindow = reinterpret_cast<OpenWindowFn>(
            ClientAddress(kOpenWindowAddress));
        auto hasWindow = reinterpret_cast<HasWindowFn>(
            ClientAddress(kHasWindowAddress));
        void* opened = openWindow(manager, g_windowId, 0, 0);
        if (IsPlausibleWindow(opened))
            g_window = opened;
        else if (!IsPlausibleWindow(g_window))
            g_window = nullptr;
        if (g_window)
            ApplyPendingState();
        Dispatch(Command::Diagnostic, g_window ? 6 : 5);
        Dispatch(Command::Diagnostic, hasWindow(manager, g_windowId) ? 8 : 9);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ReleaseSlotControls();
        g_window = nullptr;
        Dispatch(Command::Diagnostic, 7);
    }
}

void Close()
{
    if (!g_installed)
        return;

    ReleaseSlotControls();
    void* manager = WindowManager();
    if (!manager)
    {
        g_window = nullptr;
        return;
    }
    __try
    {
        auto closeWindow = reinterpret_cast<CloseWindowFn>(
            ClientAddress(kCloseWindowAddress));
        closeWindow(manager, g_windowId, -1, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_window = nullptr;
    g_factoryWindow = nullptr;
}
}
