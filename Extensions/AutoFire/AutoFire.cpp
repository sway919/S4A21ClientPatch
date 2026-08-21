#include "AutoFireConfig.h"

#include <windows.h>
#include <mmsystem.h>
#include <process.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../GameNative/GameNativeApi.h"

namespace
{
struct PressState
{
    DWORD lastPressGeneration = 0;
    bool wasDown = false;
};

struct RapidFireState
{
    PressState press;
    DWORD heldSince = 0;
    DWORD nextFireAt = 0;
    bool turboActive = false;
};

struct ComboState
{
    PressState press;
    bool active = false;
    bool restartPending = false;
    size_t actionIndex = 0;
    DWORD nextActionAt = 0;
};

struct KeySnapshot
{
    bool down = false;
    DWORD pressGeneration = 0;
};

HMODULE g_module = nullptr;
HANDLE g_stopEvent = nullptr;
LONG g_started = 0;
std::wstring g_logPath;
bool g_debug = false;

void Log(bool always, const wchar_t* format, ...)
{
    if (!always && !g_debug)
        return;

    wchar_t message[512] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);

    FILE* file = nullptr;
    _wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8");
    if (!file)
        return;
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, message);
    fclose(file);
}

bool IsDue(DWORD now, DWORD target)
{
    return static_cast<LONG>(now - target) >= 0;
}

class KeyboardMonitor
{
public:
    ~KeyboardMonitor()
    {
        Uninstall();
    }

    bool Install(HMODULE module)
    {
        if (m_hook)
            return true;

        s_current = this;
        m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProcedure, module, 0);
        if (!m_hook)
            s_current = nullptr;
        return m_hook != nullptr;
    }

    void Uninstall()
    {
        if (m_hook)
        {
            UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
        }
        if (s_current == this)
            s_current = nullptr;
    }

    KeySnapshot Snapshot(const auto_fire::KeySpec& key) const
    {
        if (!key.virtualKey)
            return {};

        if (m_hook && key.scanCode)
        {
            const PhysicalKeyState& state = m_states[StateIndex(
                key.scanCode, key.extended)];
            if (state.seen)
                return {state.down, state.pressGeneration};
        }

        // 钩子安装失败或尚未观察到该键时，保留异步键态作为兼容回退。
        return {(GetAsyncKeyState(key.virtualKey) & 0x8000) != 0, 0};
    }

private:
    struct PhysicalKeyState
    {
        bool seen = false;
        bool down = false;
        DWORD pressGeneration = 0;
    };

    static size_t StateIndex(BYTE scanCode, bool extended)
    {
        return static_cast<size_t>(scanCode) + (extended ? 0x100u : 0u);
    }

    static LRESULT CALLBACK HookProcedure(int code, WPARAM message, LPARAM parameter)
    {
        if (code == HC_ACTION && s_current)
        {
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
            // 忽略注入事件，避免把连发当成新的物理按下。
            if (!(event->flags & LLKHF_INJECTED) && event->scanCode > 0 &&
                event->scanCode <= 0xFF)
            {
                const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
                if (down || up)
                {
                    PhysicalKeyState& state = s_current->m_states[StateIndex(
                        static_cast<BYTE>(event->scanCode),
                        (event->flags & LLKHF_EXTENDED) != 0)];
                    state.seen = true;
                    if (down && !state.down)
                    {
                        state.down = true;
                        if (++state.pressGeneration == 0)
                            ++state.pressGeneration;
                    }
                    else if (up)
                    {
                        state.down = false;
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, parameter);
    }

    HHOOK m_hook = nullptr;
    PhysicalKeyState m_states[512] = {};
    static KeyboardMonitor* s_current;
};

KeyboardMonitor* KeyboardMonitor::s_current = nullptr;

HWND GameWindow()
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground)
        return nullptr;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId() ? foreground : nullptr;
}

bool IsCurrentProcessForeground()
{
    return GameWindow() != nullptr;
}

ClientPatchGameNativeApi g_native = {};
bool g_nativeBound = false;
bool g_nativeMissLogged = false;
bool g_loadNoticeSent = false;

bool EnsureGameNative()
{
    if (g_nativeBound)
        return true;

    g_nativeBound = ClientPatchBindGameNative(&g_native);
    if (g_nativeBound)
    {
        Log(true, L"gamenative bound=1");
        return true;
    }
    if (!g_nativeMissLogged)
    {
        g_nativeMissLogged = true;
        Log(true, L"gamenative bound=0");
    }
    return false;
}

INT NoticeColor()
{
    return g_native.chatRgb ? g_native.chatRgb(255, 72, 220) : 0;
}

constexpr uintptr_t kPreferredImageBase = 0x00400000;
// 官方输入捕获标志。非 0 表示聊天、搜索、起名等输入态，连发暂停。
constexpr uintptr_t kEditCaptureFlag = 0x04C6D016;

