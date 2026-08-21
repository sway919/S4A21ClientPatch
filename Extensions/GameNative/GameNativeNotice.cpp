#include "GameNativeNotice.h"
#include "GameNativeClip.h"
#include "GameNativeHook.h"
#include "GameNativeLog.h"

#include <deque>

namespace
{
constexpr uintptr_t kChatManagerPtr = 0x03A5C9B8;
constexpr uintptr_t kGetChatWindow = 0x00A9AF50;
constexpr uintptr_t kWriteChatMessage = 0x01A60620;
constexpr int kNoticeType = 30;
constexpr UINT kNoticeKickMessage = WM_APP + 80;
constexpr DWORD kLoadNoticeDelayMs = 5000;

struct PendingNotice
{
    wchar_t text[192];
    int color = 0;
    bool load = false;
};

CRITICAL_SECTION g_noticeLock;
bool g_noticeLockReady = false;
std::deque<PendingNotice> g_notices;
HHOOK g_noticeHook = nullptr;
HWND g_noticeWindow = nullptr;
HWND g_noticePumpWindow = nullptr;
DWORD g_noticeThreadId = 0;
LONG g_noticePending = 0;
int g_noticeFaults = 0;
DWORD g_noticeRetryAt = 0;
DWORD g_chatReadyAt = 0;

bool IsDue(DWORD now, DWORD target)
{
    return static_cast<LONG>(now - target) >= 0;
}

void EnsureNoticeLock()
{
    if (g_noticeLockReady)
        return;
    InitializeCriticalSection(&g_noticeLock);
    g_noticeLockReady = true;
}

struct NoticeGameWindowBest
{
    HWND window = nullptr;
    long area = 0;
};

BOOL CALLBACK FindGameClassWindow(HWND window, LPARAM parameter)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || !IsWindowVisible(window))
        return TRUE;

    wchar_t className[64] = {};
    GetClassNameW(window, className, _countof(className));
    if (wcscmp(className, L"地下城与勇士") != 0)
        return TRUE;

    RECT client = {};
    if (!GetClientRect(window, &client))
        return TRUE;
    const long area = (client.right - client.left) *
        (client.bottom - client.top);
    auto* best = reinterpret_cast<NoticeGameWindowBest*>(parameter);
    if (area > best->area)
    {
        best->window = window;
        best->area = area;
    }
    return TRUE;
}

HWND ClientGameWindow()
{
    NoticeGameWindowBest best;
    EnumWindows(FindGameClassWindow, reinterpret_cast<LPARAM>(&best));
    HWND window = best.window;
    if (window)
        return window;

    window = GetForegroundWindow();
    if (!window)
        return nullptr;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId() ? window : nullptr;
}

void* OfficialChatWindow()
{
    void** managerPtr = reinterpret_cast<void**>(
        GameNativeClientAddress(kChatManagerPtr));
    if (IsBadReadPtr(managerPtr, sizeof(void*)))
        return nullptr;

    void* manager = *managerPtr;
    if (!manager || IsBadReadPtr(manager, 0x50))
        return nullptr;

    void* chat = reinterpret_cast<void* (__thiscall*)(void*)>(
        GameNativeClientAddress(kGetChatWindow))(manager);
    if (!chat || IsBadReadPtr(chat, 0x20))
        return nullptr;
    return chat;
}

int CallOfficialNotice(void* chat, const wchar_t* text, int color)
{
    using WriteFn = int (__thiscall*)(void*, const wchar_t*, int, int, int, int,
        unsigned char, unsigned char);
    return reinterpret_cast<WriteFn>(GameNativeClientAddress(kWriteChatMessage))(
        chat, text, color, kNoticeType, 0, 0, 0, 0);
}

