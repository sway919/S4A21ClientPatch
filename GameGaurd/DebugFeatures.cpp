#include "DebugFeatures.h"
#include "mem.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
constexpr wchar_t kConfigName[] = L"GameGaurd.ini";
constexpr uintptr_t kGameLogRva = 0x023BE270;    // IDA VA 0x027BE270
constexpr uintptr_t kGameLogExRva = 0x023BE370;  // IDA VA 0x027BE370
constexpr size_t kPacketDumpBytes = 160;
constexpr size_t kHexBytesPerLine = 16;

bool g_enabled = false;
CRITICAL_SECTION g_logLock;
bool g_logLockReady = false;
wchar_t g_gameLogPath[MAX_PATH] = {};
wchar_t g_cipherPacketPath[MAX_PATH] = {};

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

void MakeClientLogPath(wchar_t* dest, size_t destCount, const wchar_t* fileName)
{
    const DWORD length = GetModuleFileNameW(nullptr, dest, static_cast<DWORD>(destCount));
    if (!length || length >= destCount)
    {
        wcsncpy_s(dest, destCount, fileName, _TRUNCATE);
        return;
    }

    wchar_t* slash = wcsrchr(dest, L'\\');
    if (!slash)
        slash = wcsrchr(dest, L'/');
    if (slash)
        wcsncpy_s(slash + 1, destCount - (slash + 1 - dest), fileName, _TRUNCATE);
    else
        wcsncpy_s(dest, destCount, fileName, _TRUNCATE);
}

void EnsureLogLock()
{
    if (g_logLockReady)
        return;
    InitializeCriticalSection(&g_logLock);
    g_logLockReady = true;
}

void AppendLogLine(const wchar_t* path, const wchar_t* line)
{
    if (!path || !path[0] || !line)
        return;

    EnsureLogLock();
    EnterCriticalSection(&g_logLock);
    FILE* file = nullptr;
    _wfopen_s(&file, path, L"a, ccs=UTF-8");
    if (file)
    {
        fwprintf(file, L"%s\n", line);
        fclose(file);
    }
    LeaveCriticalSection(&g_logLock);
}

void AppendLogFormat(const wchar_t* path, const wchar_t* format, ...)
{
    wchar_t buffer[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    AppendLogLine(path, buffer);
}

std::wstring HexToWString(const unsigned char* data, size_t length)
{
    std::wstring text;
    text.reserve(length * 3);
    wchar_t byteText[4] = {};
    for (size_t i = 0; i < length; ++i)
    {
        swprintf_s(byteText, L"%02X", data[i]);
        if (i)
            text.push_back(L' ');
        text += byteText;
    }
    return text;
}

uintptr_t DnfBase()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DNF.exe"));
    return base ? base : 0x00400000;
}

void __cdecl ProxyGameLog(int /*a1*/, wchar_t* /*sourcePath*/, wchar_t* functionName,
    int logType, wchar_t* format, ...)
{
    if (!g_enabled || !format)
        return;

    wchar_t stackBuffer[512] = {};
    wchar_t* dynamicBuffer = nullptr;
    wchar_t* output = stackBuffer;

    va_list args;
    va_start(args, format);
    int result = _vswprintf_c_l(stackBuffer, _countof(stackBuffer), format, nullptr, args);
    if (result < 0)
    {
        va_end(args);
        va_start(args, format);
        const int needed = _vscwprintf_l(format, nullptr, args) + 1;
        if (needed > 0)
        {
            dynamicBuffer = static_cast<wchar_t*>(malloc(needed * sizeof(wchar_t)));
            if (dynamicBuffer)
            {
                va_end(args);
                va_start(args, format);
                _vswprintf_c_l(dynamicBuffer, needed, format, nullptr, args);
                output = dynamicBuffer;
            }
        }
    }
    va_end(args);

    const wchar_t* name = functionName ? functionName : L"";
    AppendLogFormat(g_gameLogPath, L"[%s] [%d] [%s]", name, logType, output);

    if (dynamicBuffer)
        free(dynamicBuffer);
}

void LoadConfig(HMODULE module)
{
    const std::wstring iniPath = ModuleDirectory(module) + L"\\" + kConfigName;
    g_enabled = GetPrivateProfileIntW(L"Debug", L"Enabled", 0, iniPath.c_str()) != 0;
}

void InstallGameLogHooks()
{
    MakeClientLogPath(g_gameLogPath, _countof(g_gameLogPath), L"GameLog.log");
    DeleteFileW(L"GameLog.log");
    DeleteFileW(g_gameLogPath);

    const uintptr_t base = DnfBase();
    mem::jmphook(base + kGameLogRva, reinterpret_cast<uintptr_t>(ProxyGameLog));
    mem::jmphook(base + kGameLogExRva, reinterpret_cast<uintptr_t>(ProxyGameLog));
    AppendLogLine(g_gameLogPath, L"[GameGaurd] A21 GameLog hook installed.");
}

void PrepareCipherPacketLog()
{
    MakeClientLogPath(
        g_cipherPacketPath, _countof(g_cipherPacketPath), L"CipherPacket.log");
    DeleteFileW(L"CipherPacket.log");
    DeleteFileW(g_cipherPacketPath);
    AppendLogLine(g_cipherPacketPath, L"[GameGaurd] CipherPacket log enabled.");
}
}

namespace debug_features
{
void Apply(HMODULE module)
{
    LoadConfig(module);
    if (!g_enabled)
        return;
    InstallGameLogHooks();
    PrepareCipherPacketLog();
}

void LogCipherPacket(const wchar_t* direction, int packetType,
    const char* input, int inSize, const char* output, int outSize)
{
    if (!g_enabled || !g_cipherPacketPath[0])
        return;

    const size_t rawDump = static_cast<size_t>(outSize > 0 ? outSize : 0);
    const size_t bodyDump = static_cast<size_t>(inSize > 0 ? inSize : 0);
    const size_t rawBytes = rawDump < kPacketDumpBytes ? rawDump : kPacketDumpBytes;
    const size_t bodyBytes = bodyDump < kPacketDumpBytes ? bodyDump : kPacketDumpBytes;

    EnsureLogLock();
    EnterCriticalSection(&g_logLock);
    FILE* file = nullptr;
    _wfopen_s(&file, g_cipherPacketPath, L"a, ccs=UTF-8");
    if (file)
    {
        fwprintf(file, L"%s packet_type=0x%04X body_size=%d raw_size=%d\n",
            direction ? direction : L"?", packetType, inSize, outSize);

        auto writeHex = [&](const wchar_t* label, const char* data, size_t nbytes)
        {
            if (!data || nbytes == 0 || IsBadReadPtr(data, nbytes))
                return;
            fwprintf(file, L"%s first %u bytes:\n", label,
                static_cast<unsigned int>(nbytes));
            for (size_t offset = 0; offset < nbytes; offset += kHexBytesPerLine)
            {
                const size_t chunk = nbytes - offset < kHexBytesPerLine
                    ? nbytes - offset
                    : kHexBytesPerLine;
                fwprintf(file, L"%s\n",
                    HexToWString(reinterpret_cast<const unsigned char*>(data + offset),
                        chunk).c_str());
            }
        };

        writeHex(L"body", input, bodyBytes);
        writeHex(L"raw", output, rawBytes);
        fclose(file);
    }
    LeaveCriticalSection(&g_logLock);
}
}