bool OfficialTextInputActive()
{
    const auto* byte = reinterpret_cast<const BYTE*>(
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) +
        (kEditCaptureFlag - kPreferredImageBase));
    if (IsBadReadPtr(byte, 1))
        return false;
    return *byte != 0;
}

void QueueOfficialNotice(const wchar_t* text)
{
    if (!text || !text[0] || !EnsureGameNative() || !g_native.postChatNotice)
        return;
    g_native.postChatNotice(text, NoticeColor());
    Log(true, L"notice queue %s", text);
}

void RequestLoadNotice()
{
    if (g_loadNoticeSent)
        return;
    if (!EnsureGameNative())
        return;
    if (g_native.postLoadNotice)
        g_native.postLoadNotice(L"连发插件已载入", NoticeColor());
    else if (g_native.postChatNotice)
        g_native.postChatNotice(L"连发插件已载入", NoticeColor());
    else
        return;
    g_loadNoticeSent = true;
    Log(true, L"notice load queued");
}

void PumpThreadMessages()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void WaitWithMessagePump(DWORD durationMs)
{
    const DWORD deadline = GetTickCount() + durationMs;
    while (true)
    {
        const DWORD now = GetTickCount();
        if (IsDue(now, deadline))
            break;

        const DWORD remaining = deadline - now;
        const DWORD result = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE,
            remaining, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT)
            break;
        if (result == WAIT_OBJECT_0 + 1)
            PumpThreadMessages();
        else
            break;
    }
}

void SendGameKey(const auto_fire::KeySpec& key, unsigned int pressDurationMs)
{
    if (!key.virtualKey || !key.scanCode)
        return;

    const BYTE virtualKey = static_cast<BYTE>(key.virtualKey);
    const DWORD extended = key.extended ? KEYEVENTF_EXTENDEDKEY : 0;
    const LPARAM scan = static_cast<LPARAM>(key.scanCode) << 16;
    const LPARAM extra = key.extended ? (1 << 24) : 0;
    const HWND window = GameWindow();

    // 真实 VK：更新 GetAsyncKeyState；再 PostMessage，战斗 WndProc 才能收到按键。
    keybd_event(virtualKey, key.scanCode, extended, 0);
    if (window)
        PostMessageW(window, WM_KEYDOWN, key.virtualKey, 1 | scan | extra);
    WaitWithMessagePump(pressDurationMs);
    keybd_event(virtualKey, key.scanCode, extended | KEYEVENTF_KEYUP, 0);
    if (window)
        PostMessageW(window, WM_KEYUP, key.virtualKey,
            1 | scan | extra | (1 << 30) | (1 << 31));
}

class AutoFireRuntime
{
public:
    AutoFireRuntime(HMODULE module, KeyboardMonitor& keyboard)
        : m_configPath(auto_fire::ConfigurationPath(module)), m_keyboard(keyboard)
    {
        Reload(false);
    }

    ~AutoFireRuntime()
    {
        ReleaseMouseButton();
    }

    void Tick()
    {
        const bool foreground = !m_config.foregroundOnly || IsCurrentProcessForeground();

        bool reloadDown = false;
        const bool reloadPressed = m_config.hasReloadKey &&
            UpdatePressState(m_config.reloadKey, m_reloadPress, reloadDown);
        if (foreground && reloadPressed)
        {
            // 配置只在明确按下重载键时替换，工作循环中不持续访问磁盘。
            Reload(true);
            return;
        }

        bool toggleDown = false;
        const bool togglePressed = m_config.hasToggleKey &&
            UpdatePressState(m_config.toggleKey, m_togglePress, toggleDown);
        if (foreground && togglePressed)
        {
            m_enabled = !m_enabled;
            ResetBindings();
            QueueOfficialNotice(m_enabled ? L"连发已开启" : L"连发已关闭");
        }

        RequestLoadNotice();

        if (!foreground || !m_enabled)
        {
            ResetBindings();
            m_textInput = false;
            return;
        }

        const bool textInput = OfficialTextInputActive();
        if (textInput != m_textInput)
        {
            m_textInput = textInput;
            ResetBindings();
            Log(true, L"text-input=%d", textInput ? 1 : 0);
        }
        if (m_textInput)
            return;

        TickRapidFire();
        TickCombos();
        TickMouse();
    }

private:
    void Reload(bool fromHotkey)
    {
        ReleaseMouseButton();
        m_config = auto_fire::LoadConfiguration(m_configPath);
        m_enabled = m_config.enabled;
        m_rapidStates.assign(m_config.rapidFire.size(), {});
        m_comboStates.assign(m_config.combos.size(), {});
        m_mouseNextTransitionAt = 0;
        m_togglePress = {};
        m_reloadPress = {};
        if (m_config.hasToggleKey)
            PrimePressState(m_config.toggleKey, m_togglePress);
        if (m_config.hasReloadKey)
            PrimePressState(m_config.reloadKey, m_reloadPress);
        m_textInput = false;
        PrimeBindingStates();
        g_debug = m_config.debug;
        Log(true, L"reload enabled=%d foregroundOnly=%d rapid=%u combo=%u duration=%u hold=%u",
            m_enabled ? 1 : 0, m_config.foregroundOnly ? 1 : 0,
            static_cast<unsigned>(m_config.rapidFire.size()),
            static_cast<unsigned>(m_config.combos.size()),
            m_config.keyPressDurationMs, m_config.holdThresholdMs);
        if (fromHotkey)
            QueueOfficialNotice(L"连发配置已刷新");
        else
        {
            RequestLoadNotice();
        }
    }

