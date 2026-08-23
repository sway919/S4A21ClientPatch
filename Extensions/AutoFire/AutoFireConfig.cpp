#include "AutoFireConfig.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <vector>

namespace
{
constexpr DWORD kProfileBufferSize = 32768;
constexpr unsigned int kMinimumIntervalMs = 1;
constexpr unsigned int kMaximumIntervalMs = 1000;
constexpr int kMaximumCombos = 16;
constexpr size_t kMaximumComboActions = 64;

std::wstring Trim(const std::wstring& value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), iswspace);
    if (first == value.end())
        return {};

    const auto last = std::find_if_not(value.rbegin(), value.rend(), iswspace).base();
    return std::wstring(first, last);
}

std::wstring Upper(const std::wstring& value)
{
    std::wstring result = Trim(value);
    std::transform(result.begin(), result.end(), result.begin(), towupper);
    return result;
}

std::wstring ReadString(const wchar_t* section, const wchar_t* key,
    const wchar_t* fallback, const std::wstring& path)
{
    std::vector<wchar_t> buffer(4096, L'\0');
    GetPrivateProfileStringW(section, key, fallback, buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    return Trim(buffer.data());
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback,
    const std::wstring& path)
{
    const std::wstring value = Upper(ReadString(section, key,
        fallback ? L"1" : L"0", path));
    if (value == L"1" || value == L"TRUE" || value == L"YES" || value == L"ON")
        return true;
    if (value == L"0" || value == L"FALSE" || value == L"NO" || value == L"OFF")
        return false;
    return fallback;
}

bool ParseUnsigned(const std::wstring& text, unsigned int& value,
    unsigned int minimum, unsigned int maximum, int base = 10)
{
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty() || trimmed[0] == L'-')
        return false;

    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(trimmed.c_str(), &end, base);
    if (!end || *end != L'\0' || parsed < minimum || parsed > maximum)
        return false;

    value = static_cast<unsigned int>(parsed);
    return true;
}

struct NamedKey
{
    const wchar_t* name;
    WORD virtualKey;
};

constexpr NamedKey kNamedKeys[] = {
    {L"BACKSPACE", VK_BACK}, {L"BS", VK_BACK},
    {L"TAB", VK_TAB}, {L"ENTER", VK_RETURN}, {L"RETURN", VK_RETURN},
    {L"SHIFT", VK_SHIFT}, {L"LSHIFT", VK_LSHIFT}, {L"RSHIFT", VK_RSHIFT},
    {L"CTRL", VK_CONTROL}, {L"CONTROL", VK_CONTROL},
    {L"LCTRL", VK_LCONTROL}, {L"LCONTROL", VK_LCONTROL},
    {L"RCTRL", VK_RCONTROL}, {L"RCONTROL", VK_RCONTROL},
    {L"ALT", VK_MENU}, {L"LALT", VK_LMENU}, {L"RALT", VK_RMENU},
    {L"PAUSE", VK_PAUSE}, {L"CAPSLOCK", VK_CAPITAL},
    {L"ESC", VK_ESCAPE}, {L"ESCAPE", VK_ESCAPE}, {L"SPACE", VK_SPACE},
    {L"PAGEUP", VK_PRIOR}, {L"PGUP", VK_PRIOR},
    {L"PAGEDOWN", VK_NEXT}, {L"PGDN", VK_NEXT},
    {L"END", VK_END}, {L"HOME", VK_HOME},
    {L"LEFT", VK_LEFT}, {L"UP", VK_UP}, {L"RIGHT", VK_RIGHT}, {L"DOWN", VK_DOWN},
    {L"PRINTSCREEN", VK_SNAPSHOT}, {L"PRTSC", VK_SNAPSHOT},
    {L"INSERT", VK_INSERT}, {L"INS", VK_INSERT},
    {L"DELETE", VK_DELETE}, {L"DEL", VK_DELETE},
    {L"NUMLOCK", VK_NUMLOCK}, {L"SCROLLLOCK", VK_SCROLL},
    {L"NUMPADMULT", VK_MULTIPLY}, {L"NUMPADADD", VK_ADD},
    {L"NUMPADSUB", VK_SUBTRACT}, {L"NUMPADDOT", VK_DECIMAL},
    {L"NUMPADDIV", VK_DIVIDE},
    {L"LBUTTON", VK_LBUTTON}, {L"RBUTTON", VK_RBUTTON},
    {L"MBUTTON", VK_MBUTTON}, {L"XBUTTON1", VK_XBUTTON1}, {L"XBUTTON2", VK_XBUTTON2}
};