bool SafeOfficialNotice(void* chat, const wchar_t* text, int color)
{
    __try
    {
        CallOfficialNotice(chat, text, color);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void DrainOfficialNotices()
{
    if (g_noticePending <= 0)
        return;

    const DWORD now = GetTickCount();
    if (g_noticeRetryAt && !IsDue(now, g_noticeRetryAt))
        return;

    EnsureNoticeLock();
    EnterCriticalSection(&g_noticeLock);
    if (g_notices.empty())
    {
        InterlockedExchange(&g_noticePending, 0);
        LeaveCriticalSection(&g_noticeLock);
        return;
    }

    void* chat = OfficialChatWindow();
    if (!chat)
    {
        g_chatReadyAt = 0;
        g_noticeRetryAt = now + 250;
        LeaveCriticalSection(&g_noticeLock);
        return;
    }
    if (!g_chatReadyAt)
        g_chatReadyAt = now;

    const bool loadDue = IsDue(now, g_chatReadyAt + kLoadNoticeDelayMs);
    std::deque<PendingNotice> pending;
    std::deque<PendingNotice> waiting;
    while (!g_notices.empty())
    {
        PendingNotice notice = g_notices.front();
        g_notices.pop_front();
        if (notice.load && !loadDue)
            waiting.push_back(notice);
        else
            pending.push_back(notice);
    }
    g_notices.swap(waiting);
    if (!g_notices.empty())
    {
        InterlockedExchange(&g_noticePending, 1);
        g_noticeRetryAt = g_chatReadyAt + kLoadNoticeDelayMs;
    }
    else
    {
        InterlockedExchange(&g_noticePending, 0);
        g_noticeRetryAt = 0;
    }
    LeaveCriticalSection(&g_noticeLock);

    if (pending.empty())
        return;

    std::deque<PendingNotice> retry;
    for (const PendingNotice& notice : pending)
    {
        if (!SafeOfficialNotice(chat, notice.text, notice.color))
        {
            ++g_noticeFaults;
            if (g_noticeFaults < 3)
                retry.push_back(notice);
            g_noticeRetryAt = now + 2000;
            GameNativeLog(L"notice fault=%d", g_noticeFaults);
            break;
        }
        g_noticeFaults = 0;
        g_noticeRetryAt = 0;
    }

    if (!retry.empty())
    {
        EnsureNoticeLock();
        EnterCriticalSection(&g_noticeLock);
        while (!retry.empty())
        {
            g_notices.push_front(retry.back());
            retry.pop_back();
            InterlockedIncrement(&g_noticePending);
        }
        LeaveCriticalSection(&g_noticeLock);
    }
}

LRESULT CALLBACK NoticeGetMessageHook(int code, WPARAM message, LPARAM parameter)
{
    if (code == HC_ACTION)
    {
        const auto* msg = reinterpret_cast<const MSG*>(parameter);
        if (g_noticePending > 0)
            DrainOfficialNotices();
        GameNativeRefreshPluginClips();
    }
    return CallNextHookEx(g_noticeHook, code, message, parameter);
}

void EnsureNoticeHook()
{
    HWND window = ClientGameWindow();
    if (!window)
        return;

    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    if (!threadId)
        return;

    if (g_noticeHook && g_noticeWindow && IsWindow(g_noticeWindow) &&
        g_noticeThreadId == threadId)
        return;

    if (g_noticeHook)
    {
        UnhookWindowsHookEx(g_noticeHook);
        g_noticeHook = nullptr;
    }

    g_noticeWindow = window;
    g_noticeThreadId = threadId;
    g_noticeHook = SetWindowsHookExW(WH_GETMESSAGE, NoticeGetMessageHook,
        nullptr, threadId);
}

void KickNoticeThread()
{
    if (g_noticeWindow && IsWindow(g_noticeWindow))
        PostMessageW(g_noticeWindow, kNoticeKickMessage, 0, 0);
}

void ArmNoticeDelivery()
{
    EnsureNoticeHook();
    if (g_noticePending > 0)
        KickNoticeThread();
}

LRESULT CALLBACK NoticePumpWndProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TIMER && g_noticePending > 0)
        ArmNoticeDelivery();
    return DefWindowProcW(window, message, wParam, lParam);
}

void EnsureNoticePump()
{
    if (g_noticePumpWindow && IsWindow(g_noticePumpWindow))
        return;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = NoticePumpWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"ClientPatchGameNativeNoticePump";
    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    g_noticePumpWindow = CreateWindowExW(0, windowClass.lpszClassName,
        L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance,
        nullptr);
    if (g_noticePumpWindow)
        SetTimer(g_noticePumpWindow, 1, 250, nullptr);
}

BOOL EnqueueNotice(LPCWSTR text, INT color, bool load)
{
    if (!text || !text[0])
        return FALSE;

    PendingNotice notice = {};
    wcsncpy_s(notice.text, text, _TRUNCATE);
    notice.color = color;
    notice.load = load;

    EnsureNoticeLock();
    EnterCriticalSection(&g_noticeLock);
    g_notices.push_back(notice);
    InterlockedIncrement(&g_noticePending);
    LeaveCriticalSection(&g_noticeLock);

    EnsureNoticePump();
    ArmNoticeDelivery();
    return TRUE;
}
}

void GameNativeArmGameThreadPump()
{
    EnsureNoticeHook();
}

void GameNativeStartNotice()
{
    EnsureNoticeLock();
    EnsureNoticePump();
}

void GameNativeStopNotice()
{
    if (g_noticePumpWindow)
    {
        KillTimer(g_noticePumpWindow, 1);
        DestroyWindow(g_noticePumpWindow);
        g_noticePumpWindow = nullptr;
    }
    if (g_noticeHook)
    {
        UnhookWindowsHookEx(g_noticeHook);
        g_noticeHook = nullptr;
    }
    g_noticeWindow = nullptr;
    g_noticeThreadId = 0;
    if (g_noticeLockReady)
    {
        EnterCriticalSection(&g_noticeLock);
        g_notices.clear();
        InterlockedExchange(&g_noticePending, 0);
        g_chatReadyAt = 0;
        LeaveCriticalSection(&g_noticeLock);
    }
}

BOOL GameNativeChatReady()
{
    const BOOL ready = OfficialChatWindow() != nullptr ? TRUE : FALSE;
    if (ready || g_noticePending > 0)
        ArmNoticeDelivery();
    return ready;
}

BOOL GameNativePostChatNotice(LPCWSTR text, INT color)
{
    return EnqueueNotice(text, color, false);
}

BOOL GameNativePostLoadNotice(LPCWSTR text, INT color)
{
    return EnqueueNotice(text, color, true);
}

INT GameNativeChatRgb(INT red, INT green, INT blue)
{
    return static_cast<INT>(0xFF000000u |
        (static_cast<unsigned>(blue & 0xFF) << 16) |
        (static_cast<unsigned>(green & 0xFF) << 8) |
        static_cast<unsigned>(red & 0xFF));
}