    void ResetBindings()
    {
        for (size_t index = 0; index < m_rapidStates.size(); ++index)
        {
            m_rapidStates[index] = {};
            PrimePressState(m_config.rapidFire[index].key,
                m_rapidStates[index].press);
        }
        for (size_t index = 0; index < m_comboStates.size(); ++index)
        {
            m_comboStates[index] = {};
            PrimePressState(m_config.combos[index].trigger,
                m_comboStates[index].press);
        }
        ReleaseMouseButton();
        m_mouseNextTransitionAt = 0;
        m_mouseHeldSince = 0;
        m_mouseArmed = false;
        m_mousePress = {};
        if (m_config.mouse.enabled)
            PrimePressState(m_config.mouse.trigger, m_mousePress);
    }

    bool UpdatePressState(const auto_fire::KeySpec& key, PressState& state,
        bool& down)
    {
        const KeySnapshot snapshot = m_keyboard.Snapshot(key);
        const bool pressed = snapshot.pressGeneration != state.lastPressGeneration ||
            (snapshot.down && !state.wasDown);
        state.lastPressGeneration = snapshot.pressGeneration;
        state.wasDown = snapshot.down;
        down = snapshot.down;
        return pressed;
    }

    void PrimePressState(const auto_fire::KeySpec& key, PressState& state)
    {
        const KeySnapshot snapshot = m_keyboard.Snapshot(key);
        state.lastPressGeneration = snapshot.pressGeneration;
        state.wasDown = snapshot.down;
    }

    void PrimeBindingStates()
    {
        for (size_t index = 0; index < m_rapidStates.size(); ++index)
            PrimePressState(m_config.rapidFire[index].key, m_rapidStates[index].press);
        for (size_t index = 0; index < m_comboStates.size(); ++index)
            PrimePressState(m_config.combos[index].trigger, m_comboStates[index].press);
        m_mousePress = {};
        if (m_config.mouse.enabled)
            PrimePressState(m_config.mouse.trigger, m_mousePress);
    }

    void TickRapidFire()
    {
        for (size_t index = 0; index < m_config.rapidFire.size(); ++index)
        {
            const auto& binding = m_config.rapidFire[index];
            RapidFireState& state = m_rapidStates[index];
            bool down = false;
            const bool pressed = UpdatePressState(binding.key, state.press, down);
            if (!down)
            {
                state.heldSince = 0;
                state.nextFireAt = 0;
                state.turboActive = false;
                continue;
            }

            const DWORD now = GetTickCount();
            if (pressed)
            {
                // 第一下只走物理键。输入框由官方 4C6D016 挡住。
                state.heldSince = now;
                state.turboActive = false;
                state.nextFireAt = 0;
                continue;
            }
            if (!state.heldSince)
                continue;

            if (!state.turboActive)
            {
                const DWORD threshold = m_config.holdThresholdMs;
                if (threshold != 0 &&
                    (!state.heldSince || !IsDue(now, state.heldSince + threshold)))
                {
                    continue;
                }
                state.turboActive = true;
                state.nextFireAt = now;
                Log(false, L"turbo vk=%u hold=%u interval=%u",
                    binding.key.virtualKey, threshold, binding.intervalMs);
            }

            if (state.turboActive && state.nextFireAt &&
                IsDue(now, state.nextFireAt))
            {
                SendGameKey(binding.key, m_config.keyPressDurationMs);
                state.nextFireAt = GetTickCount() + binding.intervalMs;
            }
        }
    }

