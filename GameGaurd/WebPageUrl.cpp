#include "WebPageUrl.h"
#include "mem.h"

namespace
{
const wchar_t kUrl[] = L"https://example.com";

int __stdcall HookIeNavigate(void* browser, const wchar_t*)
{
    const int result = reinterpret_cast<int(__cdecl*)(void*, const wchar_t*)>(
        0x02746F50)(browser, kUrl);
    return result == 0;
}

const wchar_t* __cdecl HookEventUrl(int)
{
    return kUrl;
}
}

namespace web_page_url
{
void Install()
{
    mem::jmphook(0x027472E0, reinterpret_cast<uintptr_t>(&HookIeNavigate));
    mem::callhook(0x005192BE, reinterpret_cast<uintptr_t>(&HookEventUrl));
    mem::callhook(0x005192FB, reinterpret_cast<uintptr_t>(&HookEventUrl));
}
}
