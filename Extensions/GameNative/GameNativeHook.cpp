#define NOMINMAX
#include "GameNativeHook.h"

#include <cstring>

#include <windows.h>

namespace
{
constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr size_t kHookLength = 5;
}

uintptr_t GameNativeClientAddress(uintptr_t preferredAddress)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));
    return base + (preferredAddress - kPreferredImageBase);
}

bool GameNativeIsReadable(uintptr_t address, size_t length)
{
    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory,
        sizeof(memory)))
        return false;
    if (memory.State != MEM_COMMIT ||
        memory.AllocationBase != GetModuleHandleW(nullptr))
        return false;

    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) +
        memory.RegionSize;
    return address <= regionEnd && length <= regionEnd - address;
}

bool GameNativeValidateBytes(uintptr_t address, const unsigned char* expected,
    size_t length)
{
    if (!expected || !GameNativeIsReadable(address, length))
        return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(address);
    for (size_t index = 0; index < length; ++index)
    {
        if (bytes[index] != expected[index])
            return false;
    }
    return true;
}

bool GameNativeInstallJump(uintptr_t targetAddress, void* detour,
    void** trampoline)
{
    if (!targetAddress || !detour || !trampoline ||
        !GameNativeIsReadable(targetAddress, kHookLength))
        return false;

    auto* target = reinterpret_cast<unsigned char*>(targetAddress);
    auto* gateway = static_cast<unsigned char*>(VirtualAlloc(nullptr,
        kHookLength + kHookLength, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!gateway)
        return false;

    std::memcpy(gateway, target, kHookLength);
    gateway[kHookLength] = 0xE9;
    *reinterpret_cast<int32_t*>(gateway + kHookLength + 1) =
        static_cast<int32_t>((target + kHookLength) -
            (gateway + kHookLength + kHookLength));

    DWORD oldProtection = 0;
    if (!VirtualProtect(target, kHookLength, PAGE_EXECUTE_READWRITE,
        &oldProtection))
    {
        VirtualFree(gateway, 0, MEM_RELEASE);
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(detour) - (target + kHookLength));
    FlushInstructionCache(GetCurrentProcess(), target, kHookLength);
    DWORD ignored = 0;
    VirtualProtect(target, kHookLength, oldProtection, &ignored);
    *trampoline = gateway;
    return true;
}