    void TickCombos()
    {
        for (size_t index = 0; index < m_config.combos.size(); ++index)
        {
            const auto& binding = m_config.combos[index];
            ComboState& state = m_comboStates[index];
            bool down = false;
            const bool pressed = UpdatePressState(binding.trigger, state.press, down);
            if (!state.active)
            {
                if (!pressed)
                    continue;
                state.active = true;
                state.actionIndex = 0;
                state.nextActionAt = GetTickCount();
            }
            else if (pressed)
            {
                // 当前序列结束前再次物理按下时，排队执行下一轮，短按也不会丢失。
                state.restartPending = true;
            }

            if (!IsDue(GetTickCount(), state.nextActionAt))
                continue;

            const auto& action = binding.actions[state.actionIndex];
            SendGameKey(action.key, m_config.keyPressDurationMs);
            state.nextActionAt = GetTickCount() + action.delayAfterMs;

            bool downAfterSend = false;
            if (UpdatePressState(binding.trigger, state.press, downAfterSend))
                state.restartPending = true;

            ++state.actionIndex;
            if (state.actionIndex == binding.actions.size())
            {
                state.actionIndex = 0;
                if (state.restartPending)
                {
                    state.restartPending = false;
                }
                else if (!binding.repeat || !downAfterSend)
                {
                    state.active = false;
                }
            }
        }
    }

    void TickMouse()
    {
        bool down = false;
        const bool pressed = m_config.mouse.enabled &&
            UpdatePressState(m_config.mouse.trigger, m_mousePress, down);
        if (!m_config.mouse.enabled || !down)
        {
            ReleaseMouseButton();
            m_mouseNextTransitionAt = 0;
            m_mouseHeldSince = 0;
            m_mouseArmed = false;
            return;
        }
        const DWORD now = GetTickCount();
        if (!m_mouseArmed)
        {
            if (pressed)
                m_mouseHeldSince = now;
            if (!m_mouseHeldSince)
                return;
            const DWORD threshold = m_config.holdThresholdMs;
            if (threshold != 0 && !IsDue(now, m_mouseHeldSince + threshold))
                return;
            m_mouseArmed = true;
            m_mouseNextTransitionAt = now;
        }
        if (m_mouseNextTransitionAt && !IsDue(now, m_mouseNextTransitionAt))
            return;

        if (m_mouseButtonDown)
        {
            ReleaseMouseButton();
            m_mouseNextTransitionAt = now + m_config.mouse.intervalMs;
        }
        else
        {
            mouse_event(m_config.mouse.downFlag, 0, 0, m_config.mouse.data, 0);
            m_mouseButtonDown = true;
            m_mouseNextTransitionAt = now + m_config.mouse.pressDurationMs;
        }
    }

    void ReleaseMouseButton()
    {
        if (!m_mouseButtonDown)
            return;

        mouse_event(m_config.mouse.upFlag, 0, 0, m_config.mouse.data, 0);
        m_mouseButtonDown = false;
    }

    std::wstring m_configPath;
    KeyboardMonitor& m_keyboard;
    auto_fire::Configuration m_config;
    bool m_enabled = true;
    bool m_textInput = false;
    PressState m_togglePress;
    PressState m_reloadPress;
    PressState m_mousePress;
    bool m_mouseButtonDown = false;
    bool m_mouseArmed = false;
    DWORD m_mouseNextTransitionAt = 0;
    DWORD m_mouseHeldSince = 0;
    std::vector<RapidFireState> m_rapidStates;
    std::vector<ComboState> m_comboStates;
};

unsigned int __stdcall WorkerThread(void*)
{
    timeBeginPeriod(1);
    // 只提高连发工作线程的调度优先级，不改变整个 DNF 进程的优先级。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // 先建立线程消息队列，再安装低级键盘钩子；钩子回调由本线程的消息循环调度。
    MSG message = {};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_logPath = auto_fire::ConfigurationPath(g_module);
    const size_t slash = g_logPath.find_last_of(L"\\/");
    g_logPath = (slash == std::wstring::npos ? L"AutoFire.log"
        : g_logPath.substr(0, slash + 1) + L"AutoFire.log");
    KeyboardMonitor keyboard;
    const bool hooked = keyboard.Install(g_module);
    Log(true, L"worker start hook=%d", hooked ? 1 : 0);
    {
        AutoFireRuntime runtime(g_module, keyboard);
        while (true)
        {
            const DWORD result = MsgWaitForMultipleObjects(1, &g_stopEvent,
                FALSE, 1, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0)
                break;
            if (result == WAIT_OBJECT_0 + 1)
                PumpThreadMessages();
            runtime.Tick();
        }
    }
    keyboard.Uninstall();

    timeEndPeriod(1);
    return 0;
}
}

extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit()
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
        return TRUE;

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        InterlockedExchange(&g_started, 0);
        return FALSE;
    }

    uintptr_t thread = _beginthreadex(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!thread)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        InterlockedExchange(&g_started, 0);
        return FALSE;
    }

    CloseHandle(reinterpret_cast<HANDLE>(thread));
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH && g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }
    return TRUE;
}
