#define NOMINMAX

#include <windows.h>
#include <imm.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "imm32.lib")

namespace
{
constexpr wchar_t kConfigFileName[] = L"ImeFix.ini";
constexpr wchar_t kLogFileName[] = L"ImeFix.log";
constexpr wchar_t kGameWindowClass[] = L"地下城与勇士";
constexpr UINT kMinimumClientWidth = 640;
constexpr UINT kMinimumClientHeight = 400;
constexpr DWORD kDefaultPollMilliseconds = 250;
constexpr uintptr_t kPreferredImageBase = 0x00400000;
// The fixed addresses below belong to this A21 client image. Refuse to
// install hooks when a different executable layout is running.
constexpr DWORD kExpectedClientTimestamp = 0x58458972;
constexpr DWORD kExpectedClientImageSize = 0x0746E000;
// The chat manager owns the editor used by the in-game chat line. These
// addresses are fixed by the client build and are checked before every call.
constexpr uintptr_t kChatManagerPtr = 0x03A5C9B8;
constexpr uintptr_t kChatEditorOffset = 0x0000CA28;
constexpr uintptr_t kChatEditorVtable = 0x0360F334;
constexpr uintptr_t kChatEditorProcess = 0x0281FBE0;
// The client's native IME dispatcher stores its state in this fixed object.
// It accepts the original window message tuple and updates the game's own
// result/composition buffers before the editor is refreshed.
constexpr uintptr_t kImeStateObject = 0x03A44FCC;
constexpr uintptr_t kImeMessageDispatch = 0x02850410;
constexpr uintptr_t kImeHideCandidate = 0x0284EED0;
constexpr uintptr_t kImeCandidateCount = 0x04C6D020;
constexpr uintptr_t kImeEnterFlag = 0x04C6D010;
constexpr uintptr_t kImeResultBuffer = 0x04C5B608;
constexpr uintptr_t kImeCompositionBuffer = 0x04C5AE08;
constexpr uintptr_t kImeCaptureFlag = 0x04C6D016;
constexpr uintptr_t kSetImeCapture = 0x0284F070;

HMODULE g_module = nullptr;
std::wstring g_moduleDirectory;
std::wstring g_configPath;
std::wstring g_logPath;
HANDLE g_stopEvent = nullptr;
HANDLE g_worker = nullptr;
LONG g_started = 0;

struct WindowHook
{
    HWND window = nullptr;
    WNDPROC original = nullptr;
    DWORD threadId = 0;
};

std::vector<WindowHook> g_hooks;
SRWLOCK g_hookLock = SRWLOCK_INIT;
HWND g_gameWindow = nullptr;

bool g_enabled = true;
bool g_debug = false;
bool g_compositionActive = false;
bool g_resultSeen = false;
bool g_imeReturnForwarded = false;
bool g_suppressNormalReturnKeyup = false;
bool g_suppressNormalReturnChar = false;
bool g_needReturnRepair = false;
bool g_resultCharacterSeen = false;
bool g_resultBridgeConsumed = false;
bool g_inCommittedUiClose = false;
bool g_deferredResultPending = false;

void Log(bool always, const wchar_t* format, ...)
{
    if (!always && !g_debug)
        return;

    wchar_t message[768] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);

    FILE* file = nullptr;
    _wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8");
    if (!file)
        return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds, message);
    fclose(file);
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

void LoadConfig()
{
    g_configPath = g_moduleDirectory + L"\\" + kConfigFileName;
    g_enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1,
        g_configPath.c_str()) != 0;
    g_debug = GetPrivateProfileIntW(L"General", L"Debug", 0,
        g_configPath.c_str()) != 0;
}

uintptr_t ClientAddress(uintptr_t preferred)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));
    if (!base || preferred < kPreferredImageBase)
        return 0;
    return base + (preferred - kPreferredImageBase);
}

bool ReadClientBuildIdentity(DWORD& timestamp, DWORD& imageSize)
{
    const auto* module = reinterpret_cast<const unsigned char*>(
        GetModuleHandleW(nullptr));
    if (!module)
        return false;

    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x100000)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            module + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;

        timestamp = nt->FileHeader.TimeDateStamp;
        imageSize = nt->OptionalHeader.SizeOfImage;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsGameWindow(HWND window)
{
    if (!window || !IsWindowVisible(window) || IsIconic(window) ||
        GetParent(window) || GetWindow(window, GW_OWNER))
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId())
        return false;

    wchar_t className[64] = {};
    if (GetClassNameW(window, className, _countof(className)) <= 0 ||
        wcscmp(className, kGameWindowClass) != 0)
        return false;

    RECT client = {};
    return GetClientRect(window, &client) &&
        client.right >= static_cast<LONG>(kMinimumClientWidth) &&
        client.bottom >= static_cast<LONG>(kMinimumClientHeight);
}

