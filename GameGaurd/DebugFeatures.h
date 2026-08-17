#pragma once

#include <windows.h>

// GameGaurd.ini [Debug] Enabled=1 turns on GameLog and CipherPacket tracing.
namespace debug_features
{
void Apply(HMODULE module);
void LogCipherPacket(const wchar_t* direction, int packetType,
    const char* input, int inSize, const char* output, int outSize);
}
