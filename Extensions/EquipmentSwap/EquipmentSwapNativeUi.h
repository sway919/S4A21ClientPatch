#pragma once

#include <array>
#include <string>

#include <windows.h>

#include "EquipmentSwapModel.h"

namespace equipment_swap::native_ui
{
enum class Command
{
    SelectProfile,
    RecordHotkey,
    CaptureEquipment,
    ExecuteSwap,
    ClearProfile,
    Diagnostic,
};

using CommandHandler = void(*)(Command command, int argument);

struct UiState
{
    int selectedProfile = 0;
    std::wstring currentHotkey;
    bool hotkeyPending = false;
    std::array<int, kSlotCount> itemIds = {};
    std::wstring statusText;
};

bool Install(const wchar_t* layoutPath, int windowId,
    CommandHandler handler);
void Uninstall();
void* __cdecl WindowFactory(int windowId, void* manager, void* context);

void Toggle();
void Close();
bool IsOpen();
void* CurrentWindow();
void Poll();
void Refresh(const UiState& state);
}
