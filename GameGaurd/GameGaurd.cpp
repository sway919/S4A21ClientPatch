#include "mem.h"

static void StaticPatches()
{
    // --- Anti-cheat bypass ---

    // #3-4: parseCommandLineChina — skip TCLS mode init (JZ→JMP+NOP)
    mem::patch(0x0E0D04C, { 0xE9, 0x6B, 0x06, 0x00, 0x00, 0x90 });

    // #7: NOTIFUNC_CHANNELINFO — skip encrypted key section (JZ→JMP)
    mem::patch(0x1169661, { 0xEB });

    // #10: QQSafeStorage::Init — bypass (JNZ→NOP)
    mem::patch(0x0143E97A, { 0x90, 0x90 });

    // #33: startup init — bypass validation check (JZ→NOP)
    mem::patch(0x02C6C8D4, { 0x90, 0x90 });

    // #37: minimize all windows
    mem::patch(0x01203BBA, { 0xEB });

    // #fix wWinMain Check
    mem::patch(0x02793BF2, { 0xEB, 0x0A });

    // #fix TopLevelExceptionFilter Check & Event
    mem::patch(0x015479E3, { 0x75, 0x67 });

    // --- Version/signature compatibility ---

    // #1: SetCodePage GBK(936)→UTF-8(65001)
    mem::patch(0x00426BE1, { 0xE9, 0xFD });

    // #21: bypass version signature 0x1B492 check (JZ→JMP)
    mem::patch(0x01A73BBC, { 0xEB });

    // #22: skip type 0x8A event processing (JNZ→JMP)
    mem::patch(0x02304DCB, { 0xEB });

    // #23: skip game state verification (JZ→JMP)
    mem::patch(0x0231467D, { 0xE9, 0xA2, 0x00 });

    // --- Code fix / runtime patch ---

    // restore prologue destroyed by packer
    mem::patch(0x0274E6D8, { 0xE8, 0x33, 0x55, 0x51, 0x00, 0xA1, 0xE0, 0xCE, 0x99, 0x03, 0x31, 0xE8, 0x89, 0x45, 0xFC, 0xA1, 0xE4, 0xCE, 0xB7, 0x03 });

    // NOP the fixed-key XOR decrypt in NetworkProc (server sends plaintext)
    mem::patch(0x0163BDAB, { 0x90, 0x90, 0x90, 0x90, 0x90 });

    // n5 check entries → return 1 (pass)
    mem::patch(0x00E0CBF0, { 0x31, 0xC0, 0x40, 0xC2, 0x04, 0x00 });
    mem::patch(0x0163C650, { 0x31, 0xC0, 0x40, 0xC2, 0x04, 0x00 });
    mem::patch(0x0163D8B0, { 0x31, 0xC0, 0x40, 0xC2, 0x04, 0x00 });

    // Patch encrypted string at 0x035E997C to decrypted "az.dll"
    // Original is encrypted; overwrite with decrypted form so GetOrDecryptWideString
    // returns "az.dll" → GetModuleHandleW finds our az.dll → CreateObj works
    mem::patch(0x035E997C, {
        0xA1, 0x2C, 0x00, 0x00,                                     // header (decrypted flag)
        0x61, 0x00, 0x7A, 0x00, 0x2E, 0x00, 0x64, 0x00,           // "az.dll" UTF-16LE
        0x6C, 0x00, 0x6C, 0x00, 0x00, 0x00                         // + null terminator
        });
}

#define STAGE_OFFSET     0x15E218
#define STAGE_SIZE       0xAF000
#define STAGE_WPOS_OFF   (STAGE_OFFSET + 0xAF000)
#define STAGE_CS_OFF     (STAGE_OFFSET + 0xAF008)

static int __fastcall Hook_ISocketWrite(void* this_ptr, void* /*edx*/, char* buffer, int size)
{
    char* base = (char*)this_ptr;
    if (!*(base + 8)) return -2147483564;
    if (!buffer || size <= 0) return -1;

    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)(base + STAGE_CS_OFF);
    DWORD* pWpos = (DWORD*)(base + STAGE_WPOS_OFF);

    EnterCriticalSection(cs);
    DWORD wp = *pWpos;
    if ((unsigned)size >= STAGE_SIZE - wp)
    {
        LeaveCriticalSection(cs);
        return -2;
    }
    memcpy(base + STAGE_OFFSET + wp, buffer, size);
    *pWpos = wp + size;
    LeaveCriticalSection(cs);
    return 0;
}

int __fastcall Proxy_CipherEncrypt(void* This, void* NotUsed, int packet_type, char* input, int in_size, char* out_put, int* out_size)
{
    // A21 outbound header is 14 bytes; write total size at header+3.
    *(int*)(input - 14 + 3) = in_size + 14;

    *out_size = in_size;
    memcpy(out_put, input, in_size);
    return 1;
}

int __fastcall Proxy_CipherDecrypt(void* This, void* NotUsed, int packet_type, char* input, int in_size, char* out_put, int* out_size)
{
    *out_size = in_size;
    memcpy(out_put, input, in_size);
    return 1;
}

void PatchS4A16()
{
    StaticPatches();

    // Cipher::Encrypt → memcpy passthrough
    mem::jmphook(0x026B7D90, reinterpret_cast<uintptr_t>(Proxy_CipherEncrypt));

    // Cipher::Decrypt → memcpy passthrough
    mem::jmphook(0x026B7E50, reinterpret_cast<uintptr_t>(Proxy_CipherDecrypt));

    // nengine::ISocket::Write — staging append (reversed from MakePacket/TCP_SEND)
    mem::jmphook(0x05790FE9, reinterpret_cast<uintptr_t>(Hook_ISocketWrite));
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        PatchS4A16();
    }
    return TRUE;
}