bool IsImeInputWindow(HWND window)
{
    if (!window)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId())
        return false;

    wchar_t className[64] = {};
    if (GetClassNameW(window, className, _countof(className)) <= 0)
        return false;

    // DNF routes text through these hidden windows. The IME and MSCTF helper
    // windows belong to USER32/TSF and must remain untouched.
    return wcscmp(className, kGameWindowClass) == 0 ||
        wcscmp(className, L"DNF_WND_CHAT") == 0 ||
        wcscmp(className, L"DIEmWin") == 0;
}

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter)
{
    if (!IsGameWindow(window))
        return TRUE;
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
}

HWND FindGameWindow()
{
    HWND window = nullptr;
    EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&window));
    return window;
}

WNDPROC HookProc(HWND window)
{
    WNDPROC result = nullptr;
    AcquireSRWLockShared(&g_hookLock);
    for (const WindowHook& hook : g_hooks)
    {
        if (hook.window == window)
        {
            result = hook.original;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hookLock);
    return result;
}

BOOL CALLBACK CollectImeWindowCallback(HWND window, LPARAM parameter)
{
    if (IsImeInputWindow(window))
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
    return TRUE;
}

void CollectImeWindows(HWND primary, std::vector<HWND>& windows)
{
    EnumWindows(CollectImeWindowCallback,
        reinterpret_cast<LPARAM>(&windows));
    if (primary)
    {
        const DWORD threadId = GetWindowThreadProcessId(primary, nullptr);
        if (threadId)
            EnumThreadWindows(threadId, CollectImeWindowCallback,
                reinterpret_cast<LPARAM>(&windows));
        // The custom chat surface is a child window, so it is not returned by
        // EnumWindows/EnumThreadWindows even though it owns the edit state.
        EnumChildWindows(primary, CollectImeWindowCallback,
            reinterpret_cast<LPARAM>(&windows));
    }
}

HWND ImeContextWindow(HWND root)
{
    if (!root)
        return nullptr;

    const DWORD threadId = GetWindowThreadProcessId(root, nullptr);
    GUITHREADINFO info = {sizeof(info)};
    if (!threadId || !GetGUIThreadInfo(threadId, &info))
        return root;

    const HWND focus = info.hwndFocus ? info.hwndFocus : info.hwndActive;
    if (!focus || !IsWindow(focus))
        return root;

    DWORD processId = 0;
    GetWindowThreadProcessId(focus, &processId);
    return processId == GetCurrentProcessId() ? focus : root;
}

HIMC AcquireImeContext(HWND root, HWND& owner)
{
    owner = ImeContextWindow(root);
    HIMC context = ImmGetContext(owner);
    if (!context && owner != root)
    {
        owner = root;
        context = ImmGetContext(owner);
    }
    return context;
}

void CloseImeCandidateWindows(HWND root, HWND contextWindow)
{
    // NI_CLOSECANDIDATE is per candidate list. Old IMM implementations can
    // leave auxiliary lists alive, so close all four standard list slots.
    HIMC context = nullptr;
    HWND owner = contextWindow;
    if (owner)
        context = ImmGetContext(owner);
    if (!context && root && owner != root)
    {
        owner = root;
        context = ImmGetContext(owner);
    }
    if (context)
    {
        for (DWORD index = 0; index != 4; ++index)
            ImmNotifyIME(context, NI_CLOSECANDIDATE, index, 0);
        ImmReleaseContext(owner, context);
    }

    // The default IME window owns the visible candidate popup even when the
    // legacy game window has no HIMC of its own.
    const HWND defaultIme = ImmGetDefaultIMEWnd(owner ? owner : root);
    if (defaultIme)
    {
        for (DWORD index = 0; index != 4; ++index)
            SendMessageW(defaultIme, WM_IME_NOTIFY, IMN_CLOSECANDIDATE,
                static_cast<LPARAM>(index));
    }
}

LRESULT CallOriginal(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
bool HideNativeImeCandidate();
bool DispatchNativeIme(UINT message, WPARAM wParam, LPARAM lParam);
void CloseImeCandidatesOnHookedWindows(HWND primary);

bool ResetGameImeCapture()
{
    __try
    {
        auto* capture = reinterpret_cast<volatile unsigned char*>(
            ClientAddress(kImeCaptureFlag));
        if (!*capture)
            return false;

        using SetCaptureFn = void (__cdecl*)(char);
        reinterpret_cast<SetCaptureFn>(ClientAddress(kSetImeCapture))(0);
        Log(false, L"game ime capture reset flag=%d", *capture);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(true, L"game ime capture reset fault");
        return false;
    }
}

bool IsReadableAddress(uintptr_t address, size_t length)
{
    if (!address || !length)
        return false;

    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory,
        sizeof(memory)) || memory.State != MEM_COMMIT)
        return false;

    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & readable) == 0)
        return false;

    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(
        memory.BaseAddress) + memory.RegionSize;
    return address <= regionEnd && length <= regionEnd - address;
}

