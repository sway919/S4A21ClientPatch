#pragma once

#include <windows.h>

namespace multi_instance
{
class InlineHook
{
public:
    bool Install(void* target, void* detour, void** trampolineOut)
    {
        if (!target || !detour || !trampolineOut)
            return false;

        SIZE_T patchLength = 0;
        const BYTE* targetBytes = reinterpret_cast<const BYTE*>(target);
        while (patchLength < 5)
            patchLength += InstructionLength(targetBytes + patchLength);
        if (patchLength < 5)
            return false;

        BYTE* trampoline = reinterpret_cast<BYTE*>(VirtualAlloc(
            nullptr, patchLength + 5, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline)
            return false;

        memcpy(trampoline, target, patchLength);
        trampoline[patchLength] = 0xE9;
        *reinterpret_cast<DWORD*>(trampoline + patchLength + 1) =
            static_cast<DWORD>(reinterpret_cast<BYTE*>(target) + patchLength -
                (trampoline + patchLength + 5));

        DWORD oldProtection = 0;
        if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE,
            &oldProtection))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        BYTE* writableTarget = reinterpret_cast<BYTE*>(target);
        writableTarget[0] = 0xE9;
        *reinterpret_cast<DWORD*>(writableTarget + 1) =
            static_cast<DWORD>(reinterpret_cast<BYTE*>(detour) -
                (writableTarget + 5));
        for (SIZE_T i = 5; i < patchLength; ++i)
            writableTarget[i] = 0x90;

        FlushInstructionCache(GetCurrentProcess(), target, patchLength);
        VirtualProtect(target, patchLength, oldProtection, &oldProtection);
        *trampolineOut = trampoline;
        return true;
    }

private:
    static SIZE_T InstructionLength(const BYTE* code)
    {
        const BYTE opcode = code[0];
        if (opcode == 0x55 || opcode == 0x56 || opcode == 0x57 ||
            opcode == 0x53 || opcode == 0x5D || opcode == 0x5E ||
            opcode == 0x5F || opcode == 0x50 || opcode == 0x51 ||
            opcode == 0x52 || opcode == 0x58 || opcode == 0x59 ||
            opcode == 0x5A || opcode == 0x90 || opcode == 0xCC)
            return 1;
        if (opcode == 0x8B &&
            (code[1] == 0xEC || code[1] == 0xFF ||
                (code[1] & 0xC0) == 0xC0))
            return 2;
        if (opcode == 0x89 && (code[1] & 0xC0) == 0xC0)
            return 2;
        if (opcode == 0x83 && (code[1] == 0xEC || code[1] == 0xE4))
            return 3;
        if (opcode == 0x81 && (code[1] == 0xEC || code[1] == 0xE4))
            return 6;
        if (opcode == 0x6A || opcode == 0xEB)
            return 2;
        if (opcode == 0x68 || opcode == 0xE8 || opcode == 0xE9 ||
            (opcode >= 0xB8 && opcode <= 0xBF))
            return 5;
        if (opcode == 0xFF && code[1] == 0x25)
            return 6;
        return 1;
    }
};
}
