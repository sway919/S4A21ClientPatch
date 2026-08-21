#pragma once

#include <cstddef>
#include <cstdint>

uintptr_t GameNativeClientAddress(uintptr_t preferredAddress);
bool GameNativeIsReadable(uintptr_t address, size_t length);
bool GameNativeValidateBytes(uintptr_t address, const unsigned char* expected,
    size_t length);
bool GameNativeInstallJump(uintptr_t address, void* detour, void** trampoline);
