#include "mem.h"
#include "DebugFeatures.h"
#include "PluginLoader.h"
#include "WebPageUrl.h"
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

    // #21: bypass version signature 0x1B492 check (JZ→JMP)
    mem::patch(0x01A73BBC, { 0xEB });

    // #23: skip game state verification (JZ→JMP)
    mem::patch(0x0231467D, { 0xE9, 0xA2, 0x00 });

    // --- Code fix / runtime patch ---

    // restore prologue destroyed by packer
    mem::patch(0x0274E6D8, { 0xE8, 0x33, 0x55, 0x51, 0x00, 0xA1, 0xE0, 0xCE, 0x99, 0x03, 0x31, 0xE8, 0x89, 0x45, 0xFC, 0xA1, 0xE4, 0xCE, 0xB7, 0x03 });

    // Restore the inverse of 13224B0's native config-group mapping on the
    // external chat checkbox save path. The original caller passes its UI
    // inner index as the config group: outer 0 must map to group 4, while
    // outer 2 inner 0..5 maps to groups 0,1,2,3,5,6. These two blocks occupy
    // runtime-verified INT3 alignment gaps and tail-jump to the original
    // 0A9AF50 call target with the original stack and return address intact.
    mem::patch(0x01322429, { 0xE8, 0x82, 0x59, 0x02, 0x00 });
    mem::patch(0x01347DB0, {
        0x83, 0xF8, 0x04,                   // cmp eax, 4
        0xF5,                               // cmc
        0x83, 0xD0, 0x00,                   // adc eax, 0
        0x83, 0x7E, 0x30, 0x00,             // cmp dword ptr [esi+30h], 0
        0xE9, 0x40, 0x0E, 0x00, 0x00        // jmp 01348C00
        });
    mem::patch(0x01348C00, {
        0x75, 0x05,                         // jne use_chat_group
        0xB8, 0x04, 0x00, 0x00, 0x00,       // mov eax, 4
        0x89, 0x44, 0x24, 0x04,             // mov [esp+4], eax
        0xE9, 0x40, 0x23, 0x75, 0xFF        // jmp 00A9AF50
        });

    // 13224B0 normally clamps a missing inner chat tab to index zero. The
    // shell can legitimately expose fewer than all six saved chat groups, so
    // that fallback lets the last absent group overwrite the Normal tab.
    // Preserve the native apply call for an existing tab, and skip it when the
    // requested inner index is outside the live tab vector.
    mem::patch(0x01322644, { 0xE8, 0x58, 0x68, 0x02, 0x00, 0x90 });
    mem::patch(0x01348EA1, {
        0x73, 0x05,                         // jae missing_tab
        0x58,                               // preserve the call return address
        0x57,                               // push edi (inner tab)
        0x56,                               // push esi (outer tab)
        0x50,                               // restore the call return address
        0xC3,                               // ret to the original apply call
        0x83, 0x04, 0x24, 0x08,             // missing_tab: skip to cleanup
        0xC2, 0x08, 0x00                    // discard the two earlier args
        });

    // Restore the two native raid message pairs omitted from 13224B0's chat
    // row conversion: 52/53 share row 4 and 54/55 share row 5. All other
    // types continue through the original resource lookup unchanged.
    mem::patch(0x0132256C, { 0xE8, 0xF0, 0x66, 0x02, 0x00 });
    mem::patch(0x01348C61, {
        0x2C, 0x33,                         // sub al, 51
        0x3C, 0x03,                         // cmp al, 3
        0x77, 0x5A,                         // ja original_lookup
        0xD0, 0xE8,                         // shr al, 1
        0x0C, 0x04,                         // or al, 4
        0x89, 0x45, 0xF8,                   // mov [ebp-8], eax
        0xEB, 0x51                          // jmp original_lookup
        });
    mem::patch(0x01348CC1, {
        0xE9, 0x0A, 0x29, 0xFD, 0xFF        // jmp 0131B5D0
        });

    // The external System list has one shell-owned leading row. Keep the
    // native type 16..30 range check, then advance only the resulting UI row
    // from 0..14 to 1..15 before continuing in 13224B0.
    mem::patch(0x01322532, { 0xE9, 0x0E, 0x83, 0x73, 0x00 });
    mem::patch(0x01A5A845, {
        0x40,                               // inc eax (system UI row)
        0x89, 0x45, 0xF8,                   // mov [ebp-8], eax
        0xE9, 0x1E, 0x7D, 0x8C, 0xFF        // jmp 0132256C
        });

    // Type 31 alone uses the special system-select path. The shell changed
    // this equality test into a signed range check, which also skips the
    // native config write for the persisted raid message types 52..55.
    mem::patch(0x01A5A5D2, { 0x74 });

    // The rebuild loop iterates native message types, not packed-byte offsets.
    // Process 0..30, skip the special/unpersisted 31..51 range, then process
    // the persisted raid pairs 52..55.
    mem::patch(0x01A5E7C3, { 0xE8, 0x89, 0x08, 0x00, 0x00 });
    mem::patch(0x01A5F051, {
        0x8B, 0xC3,                         // mov eax, ebx
        0x83, 0xFE, 0x1F,                   // cmp esi, 31
        0x75, 0x03,                         // jne compare_group
        0x6A, 0x34,                         // push 52
        0x5E,                               // pop esi
        0x83, 0xFF, 0x07,                   // compare_group: cmp edi, 7
        0xC3                                // ret
        });
    mem::patch(0x01A5E827, { 0x38 });

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

    // 19E95C0 binds the local character before USERINFO copies its job-face
    // frame. Route its native draw call through the adjacent INT3 alignment
    // gap and suppress only incomplete frames (defaultfaces.img has 0..13).
    // Keep 19E97A8's original epilogue in place: the member-removal path
    // jumps there directly when a face-bar slot becomes null.
    mem::patch(0x019E97B1, {
        0x83, 0x7E, 0x18, 0x0E,             // check_frame: cmp [esi+18h], 14
        0x73, 0xF1,                         // jae 019E97A8 (native epilogue)
        0x53,                               // draw_native: push ebx (slot)
        0xE8, 0xE3, 0xFC, 0xFF, 0xFF,       // call 019E94A0
        0xEB, 0xE9                          // jmp 019E97A8 (native epilogue)
        });
    mem::patch(0x019E97A2, {
        0x85, 0xF6,                         // test esi, esi
        0x74, 0x11,                         // jz draw_native
        0xEB, 0x09                          // jmp check_frame
        });
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
        web_page_url::Install();

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