bool IsWritableAddress(uintptr_t address, size_t length)
{
    if (!IsReadableAddress(address, length))
        return false;

    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory,
        sizeof(memory)) || memory.State != MEM_COMMIT)
        return false;

    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (memory.Protect & writable) != 0;
}

bool ReadGameByte(uintptr_t preferred, unsigned char& value)
{
    const uintptr_t address = ClientAddress(preferred);
    if (!IsReadableAddress(address, sizeof(value)))
        return false;

    __try
    {
        value = *reinterpret_cast<volatile const unsigned char*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadAddressDword(uintptr_t address, unsigned long& value)
{
    if (!IsReadableAddress(address, sizeof(value)))
        return false;

    __try
    {
        value = *reinterpret_cast<volatile const unsigned long*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadGameDword(uintptr_t preferred, unsigned long& value)
{
    return ReadAddressDword(ClientAddress(preferred), value);
}

bool WriteAddressDword(uintptr_t address, unsigned long value)
{
    if (!IsWritableAddress(address, sizeof(value)))
        return false;

    __try
    {
        *reinterpret_cast<volatile unsigned long*>(address) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteAddressByte(uintptr_t address, unsigned char value)
{
    if (!IsWritableAddress(address, sizeof(value)))
        return false;

    __try
    {
        *reinterpret_cast<volatile unsigned char*>(address) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteGameByte(uintptr_t preferred, unsigned char value)
{
    return WriteAddressByte(ClientAddress(preferred), value);
}

bool AlignImeCursorForReturn()
{
    unsigned long start = 0;
    unsigned long end = 0;
    if (!ReadGameDword(0x04C6D06C, start) ||
        !ReadGameDword(0x04C6D070, end) || start == end || !end)
        return start == end && end != 0;

    if (!WriteAddressDword(ClientAddress(0x04C6D06C), end))
    {
        Log(false, L"ime cursor align failed start=%08lX end=%08lX",
            start, end);
        return false;
    }
    Log(false, L"ime cursor aligned start=%08lX end=%08lX", start, end);
    return true;
}

void RetireNativeCandidateState()
{
    unsigned char active = 0;
    if (!ReadGameByte(0x04C6D013, active) || !active)
        return;

    // Preserve the result buffer (byte_4C6D015) while retiring only the
    // composition/candidate markers that drive the client's own renderer.
    const bool cleared = WriteGameByte(0x04C6D013, 0) &&
        WriteGameByte(0x04C6D017, 0);
    if (cleared)
        WriteGameByte(0x04C6D014, 1);
    Log(false, L"ime candidate state retired active=%d cleared=%d",
        active ? 1 : 0, cleared ? 1 : 0);
}

bool NativeCandidateStateActive()
{
    unsigned char active = 0;
    unsigned long count = 0;
    const bool haveActive = ReadGameByte(0x04C6D013, active);
    const bool haveCount = ReadGameDword(kImeCandidateCount, count);
    return (haveActive && active != 0) || (haveCount && count != 0);
}

void ClearNativeCandidateList()
{
    // A modern TSF commit can end with WM_IME_ENDCOMPOSITION only.  The
    // legacy client then releases its candidate data pointer but leaves the
    // renderer count non-zero, so the old list is painted in later inputs.
    const bool notified = DispatchNativeIme(WM_IME_NOTIFY,
        IMN_CLOSECANDIDATE, 0);
    unsigned long count = 0;
    const bool read = ReadGameDword(kImeCandidateCount, count);
    bool cleared = false;
    if (read && count != 0)
        cleared = WriteAddressDword(ClientAddress(kImeCandidateCount), 0);
    Log(false,
        L"ime native candidate list cleared notified=%d count=%lu cleared=%d",
        notified ? 1 : 0, count, cleared ? 1 : 0);
}

bool GameBufferHasText(uintptr_t preferred)
{
    const uintptr_t address = ClientAddress(preferred);
    if (!IsReadableAddress(address, sizeof(wchar_t)))
        return false;

    __try
    {
        return *reinterpret_cast<volatile const wchar_t*>(address) != L'\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void LogGameImeState(const wchar_t* label)
{
    unsigned char flags[8] = {};
    bool complete = true;
    for (size_t index = 0; index != _countof(flags); ++index)
    {
        if (!ReadGameByte(0x04C6D010 + index, flags[index]))
        {
            complete = false;
            break;
        }
    }
    Log(false,
        L"%s game ime flags=%02X%02X%02X%02X%02X%02X%02X%02X "
        L"result=%d composition=%d cursor=%08lX/%08lX readable=%d",
        label ? label : L"state", flags[0], flags[1], flags[2], flags[3],
        flags[4], flags[5], flags[6], flags[7],
        GameBufferHasText(kImeResultBuffer) ? 1 : 0,
        GameBufferHasText(kImeCompositionBuffer) ? 1 : 0,
        [&]() {
            unsigned long value = 0;
            return ReadGameDword(0x04C6D06C, value) ? value : 0xFFFFFFFFUL;
        }(),
        [&]() {
            unsigned long value = 0;
            return ReadGameDword(0x04C6D070, value) ? value : 0xFFFFFFFFUL;
        }(),
        complete ? 1 : 0);
}

void CloseCommittedImeUi(HWND window)
{
    if (!window || g_inCommittedUiClose)
        return;

    g_inCommittedUiClose = true;
    // Closing candidate lists must not call ImmSetOpenStatus(FALSE): that
    // switches the user's IME to English mode. NI_CLOSECANDIDATE only retires
    // the popup while preserving the current open/conversion mode.
    // The game also keeps the candidate window position in its native IME
    // bridge.  A result-only TSF commit can skip the normal START path that
    // moves that window off-screen, leaving the old candidate surface visible
    // even after WM_IME_ENDCOMPOSITION has retired the composition flags.
    HideNativeImeCandidate();
    ClearNativeCandidateList();
    CloseImeCandidateWindows(window, ImeContextWindow(window));
    CloseImeCandidatesOnHookedWindows(window);
    g_inCommittedUiClose = false;
    Log(false, L"ime committed candidate lists closed hwnd=%p", window);
}

bool DispatchNativeIme(UINT message, WPARAM wParam, LPARAM lParam)
{
    const uintptr_t state = ClientAddress(kImeStateObject);
    const uintptr_t dispatch = ClientAddress(kImeMessageDispatch);
    if (!IsReadableAddress(state, 8) || !IsReadableAddress(dispatch, 1))
    {
        Log(false, L"native ime bridge unavailable state=%p dispatch=%p",
            reinterpret_cast<void*>(state), reinterpret_cast<void*>(dispatch));
        return false;
    }

    __try
    {
        using DispatchFn = int (__thiscall*)(void*, unsigned int,
            unsigned int, int);
        const int result = reinterpret_cast<DispatchFn>(dispatch)(
            reinterpret_cast<void*>(state), static_cast<unsigned int>(message),
            static_cast<unsigned int>(wParam), static_cast<int>(lParam));
        Log(false, L"native ime bridge message=%04X result=%d state=%p",
            message, result, reinterpret_cast<void*>(state));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(true, L"native ime bridge fault message=%04X state=%p", message,
            reinterpret_cast<void*>(state));
        return false;
    }
}

bool HideNativeImeCandidate()
{
    const uintptr_t state = ClientAddress(kImeStateObject);
    const uintptr_t hide = ClientAddress(kImeHideCandidate);
    if (!IsReadableAddress(state, 8) || !IsReadableAddress(hide, 1))
        return false;

    __try
    {
        using HideFn = int (__thiscall*)(void*, int, int);
        const int result = reinterpret_cast<HideFn>(hide)(
            reinterpret_cast<void*>(state), -1000, -1000);
        Log(false, L"native ime candidate hidden result=%d state=%p", result,
            reinterpret_cast<void*>(state));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(true, L"native ime candidate hide fault state=%p",
            reinterpret_cast<void*>(state));
        return false;
    }
}

void* FindChatEditor()
{
    __try
    {
        const uintptr_t managerSlot = ClientAddress(kChatManagerPtr);
        if (!IsReadableAddress(managerSlot, sizeof(void*)))
            return nullptr;

        const uintptr_t manager = *reinterpret_cast<const uintptr_t*>(
            managerSlot);
        if (!IsReadableAddress(manager, kChatEditorOffset + sizeof(void*)))
            return nullptr;

        const uintptr_t editor = *reinterpret_cast<const uintptr_t*>(
            manager + kChatEditorOffset);
        if (!IsReadableAddress(editor, 0x40))
            return nullptr;

        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(editor);
        if (vtable != ClientAddress(kChatEditorVtable) ||
            !IsReadableAddress(vtable + 9 * sizeof(void*), sizeof(void*)))
            return nullptr;
        if (*reinterpret_cast<const uintptr_t*>(
                vtable + 9 * sizeof(void*)) !=
            ClientAddress(kChatEditorProcess))
            return nullptr;
        return reinterpret_cast<void*>(editor);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(true, L"chat editor pointer chain fault");
        return nullptr;
    }
}

bool FlushChatEditor()
{
    void* editor = FindChatEditor();
    if (!editor)
    {
        Log(false, L"chat editor bridge unavailable");
        return false;
    }

    __try
    {
        using ProcessFn = int (__thiscall*)(void*);
        const int result = reinterpret_cast<ProcessFn>(
            ClientAddress(kChatEditorProcess))(editor);
        Log(false, L"chat editor bridge editor=%p result=%d", editor, result);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(true, L"chat editor bridge fault editor=%p", editor);
        return false;
    }
}

void LogChatEditorState(const wchar_t* label)
{
    unsigned long manager = 0;
    unsigned long editor = 0;
    unsigned long active = 0;
    unsigned long cursor = 0;
    unsigned long selection = 0;
    unsigned long editorScene = 0;
    unsigned long currentScene = 0;
    if (ReadGameDword(kChatManagerPtr, manager) &&
        ReadAddressDword(static_cast<uintptr_t>(manager) + kChatEditorOffset,
            editor))
    {
        ReadAddressDword(static_cast<uintptr_t>(editor) + 0x31, active);
        ReadAddressDword(static_cast<uintptr_t>(editor) + 0x360, cursor);
        ReadAddressDword(static_cast<uintptr_t>(editor) + 0x364, selection);
        ReadAddressDword(static_cast<uintptr_t>(editor) + 0xD4, editorScene);
    }
    ReadGameDword(0x03998E28, currentScene);
    Log(false,
        L"%s chat manager=%08lX editor=%08lX active=%08lX cursor=%08lX/%08lX scene=%08lX/%08lX",
        label ? label : L"chat", manager, editor, active, cursor, selection,
        editorScene, currentScene);
}

LONG CompositionStringBytes(HWND window, DWORD index)
{
    HWND contextWindow = nullptr;
    HIMC context = AcquireImeContext(window, contextWindow);
    if (!context)
        return -1;
    const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    ImmReleaseContext(contextWindow, context);
    return bytes;
}

LONG ResultStringBytes(HWND window)
{
    const LONG bytes = CompositionStringBytes(window, GCS_RESULTSTR);
    return bytes > 0 ? bytes : 0;
}

LRESULT CallOriginal(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    WNDPROC original = HookProc(window);
    return original ? CallWindowProcW(original, window, message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
}

void CloseImeCandidatesOnHookedWindows(HWND primary)
{
    std::vector<HWND> windows;
    AcquireSRWLockShared(&g_hookLock);
    for (const WindowHook& hook : g_hooks)
    {
        if (hook.window && IsWindow(hook.window) && hook.window != primary)
            windows.push_back(hook.window);
    }
    ReleaseSRWLockShared(&g_hookLock);

    for (HWND window : windows)
        CloseImeCandidateWindows(window, ImeContextWindow(window));
}

LRESULT ForwardReturnSynchronously(HWND window, LPARAM lParam,
    bool suppressQueuedChar)
{
    if (!window)
        return 0;

    // The result has already been delivered to the game's IME object. Do not
    // call CPS_COMPLETE here: on newer TSF implementations that can generate
    // a second result and erase the text before the editor consumes it.
    // Execute the original procedure for the actual key window exactly once;
    // it is the game's own dispatcher that reaches the chat sender. The
    // synchronous WM_CHAR below is needed because TranslateMessage already
    // ran before this window procedure when the IME consumed the key.
    // A result-only TSF commit can leave the game's result buffer pending
    // until the editor runs. Consume that result before delivering Enter;
    // sub_284F1A0 checks the editor's cursor bounds while setting the private
    // send marker, so doing this afterwards is too late.
    const bool pendingResult = g_deferredResultPending ||
        GameBufferHasText(kImeResultBuffer);
    if (pendingResult && !g_resultBridgeConsumed)
    {
        LogGameImeState(L"return before pending result");
        LogChatEditorState(L"return before pending result");
        FlushChatEditor();
        LogGameImeState(L"return after pending result");
        LogChatEditorState(L"return after pending result");
        CloseCommittedImeUi(window);
    }

    if (pendingResult)
    {
        // END was held back while the result was being consumed so the
        // legacy result buffer could not be cleared too early. Complete that
        // native cleanup now, before Enter, to retire the game's own
        // composition/candidate rendering state.
        DispatchNativeIme(WM_IME_ENDCOMPOSITION, 0, 0);
        LogGameImeState(L"return after native end dispatch");
        RetireNativeCandidateState();
        InvalidateRect(window, nullptr, FALSE);
    }

    // The legacy client keeps a composition selection range in the two
    // pointers below the IME context. A modern TSF commit can leave a stale
    // non-empty range even though its text is already displayed; the native
    // Enter handler refuses to set the send flag until that range is empty.
    if (pendingResult)
        AlignImeCursorForReturn();

    g_needReturnRepair = false;
    g_resultSeen = false;
    const LRESULT gameKey =
        CallOriginal(window, WM_KEYDOWN, VK_RETURN, lParam);
    // The game's IME dispatcher commits chat/send on WM_CHAR(13), not on
    // WM_KEYDOWN. Dispatch it on the same stack when the IME ate the key.
    const LRESULT gameChar =
        CallOriginal(window, WM_CHAR, VK_RETURN, lParam);
    LogGameImeState(L"return after original");
    unsigned char enterFlag = 0;
    const bool originalSetEnter =
        ReadGameByte(kImeEnterFlag, enterFlag) && enterFlag != 0;
    if (!originalSetEnter)
    {
        // Some TSF paths reach the window procedure but never enter the
        // client's dispatcher. Feed the same tuple to that dispatcher so
        // WM_CHAR(13) sets the native send marker exactly once.
        DispatchNativeIme(WM_KEYDOWN, VK_RETURN, lParam);
        DispatchNativeIme(WM_CHAR, VK_RETURN, lParam);
        LogGameImeState(L"return after native");
    }
    if (suppressQueuedChar)
        g_suppressNormalReturnChar = true;

    // WM_CHAR(13) sets the game's private Enter marker. Process it now while
    // the editor and chat manager are known to be alive on this thread.
    FlushChatEditor();
    LogGameImeState(L"return after editor");
    LogChatEditorState(L"return after editor");
    g_deferredResultPending = false;
    g_resultBridgeConsumed = false;

    // Clear a stale private capture only after the game has seen VK_RETURN.
    // Clearing it before dispatch changes the state examined by the native
    // key handler and can suppress the send branch.
    ResetGameImeCapture();
    Log(false,
        L"return forwarded synchronously hwnd=%p key=%ld char=%ld native=%d",
        window, static_cast<long>(gameKey), static_cast<long>(gameChar),
        originalSetEnter ? 0 : 1);
    return gameKey;
}

LRESULT CALLBACK ImeWindowProc(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam)
{
    if (!HookProc(window) || !g_enabled)
        return CallOriginal(window, message, wParam, lParam);

    if (message == WM_IME_STARTCOMPOSITION)
    {
        g_compositionActive = true;
        g_resultSeen = false;
        g_resultCharacterSeen = false;
        g_deferredResultPending = false;
    }
    else if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wParam == VK_RETURN && g_needReturnRepair)
    {
        return ForwardReturnSynchronously(window, lParam, true);
    }

    // Mark a result before calling the old procedure. It may synchronously
    // finish the composition and re-enter this subclass with ENDCOMPOSITION.
    if (message == WM_IME_COMPOSITION && (lParam & GCS_RESULTSTR))
    {
        g_resultSeen = true;
        g_resultCharacterSeen = false;
    }

    // IMM may route Enter through WM_IME_KEYDOWN while the candidate list is
    // active. Commit first and synchronously invoke the equivalent game key.
    if (message == WM_IME_KEYDOWN && wParam == VK_RETURN &&
        g_needReturnRepair)
    {
        ForwardReturnSynchronously(window, lParam, false);
        g_imeReturnForwarded = true;
        Log(false, L"ime return forwarded hwnd=%p", window);
        return 0;
    }

    if (message == WM_IME_KEYUP && wParam == VK_RETURN &&
        g_imeReturnForwarded)
    {
        g_imeReturnForwarded = false;
        g_suppressNormalReturnKeyup = true;
        return 0;
    }

    if (message == WM_KEYUP && wParam == VK_RETURN)
    {
        if (g_imeReturnForwarded)
        {
            g_imeReturnForwarded = false;
            return CallOriginal(window, message, wParam, lParam);
        }
        if (g_suppressNormalReturnKeyup)
        {
            g_suppressNormalReturnKeyup = false;
            return 0;
        }
    }

    if (message == WM_CHAR && wParam == VK_RETURN &&
        g_suppressNormalReturnChar)
    {
        g_suppressNormalReturnChar = false;
        return 0;
    }

    // Capture the state before the legacy END handler clears its IMM context;
    // otherwise an empty composition can be indistinguishable from a fully
    // cleaned one by the time the subclass resumes.
    const LONG endCompositionBytes = message == WM_IME_ENDCOMPOSITION
        ? CompositionStringBytes(window, GCS_COMPSTR)
        : -1;
    const bool endCandidateStateActive = message == WM_IME_ENDCOMPOSITION &&
        NativeCandidateStateActive();

    const bool deferResultEnd = message == WM_IME_ENDCOMPOSITION &&
        !g_resultCharacterSeen &&
        (g_resultSeen || g_deferredResultPending);
    // The legacy window procedure clears the game's result buffer as part of
    // END. For a result-only TSF commit there was no WM_CHAR for the editor
    // to consume, so keep END on this bridge until the repaired Enter path
    // has processed that result.
    const LRESULT result = deferResultEnd
        ? 0
        : CallOriginal(window, message, wParam, lParam);

    if (message == WM_IME_COMPOSITION)
    {
        if (lParam & GCS_RESULTSTR)
        {
            // A compatible IMM32/game-mode input method has already emitted
            // the committed character through the original procedure. Do
            // not bridge the result or touch candidate state in that path.
            if (g_resultCharacterSeen)
            {
                g_resultBridgeConsumed = false;
                g_deferredResultPending = false;
                g_needReturnRepair = false;
                Log(false, L"ime result native character path hwnd=%p",
                    window);
            }
            else
            {
                // No character means the newer TSF path left the result in
                // IMM32/game buffers for the bridge to consume.
                const LONG resultBytes = ResultStringBytes(window);
                const bool gameResultPresent =
                    GameBufferHasText(kImeResultBuffer);
                LogGameImeState(L"result before bridge");
                if (resultBytes > 0 && !gameResultPresent)
                    DispatchNativeIme(message, wParam, lParam);
                const bool bridgeConsumed =
                    (resultBytes > 0 || gameResultPresent) &&
                    FlushChatEditor();
                LogGameImeState(L"result after bridge");
                LogChatEditorState(L"result after bridge");
                g_resultBridgeConsumed = bridgeConsumed;
                g_deferredResultPending = !g_resultCharacterSeen &&
                    (resultBytes > 0 || gameResultPresent ||
                     GameBufferHasText(kImeResultBuffer));
                g_needReturnRepair = !g_resultCharacterSeen &&
                    !bridgeConsumed;
                Log(false,
                    L"ime result hwnd=%p flags=%08llX chars=%d repair=%d",
                    window, static_cast<unsigned long long>(lParam),
                    resultBytes > 0 ? 1 : 0,
                    g_needReturnRepair ? 1 : 0);
            }
        }
        else if (lParam & GCS_COMPSTR)
        {
            g_compositionActive = true;
            // Backspace can leave an empty composition without another
            // candidate update. If the legacy renderer still owns a list,
            // retire that stale state synchronously.
            if (CompositionStringBytes(window, GCS_COMPSTR) == 0 &&
                NativeCandidateStateActive())
            {
                CloseCommittedImeUi(window);
                RetireNativeCandidateState();
                g_compositionActive = false;
                g_needReturnRepair = false;
                InvalidateRect(window, nullptr, FALSE);
                Log(false, L"ime empty composition cleared hwnd=%p", window);
            }
        }
    }
    else if (message == WM_IME_ENDCOMPOSITION)
    {
        // Only result-only commits need repair. A compatible IMM32/game-mode
        // input method delivers a character before END; in that case the
        // original window procedure above is the complete handling path.
        const bool resultWithoutCharacter =
            (g_resultSeen || g_deferredResultPending) &&
            !g_resultCharacterSeen;
        const bool staleCandidateWithoutResult =
            !resultWithoutCharacter && !g_resultCharacterSeen &&
            endCompositionBytes <= 0 && endCandidateStateActive;
        bool nativeEndDispatched = false;
        if (staleCandidateWithoutResult)
        {
            CloseCommittedImeUi(window);
            RetireNativeCandidateState();
            InvalidateRect(window, nullptr, FALSE);
            Log(false, L"ime end cleared stale candidate hwnd=%p", window);
        }
        if (resultWithoutCharacter && !GameBufferHasText(kImeResultBuffer) &&
            !g_deferredResultPending)
        {
            nativeEndDispatched = DispatchNativeIme(message, wParam, lParam);
            CloseCommittedImeUi(window);
        }
        else if (resultWithoutCharacter)
        {
            if (g_resultBridgeConsumed)
            {
                // The result has already gone through the chat editor, so
                // it is safe to run the client's END cleanup immediately.
                // Keeping the repair marker armed below still handles the
                // following Enter without allowing the candidate UI to
                // remain active between messages.
                nativeEndDispatched = DispatchNativeIme(message, wParam, lParam);
                LogGameImeState(L"native ime end after result");
                if (nativeEndDispatched)
                    CloseCommittedImeUi(window);
                RetireNativeCandidateState();
            }
            else
            {
                HideNativeImeCandidate();
                RetireNativeCandidateState();
                Log(false, L"native ime end deferred pending result hwnd=%p",
                    window);
            }
        }
        if (resultWithoutCharacter)
        {
            if (!nativeEndDispatched)
                CloseImeCandidateWindows(window, ImeContextWindow(window));
            CloseImeCandidatesOnHookedWindows(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        g_compositionActive = false;
        g_resultSeen = false;
        // A result-only composition can still leave the next Enter as
        // WM_IME_KEYDOWN. Keep the repair armed even when the editor bridge
        // already consumed the text; the following Enter must still produce
        // the game's WM_CHAR(13) send marker.
        g_needReturnRepair = resultWithoutCharacter;
        Log(false, L"ime end hwnd=%p result=%d chars=%d repair=%d", window,
            resultWithoutCharacter ? 1 : 0,
            g_resultCharacterSeen ? 1 : 0, g_needReturnRepair ? 1 : 0);
        g_resultCharacterSeen = false;
        g_imeReturnForwarded = false;
        g_suppressNormalReturnKeyup = false;
        g_suppressNormalReturnChar = false;
    }

    // Some IMEs deliver committed text through WM_CHAR/WM_IME_CHAR without a
    // GCS_RESULTSTR message. Any character received while a result is marked
    // means the original compatible path already consumed the commit.
    if (message == WM_CHAR || message == WM_IME_CHAR)
    {
        if (g_resultSeen)
        {
            g_resultCharacterSeen = true;
            g_needReturnRepair = false;
        }
        else if (g_compositionActive && wParam >= 0x80)
            g_needReturnRepair = true;
        if (wParam >= 0x80)
            Log(false, L"unicode char hwnd=%p message=%04X value=%04llX",
                window, message, static_cast<unsigned long long>(wParam));
    }

    return result;
}

bool UninstallBridge()
{
    AcquireSRWLockExclusive(&g_hookLock);
    for (const WindowHook& hook : g_hooks)
    {
        if (hook.window && IsWindow(hook.window) &&
            GetWindowLongPtrW(hook.window, GWLP_WNDPROC) ==
                reinterpret_cast<LONG_PTR>(&ImeWindowProc))
        {
            SetWindowLongPtrW(hook.window, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(hook.original));
        }
    }
    g_hooks.clear();
    ReleaseSRWLockExclusive(&g_hookLock);
    g_gameWindow = nullptr;
    g_compositionActive = false;
    g_resultSeen = false;
    g_resultCharacterSeen = false;
    g_resultBridgeConsumed = false;
    g_deferredResultPending = false;
    g_inCommittedUiClose = false;
    g_imeReturnForwarded = false;
    g_suppressNormalReturnKeyup = false;
    g_suppressNormalReturnChar = false;
    g_needReturnRepair = false;
    return true;
}

bool InstallBridge(HWND window)
{
    if (!window || !IsImeInputWindow(window))
        return false;

    AcquireSRWLockShared(&g_hookLock);
    for (const WindowHook& hook : g_hooks)
    {
        if (hook.window == window)
        {
            ReleaseSRWLockShared(&g_hookLock);
            return true;
        }
    }
    ReleaseSRWLockShared(&g_hookLock);

    WNDPROC original = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (!original)
        return false;

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&ImeWindowProc));
    const DWORD error = GetLastError();
    if (!previous && error != ERROR_SUCCESS)
    {
        Log(true, L"window subclass failed hwnd=%p error=%lu", window,
            static_cast<unsigned long>(error));
        return false;
    }

    WindowHook hook;
    hook.window = window;
    hook.original = reinterpret_cast<WNDPROC>(previous);
    hook.threadId = GetWindowThreadProcessId(window, nullptr);
    AcquireSRWLockExclusive(&g_hookLock);
    g_hooks.push_back(hook);
    ReleaseSRWLockExclusive(&g_hookLock);
    if (IsGameWindow(window))
        g_gameWindow = window;
    wchar_t className[64] = {};
    GetClassNameW(window, className, _countof(className));
    Log(true, L"window subclass installed hwnd=%p class=%s tid=%lu original=%p",
        window, className, static_cast<unsigned long>(hook.threadId),
        hook.original);
    return true;
}

DWORD WINAPI WorkerThread(LPVOID)
{
    while (WaitForSingleObject(g_stopEvent, kDefaultPollMilliseconds) ==
        WAIT_TIMEOUT)
    {
        if (!g_enabled)
            continue;
        HWND window = FindGameWindow();
        std::vector<HWND> windows;
        CollectImeWindows(window, windows);
        for (HWND candidate : windows)
            InstallBridge(candidate);
        if (!window && g_gameWindow && !IsWindow(g_gameWindow))
            UninstallBridge();
    }

    UninstallBridge();
    return 0;
}
}

extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit()
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
        return TRUE;

    LoadConfig();
    Log(true, L"init enabled=%d debug=%d", g_enabled ? 1 : 0,
        g_debug ? 1 : 0);
    if (!g_enabled)
        return TRUE;

    DWORD clientTimestamp = 0;
    DWORD clientImageSize = 0;
    if (!ReadClientBuildIdentity(clientTimestamp, clientImageSize) ||
        clientTimestamp != kExpectedClientTimestamp ||
        clientImageSize != kExpectedClientImageSize)
    {
        Log(true,
            L"unsupported client build timestamp=%08lX imageSize=%08lX",
            static_cast<unsigned long>(clientTimestamp),
            static_cast<unsigned long>(clientImageSize));
        g_enabled = false;
        return TRUE;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        Log(true, L"worker event creation failed error=%lu",
            static_cast<unsigned long>(GetLastError()));
        return TRUE;
    }

    g_worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!g_worker)
    {
        Log(true, L"worker creation failed error=%lu",
            static_cast<unsigned long>(GetLastError()));
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return TRUE;
    }
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_module = module;
        g_moduleDirectory = ModuleDirectory(module);
        g_logPath = g_moduleDirectory + L"\\" + kLogFileName;
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        if (g_worker)
        {
            WaitForSingleObject(g_worker, 1000);
            CloseHandle(g_worker);
            g_worker = nullptr;
        }
        if (g_stopEvent)
        {
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
        }
        UninstallBridge();
    }
    return TRUE;
}
