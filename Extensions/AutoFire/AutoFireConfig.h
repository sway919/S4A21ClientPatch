#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace auto_fire
{
struct KeySpec
{
    WORD virtualKey = 0;
    BYTE scanCode = 0;
    bool extended = false;
};

struct RapidFireBinding
{
    KeySpec key;
    unsigned int intervalMs = 1;
};

struct SequenceAction
{
    KeySpec key;
    unsigned int delayAfterMs = 1;
};

struct ComboBinding
{
    KeySpec trigger;
    bool repeat = true;
    std::vector<SequenceAction> actions;
};

struct MouseBinding
{
    bool enabled = false;
    KeySpec trigger;
    DWORD downFlag = MOUSEEVENTF_LEFTDOWN;
    DWORD upFlag = MOUSEEVENTF_LEFTUP;
    DWORD data = 0;
    unsigned int pressDurationMs = 16;
    unsigned int intervalMs = 10;
};

struct Configuration
{
    bool enabled = true;
    bool foregroundOnly = true;
    bool debug = false;
    unsigned int keyPressDurationMs = 16;
    // 第一下走物理键；按住超过该毫秒才开始连发。官方输入捕获标志为真时暂停。
    unsigned int holdThresholdMs = 0;
    bool hasToggleKey = false;
    KeySpec toggleKey;
    bool hasReloadKey = false;
    KeySpec reloadKey;
    std::vector<RapidFireBinding> rapidFire;
    std::vector<ComboBinding> combos;
    MouseBinding mouse;
};

std::wstring ConfigurationPath(HMODULE module);
Configuration LoadConfiguration(const std::wstring& path);
}
