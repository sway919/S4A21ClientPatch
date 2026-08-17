#include "mem.h"
#include "DebugFeatures.h"
#include "PluginLoader.h"
#include <intrin.h>

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

    // #fix wWinMain Check
    mem::patch(0x02793BF2, { 0xEB, 0x0A });

    // #fix TopLevelExceptionFilter Check & Event
    mem::patch(0x015479E3, { 0x75, 0x67 });

    // --- Version/signature compatibility ---

    // #1: SetCodePage GBK(936)→UTF-8(65001)
    mem::patch(0x00426BE1, { 0xE9, 0xFD });

    // #21: bypass version signature 0x1B492 check (JZ→JMP)
    mem::patch(0x01A73BBC, { 0xEB });

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

    // #22: skip type 0x8A event processing (JNZ→JMP) — required for the
    // personal warehouse window to open with our server.
    mem::patch(0x02304DCB, { 0xEB });

    // Patch encrypted string at 0x035E997C to decrypted "az.dll"
    // Original is encrypted; overwrite with decrypted form so GetOrDecryptWideString
    // returns "az.dll" → GetModuleHandleW finds our az.dll → CreateObj works
    mem::patch(0x035E997C, {
        0xA1, 0x2C, 0x00, 0x00,                                     // header (decrypted flag)
        0x61, 0x00, 0x7A, 0x00, 0x2E, 0x00, 0x64, 0x00,           // "az.dll" UTF-16LE
        0x6C, 0x00, 0x6C, 0x00, 0x00, 0x00                         // + null terminator
        });

    // --- Channel select: force manual selection (debug aid) ---

    // CNSelectChannelModule window branch (sub_1329B10): JZ→JMP so the tick
    // always takes the show-SelectChannelWindow(id 70) path at loc_1329B58
    // instead of calling AutoSelectChannel().
    // NOTE: the "pending auto channel" gate sub_10BCE20 must stay intact —
    // GotoChannelSelectInit needs it to enter the channel-select module for
    // in-game channel switching; forcing it to 0 breaks that flow.
    mem::patch(0x1329B4B, { 0xEB });
}

// nengine::ISocket::Write — full port of the official client original
// (RingBufferPushBytes). Send staging ring at this+0x15E218:
//   ring+0x000000  byte buffer[0xAF000]
//   ring+0x0AF000  wpos (producer index)
//   ring+0x0AF004  rpos (consumer index, advanced by the untouched pop/flush side)
//   ring+0x0AF008  CRITICAL_SECTION
// Invariant: wpos==rpos means EMPTY, so push must never let wpos catch rpos;
// the ring holds at most 0xAF000-1 bytes. Error codes and the wrapper's
// return convention (1 / 0x800000xx) match the original because A21 callers
// may branch on them.
#define STAGE_OFFSET     0x15E218
#define STAGE_SIZE       0xAF000
#define STAGE_WPOS_OFF   (STAGE_OFFSET + STAGE_SIZE)
#define STAGE_RPOS_OFF   (STAGE_WPOS_OFF + 4)
#define STAGE_CS_OFF     (STAGE_WPOS_OFF + 8)

static int StageRingPush(char* ring, const char* src, int size)
{
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)(ring + STAGE_SIZE + 8);
    DWORD* pWpos = (DWORD*)(ring + STAGE_SIZE);
    DWORD* pRpos = (DWORD*)(ring + STAGE_SIZE + 4);

    EnterCriticalSection(cs);
    if ((unsigned)size >= STAGE_SIZE)
    {
        LeaveCriticalSection(cs);
        return -1;
    }

    DWORD wp = *pWpos;
    DWORD rp = *pRpos;
    if (wp < rp)
    {
        if (size >= (int)(rp - wp))
        {
            LeaveCriticalSection(cs);
            return -4;
        }
        memcpy(ring + wp, src, size);
        *pWpos = wp + size;
        LeaveCriticalSection(cs);
        return 0;
    }

    DWORD tail = STAGE_SIZE - wp;  // room from wpos to the end
    if (size < (int)tail)
    {
        memcpy(ring + wp, src, size);
        *pWpos = wp + size;
        LeaveCriticalSection(cs);
        return 0;
    }
    if (size == (int)tail)
    {
        if (!rp)
        {
            LeaveCriticalSection(cs);
            return -2;  // filling exactly to the end would force wpos==rpos==0
        }
        memcpy(ring + wp, src, size);
        wp += size;
        if (wp == STAGE_SIZE)
            wp = 0;
        *pWpos = wp;
        LeaveCriticalSection(cs);
        return 0;
    }
    if (size >= (int)(tail + rp))
    {
        LeaveCriticalSection(cs);
        return -3;
    }
    memcpy(ring + wp, src, tail);            // fill to the end
    memcpy(ring, src + tail, size - tail);   // wrap the rest to the front
    *pWpos = size - tail;
    LeaveCriticalSection(cs);
    return 0;
}