bool CompleteKeySpec(WORD virtualKey, auto_fire::KeySpec& result)
{
    if (!virtualKey)
        return false;

    // 鼠标键没有键盘扫描码，但可以作为组合键或鼠标连点的物理触发键。
    if (virtualKey >= VK_LBUTTON && virtualKey <= VK_XBUTTON2)
    {
        result.virtualKey = virtualKey;
        result.scanCode = 0;
        result.extended = false;
        return true;
    }

    const UINT mapped = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
    if (!mapped)
        return false;

    result.virtualKey = virtualKey;
    result.scanCode = static_cast<BYTE>(mapped & 0xFF);
    result.extended = (mapped & 0xFF00) == 0xE000;
    return true;
}

bool ParseScanCode(const std::wstring& value, auto_fire::KeySpec& result)
{
    if (value.size() < 4 || value.compare(0, 2, L"SC") != 0)
        return false;

    std::wstring digits = value.substr(2);
    bool extended = false;
    // 扩展扫描码以 SCE0xx 表示，例如右方向键为 SCE04D。
    if (digits.size() == 4 && digits.compare(0, 2, L"E0") == 0)
    {
        extended = true;
        digits.erase(0, 2);
    }

    unsigned int scanCode = 0;
    if (!ParseUnsigned(digits, scanCode, 1, 0xFF, 16))
        return false;

    const UINT mappedCode = extended ? (0xE000u | scanCode) : scanCode;
    const UINT virtualKey = MapVirtualKeyW(mappedCode, MAPVK_VSC_TO_VK_EX);
    if (!virtualKey)
        return false;

    result.virtualKey = static_cast<WORD>(virtualKey);
    result.scanCode = static_cast<BYTE>(scanCode);
    result.extended = extended;
    return true;
}

bool ParseKey(const std::wstring& text, auto_fire::KeySpec& result)
{
    const std::wstring value = Upper(text);
    if (value.empty())
        return false;

    if (ParseScanCode(value, result))
        return true;

    for (const NamedKey& key : kNamedKeys)
    {
        if (value == key.name)
            return CompleteKeySpec(key.virtualKey, result);
    }

    if (value.size() >= 2 && value[0] == L'F')
    {
        unsigned int functionNumber = 0;
        if (ParseUnsigned(value.substr(1), functionNumber, 1, 24))
            return CompleteKeySpec(static_cast<WORD>(VK_F1 + functionNumber - 1), result);
    }

    if (value.size() == 7 && value.compare(0, 6, L"NUMPAD") == 0 &&
        value[6] >= L'0' && value[6] <= L'9')
    {
        return CompleteKeySpec(static_cast<WORD>(VK_NUMPAD0 + value[6] - L'0'), result);
    }

    if (value.size() == 1)
    {
        const SHORT mapped = VkKeyScanW(value[0]);
        if (mapped != -1)
            return CompleteKeySpec(static_cast<WORD>(mapped & 0xFF), result);
    }

    return false;
}

