#include "mem.h"
#include "DebugFeatures.h"
#include "PluginLoader.h"
#include <cstring>
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

    // packed `jge`→`jz`：type 52-55 走进 1A5A590 第一段写。
    mem::patch(0x01A5A5D2, { 0x74 });

    // 19E95C0 (put char in a face-bar slot) ends with call 19E94A0.
    // That blit uses this+0x18 before 19DFB10 copies the job face;
    // leftover 14 is past defaultfaces.img frames 0–13 and draws the
    // missing-image stub at client (0,0) on select→town.
    // Keep 13E27E0's "is me" path (19E9800 → 19E95C0 bind, widgets
    // 0x83/0x84, 163DA50). Only NOP this trailing draw; later official
    // 19E94A0 callers still run.
    mem::patch(0x19E97A3, { 0x90, 0x90, 0x90, 0x90, 0x90 });
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

// 聊天页签过滤：不自写 154B，不自调 1A59B90。

static int FilterBlobSum(const unsigned char* blob)
{
    int n = 0;
    for (int i = 0; i < 154; ++i)
        n += blob[i];
    return n;
}

// 全 0 的 154B 当 miss，走 1A59B90 失败默认。
static char __fastcall Hook_ReadFilterBlob(void* store, void* /*edx*/, int key, void* blob, int size)
{
    const char ok = reinterpret_cast<char (__thiscall *)(void*, int, void*, int)>(0x01385870)(
        store, key, blob, size);
    if (ok && size == 154 && blob && FilterBlobSum(static_cast<unsigned char*>(blob)) == 0)
        return 0;
    return ok;
}

// 空 slot (-1,-1,-1) 走 137EEF0(138,97,97)。
static char __fastcall Hook_LoadTabIds(void* store)
{
    const char ok = reinterpret_cast<char (__thiscall *)(void*)>(0x01386210)(store);
    int* slot0 = reinterpret_cast<int*>(0x0397F0A8);
    int* slot1 = reinterpret_cast<int*>(0x0397F0AC);
    int* slot2 = reinterpret_cast<int*>(0x0397F0B0);
    if (*slot0 == -1 && *slot1 == -1 && *slot2 == -1)
    {
        reinterpret_cast<int (__stdcall *)(int, int, int)>(0x0137EEF0)(138, 97, 97);
        typedef void* (__cdecl *FnStore)();
        void* st = reinterpret_cast<FnStore>(0x013868F0)();
        if (st)
            reinterpret_cast<int (__thiscall *)(void*)>(0x01388420)(st);
    }
    return ok;
}

// 跳过错位的 1A5C6B0，避免 hide 窗口 tab 81-86。
static int __fastcall Hook_HideExtraTabs(void* /*chat*/)
{
    return 0;
}

// 13224B0：type 16-30 读 group 4；type 1/2/3/6 共用 channel 2。
static int Official13224B0BlobGroup(int leftover_group, int type)
{
    if ((unsigned)(type - 16) <= 14)
        return 4;
    return leftover_group;
}

static int Official13224B0SharedChannel(int type)
{
    return type == 1 || type == 2 || type == 3 || type == 6;
}

static void OfficialSaveFilter(void* win, int group, int type, int value)
{
    reinterpret_cast<void (__thiscall *)(void*, int, int, int)>(0x01A5A860)(
        win, group, type, value);
}

// leftover 脏 bool 收成 0/1；先写 [esi+34h]，不够再写 13224B0 会读的 group。
static void __fastcall Hook_SaveFromEngine(void* win, void* /*edx*/, int group, int type, int value)
{
    int v = value;
    if (*reinterpret_cast<int*>(0x03998E00) == 4483)
        v &= 1;
    OfficialSaveFilter(win, group, type, v);
    if (*reinterpret_cast<int*>(0x03998E00) == 4483)
    {
        const int apply_group = Official13224B0BlobGroup(group, type);
        if (apply_group != group)
            OfficialSaveFilter(win, apply_group, type, v);
    }
}

// shared-channel type 只把 leftover group 交给 13224B0。
static void __cdecl Hook_Df80EngineApply(int group, int type, int value)
{
    if (Official13224B0SharedChannel(type) &&
        group != Official13224B0BlobGroup(0, type))
        return;
    reinterpret_cast<void (__cdecl *)(int, int, int)>(0x013224B0)(group, type, value);
}

// 只 hook call site。
static void InstallChatFilterOfficial()
{
    mem::callhook(0x01A59BD3, reinterpret_cast<uintptr_t>(Hook_ReadFilterBlob));
    mem::callhook(0x01A66137, reinterpret_cast<uintptr_t>(Hook_LoadTabIds));
    mem::callhook(0x011110B3, reinterpret_cast<uintptr_t>(Hook_HideExtraTabs));
    mem::callhook(0x01322430, reinterpret_cast<uintptr_t>(Hook_SaveFromEngine));
    mem::callhook(0x01A5E81C, reinterpret_cast<uintptr_t>(Hook_Df80EngineApply));
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

    InstallChatFilterOfficial();

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