static int __fastcall Proxy_ISocketWrite(void* this_ptr, void* /*edx*/, char* buffer, int size)
{
    char* base = (char*)this_ptr;
    if (!*(base + 4)) return (int)0x80000001;  // not initialized
    if (!*(base + 8)) return (int)0x80000054;  // socket closed
    if (!size || !buffer) return (int)0x80000000;
    return StageRingPush(base + STAGE_OFFSET, buffer, size) >= 0 ? 1 : (int)0x80000059;
}

int __fastcall Proxy_CipherEncrypt(void* This, void* NotUsed, int packet_type, char* input, int in_size, char* out_put, int* out_size)
{
    // A21 outbound header is 14 bytes; write total size at header+3.
    *(int*)(input - 14 + 3) = in_size + 14;

    *out_size = in_size;
    memcpy(out_put, input, in_size);
    debug_features::LogCipherPacket(
        L"encrypt", packet_type, input, in_size, out_put, *out_size);
    return 1;
}

int __fastcall Proxy_CipherDecrypt(void* This, void* NotUsed, int packet_type, char* input, int in_size, char* out_put, int* out_size)
{
    *out_size = in_size;
    memcpy(out_put, input, in_size);
    debug_features::LogCipherPacket(
        L"decrypt", packet_type, input, in_size, out_put, *out_size);
    return 1;
}

// Native reimplementation of CThread::Create (official logic from the clean
// reference). A21's own Create is VM'd (call target 0x4F69122) and in our
// runtime it neither resumes the new thread nor preserves the object vtable
// (0x103 gets written over [this+0]). This proxy creates the thread and
// resumes it immediately, exactly like the official client:
//   handle at this+0x1C, tid at this+0x20, started flag at this+0x24,
//   start routine = the shared thunk 0x028A4DC0.
static DWORD __fastcall Proxy_CThreadCreate(void* This, void* /*edx*/)
{
    DWORD* self = (DWORD*)This;
    // a previous VM-path teardown may have clobbered [this+0] with 0x103;
    // restore the listener class vtable so the thunk can dispatch Run()
    if (*self != 0x0361C50C)
        *self = 0x0361C50C;
    HANDLE h = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)0x028A4DC0, self,
                            4 /* CREATE_SUSPENDED */, self + 8);
    self[7] = (DWORD)(uintptr_t)h;
    if (!h)
        return 0;  // the official throws here
    SetThreadPriority(h, 2);  // THREAD_PRIORITY_HIGHEST
    *((BYTE*)self + 36) = 1;
    return ResumeThread(h);
}

void PatchS4A21(HMODULE module)
{
    StaticPatches();

    // Cipher::Encrypt → memcpy passthrough
    mem::jmphook(0x026B7D90, reinterpret_cast<uintptr_t>(Proxy_CipherEncrypt));

    // Cipher::Decrypt → memcpy passthrough
    mem::jmphook(0x026B7E50, reinterpret_cast<uintptr_t>(Proxy_CipherDecrypt));

    // nengine::ISocket::Write — full ring-buffer port of the official original
    mem::jmphook(0x05790FE9, reinterpret_cast<uintptr_t>(Proxy_ISocketWrite));

    // CThread::Create — native reimplementation replacing the VM'd original
    // (called from MTUPD StartListening at 0x28A06A2)
    mem::callhook(0x28A06A2, reinterpret_cast<uintptr_t>(Proxy_CThreadCreate));

    debug_features::Apply(module);
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        PatchS4A21(hModule);

        HANDLE thread = CreateThread(nullptr, 0,
            [](LPVOID context) -> DWORD {
                plugin_loader::LoadConfiguredPlugins(static_cast<HMODULE>(context));
                return 0;
            }, hModule, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