std::vector<std::pair<std::wstring, std::wstring>> ReadSection(
    const wchar_t* section, const std::wstring& path)
{
    std::vector<wchar_t> buffer(kProfileBufferSize, L'\0');
    const DWORD length = GetPrivateProfileSectionW(section, buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (!length || length >= buffer.size() - 2)
        return entries;

    for (const wchar_t* current = buffer.data(); *current;
        current += wcslen(current) + 1)
    {
        const wchar_t* separator = wcschr(current, L'=');
        if (!separator)
            continue;
        entries.emplace_back(Trim(std::wstring(current, separator)),
            Trim(separator + 1));
    }
    return entries;
}

std::vector<std::wstring> Split(const std::wstring& value, wchar_t separator)
{
    std::vector<std::wstring> result;
    size_t begin = 0;
    while (begin <= value.size())
    {
        const size_t end = value.find(separator, begin);
        result.push_back(Trim(value.substr(begin,
            end == std::wstring::npos ? std::wstring::npos : end - begin)));
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return result;
}

void LoadRapidFire(const std::wstring& path, auto_fire::Configuration& config)
{
    // [RapidFire] 的键名是物理键，值是连发周期毫秒。
    for (const auto& entry : ReadSection(L"RapidFire", path))
    {
        auto_fire::KeySpec key;
        unsigned int interval = 0;
        if (ParseKey(entry.first, key) && key.scanCode &&
            ParseUnsigned(entry.second, interval, kMinimumIntervalMs, kMaximumIntervalMs))
        {
            config.rapidFire.push_back({key, interval});
        }
    }
}

void LoadCombos(const std::wstring& path, auto_fire::Configuration& config)
{
    // 每个组合必须显式启用；缺少 Enabled 时按关闭处理，避免示例配置意外生效。
    for (int index = 0; index < kMaximumCombos; ++index)
    {
        const std::wstring section = L"Combo" + std::to_wstring(index);
        if (!ReadBool(section.c_str(), L"Enabled", false, path))
            continue;

        const std::wstring triggerText = ReadString(section.c_str(), L"Trigger", L"", path);
        const std::wstring actionsText = ReadString(section.c_str(), L"Actions", L"", path);

        auto_fire::ComboBinding combo;
        if (!ParseKey(triggerText, combo.trigger) || actionsText.empty())
            continue;

        combo.repeat = ReadBool(section.c_str(), L"Repeat", true, path);
        for (const std::wstring& actionText : Split(actionsText, L','))
        {
            if (actionText.empty() || combo.actions.size() >= kMaximumComboActions)
                continue;

            const size_t colon = actionText.find_last_of(L':');
            const std::wstring keyText = colon == std::wstring::npos
                ? actionText : actionText.substr(0, colon);
            const std::wstring delayText = colon == std::wstring::npos
                ? L"1" : actionText.substr(colon + 1);

            auto_fire::SequenceAction action;
            if (ParseKey(keyText, action.key) && action.key.scanCode &&
                ParseUnsigned(delayText, action.delayAfterMs,
                    kMinimumIntervalMs, kMaximumIntervalMs))
            {
                combo.actions.push_back(action);
            }
        }

        if (!combo.actions.empty())
            config.combos.push_back(std::move(combo));
    }
}

void LoadMouse(const std::wstring& path, auto_fire::Configuration& config)
{
    config.mouse.enabled = ReadBool(L"Mouse", L"Enabled", false, path);
    if (!config.mouse.enabled)
        return;

    if (!ParseKey(ReadString(L"Mouse", L"Trigger", L"MButton", path),
        config.mouse.trigger))
    {
        config.mouse.enabled = false;
        return;
    }

    const std::wstring button = Upper(ReadString(L"Mouse", L"Button", L"Left", path));
    if (button == L"RIGHT")
    {
        config.mouse.downFlag = MOUSEEVENTF_RIGHTDOWN;
        config.mouse.upFlag = MOUSEEVENTF_RIGHTUP;
    }
    else if (button == L"MIDDLE")
    {
        config.mouse.downFlag = MOUSEEVENTF_MIDDLEDOWN;
        config.mouse.upFlag = MOUSEEVENTF_MIDDLEUP;
    }
    else if (button == L"XBUTTON1")
    {
        config.mouse.downFlag = MOUSEEVENTF_XDOWN;
        config.mouse.upFlag = MOUSEEVENTF_XUP;
        config.mouse.data = XBUTTON1;
    }
    else if (button == L"XBUTTON2")
    {
        config.mouse.downFlag = MOUSEEVENTF_XDOWN;
        config.mouse.upFlag = MOUSEEVENTF_XUP;
        config.mouse.data = XBUTTON2;
    }
    else if (button != L"LEFT")
    {
        config.mouse.enabled = false;
        return;
    }

    unsigned int interval = 0;
    if (ParseUnsigned(ReadString(L"Mouse", L"IntervalMs", L"10", path),
        interval, kMinimumIntervalMs, kMaximumIntervalMs))
    {
        config.mouse.intervalMs = interval;
    }

    unsigned int pressDuration = 0;
    if (ParseUnsigned(ReadString(L"Mouse", L"PressDurationMs", L"16", path),
        pressDuration, kMinimumIntervalMs, kMaximumIntervalMs))
    {
        config.mouse.pressDurationMs = pressDuration;
    }
}
}

namespace auto_fire
{
std::wstring ConfigurationPath(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path))
        return L"AutoFire.ini";

    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"AutoFire.ini";
    result.resize(slash + 1);
    result += L"AutoFire.ini";
    return result;
}

Configuration LoadConfiguration(const std::wstring& path)
{
    Configuration config;
    config.enabled = ReadBool(L"General", L"Enabled", true, path);
    config.foregroundOnly = ReadBool(L"General", L"ForegroundOnly", true, path);
    config.debug = ReadBool(L"General", L"Debug", false, path);

    unsigned int keyPressDuration = 0;
    if (ParseUnsigned(ReadString(L"General", L"KeyPressDurationMs", L"16", path),
        keyPressDuration, kMinimumIntervalMs, kMaximumIntervalMs))
    {
        config.keyPressDurationMs = keyPressDuration;
    }

    unsigned int holdThreshold = 0;
    if (ParseUnsigned(ReadString(L"General", L"HoldThresholdMs", L"0", path),
        holdThreshold, 0, kMaximumIntervalMs))
    {
        config.holdThresholdMs = holdThreshold;
    }

    config.hasToggleKey = ParseKey(
        ReadString(L"General", L"ToggleKey", L"PGUP", path), config.toggleKey);
    config.hasReloadKey = ParseKey(
        ReadString(L"General", L"ReloadKey", L"PGDN", path), config.reloadKey);

    LoadRapidFire(path, config);
    LoadCombos(path, config);
    LoadMouse(path, config);
    return config;
}
}
