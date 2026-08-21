#define NOMINMAX
#include "GameNativeClip.h"
#include "GameNativeHook.h"
#include "GameNativeLog.h"
#include "GameNativeNotice.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
constexpr uintptr_t kImageDrawAddress = 0x027D8CA0;
constexpr uintptr_t kControlLookupAddress = 0x01A26BB0;
constexpr uintptr_t kWindowManagerGlobalAddress = 0x03A5C9B8;
constexpr uintptr_t kHasWindowAddress = 0x02301820;
constexpr uintptr_t kItemControlVtableAddress = 0x031F5864;
constexpr size_t kControlWidthOffset = 0x3C;
constexpr size_t kControlHeightOffset = 0x40;
constexpr size_t kControlLocalXOffset = 52;
constexpr size_t kControlLocalYOffset = 56;
constexpr size_t kControlScreenXOffset = 76;
constexpr size_t kControlScreenYOffset = 80;
constexpr size_t kImageDrawVtableSlot = 10;
constexpr size_t kImageClipLeftOffset = 964;
constexpr size_t kImageClipRightOffset = 968;
constexpr size_t kImageClipTopOffset = 972;
constexpr size_t kImageClipBottomOffset = 976;
constexpr int kDefaultTileOffset = 11;
constexpr DWORD kClipPruneMs = 250;
constexpr int kMaxOriginJump = 80;
constexpr int kSettleFrames = 8;

using ControlLookupFn = void* (__thiscall*)(void* window, void* result,
    int controlId);
using HasWindowFn = bool(__thiscall*)(void* manager, int windowId);

struct ClientSharedPtr
{
    void* object = nullptr;
    void* controlBlock = nullptr;
};

struct ClippedImageTemplate
{
    int controlId = 0;
    int posX = 0;
    int posY = 0;
    int clipLeft = 0;
    int clipTop = 0;
    int clipRight = 0;
    int clipBottom = 0;
};

struct ClippedImage
{
    void* image = nullptr;
    int controlId = 0;
    int posX = 0;
    int posY = 0;
    int clipLeft = 0;
    int clipTop = 0;
    int clipRight = 0;
    int clipBottom = 0;
    int localLeft = 0;
    int localTop = 0;
    int localRight = 0;
    int localBottom = 0;
    int writtenLeft = 0;
    int writtenTop = 0;
    int writtenRight = 0;
    int writtenBottom = 0;
};

struct PluginWindowClip
{
    void* window = nullptr;
    int windowId = -1;
    int tileX = kDefaultTileOffset;
    int tileY = kDefaultTileOffset;
    bool seenOpen = false;
    bool collected = false;
    bool localsCaptured = false;
    int lastOriginX = 0x7FFFFFFF;
    int lastOriginY = 0x7FFFFFFF;
    int lastDeltaScreenX = 1;
    int lastDeltaPosX = 1;
    int lastDeltaScreenY = 1;
    int lastDeltaPosY = 1;
    int settleFrames = 0;
    bool keptWithoutHasWindow = false;
    std::vector<ClippedImageTemplate> templates;
    std::vector<ClippedImage> images;
};

int g_pendingTileX = kDefaultTileOffset;
int g_pendingTileY = kDefaultTileOffset;
int g_pendingWindowW = 0;
int g_pendingWindowH = 0;
std::vector<ClippedImageTemplate> g_pendingTemplates;
std::vector<PluginWindowClip> g_pluginWindows;
bool g_clipReady = false;
SRWLOCK g_clipLock = SRWLOCK_INIT;
LONG g_pluginWindowCount = 0;
DWORD g_lastPruneAt = 0;
int g_hasWindowReady = 0;

void ReleaseClientSharedPtr(ClientSharedPtr& value)
{
    void* block = value.controlBlock;
    value.object = nullptr;
    value.controlBlock = nullptr;
    if (!block)
        return;
    __try
    {
        auto* uses = reinterpret_cast<volatile long*>(
            static_cast<unsigned char*>(block) + 4);
        if (InterlockedDecrement(uses) == 0)
        {
            auto** vtable = *reinterpret_cast<void***>(block);
            reinterpret_cast<void(__thiscall*)(void*)>(vtable[0])(block);
            auto* weaks = reinterpret_cast<volatile long*>(
                static_cast<unsigned char*>(block) + 8);
            if (InterlockedDecrement(weaks) == 0)
                reinterpret_cast<void(__thiscall*)(void*)>(vtable[1])(block);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

bool GetControl(void* window, int controlId, ClientSharedPtr& result)
{
    result = {};
    if (!window)
        return false;
    __try
    {
        auto lookup = reinterpret_cast<ControlLookupFn>(
            GameNativeClientAddress(kControlLookupAddress));
        lookup(window, &result, controlId);
        return result.object != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = {};
        return false;
    }
}

bool UsesImageDraw(void* object)
{
    if (!object)
        return false;
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(object);
        return vtable && vtable[kImageDrawVtableSlot] ==
            reinterpret_cast<void*>(GameNativeClientAddress(kImageDrawAddress));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsItemControl(void* object)
{
    if (!object)
        return false;
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(object);
        return vtable == reinterpret_cast<void**>(
            GameNativeClientAddress(kItemControlVtableAddress));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void RefreshImagePointers(PluginWindowClip& slot)
{
    for (ClippedImage& image : slot.images)
    {
        ClientSharedPtr control;
        if (GetControl(slot.window, image.controlId, control) &&
            control.object && UsesImageDraw(control.object) &&
            !IsItemControl(control.object))
            image.image = control.object;
        ReleaseClientSharedPtr(control);
    }
}

void BindTemplateImages(PluginWindowClip& slot)
{
    slot.images.clear();
    for (const ClippedImageTemplate& layout : slot.templates)
    {
        ClientSharedPtr control;
        if (!GetControl(slot.window, layout.controlId, control) ||
            !control.object || !UsesImageDraw(control.object) ||
            IsItemControl(control.object))
        {
            ReleaseClientSharedPtr(control);
            continue;
        }

        ClippedImage captured;
        captured.image = control.object;
        captured.controlId = layout.controlId;
        captured.posX = layout.posX;
        captured.posY = layout.posY;
        captured.clipLeft = layout.clipLeft;
        captured.clipTop = layout.clipTop;
        captured.clipRight = layout.clipRight;
        captured.clipBottom = layout.clipBottom;
        slot.images.push_back(captured);
        ReleaseClientSharedPtr(control);
    }
}

void* WindowManager()
{
    __try
    {
        return *reinterpret_cast<void**>(
            GameNativeClientAddress(kWindowManagerGlobalAddress));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

bool HasWindowAvailable()
{
    if (g_hasWindowReady != 0)
        return g_hasWindowReady > 0;

    static constexpr unsigned char kSignature[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x3D, 0x8B, 0x03, 0x00, 0x00,
    };
    const bool ready = GameNativeValidateBytes(
        GameNativeClientAddress(kHasWindowAddress), kSignature,
        sizeof(kSignature));
    g_hasWindowReady = ready ? 1 : -1;
    return ready;
}

bool WindowIsOpen(int windowId)
{
    void* manager = WindowManager();
    if (!manager || windowId < 0 || !HasWindowAvailable())
        return false;
    __try
    {
        auto hasWindow = reinterpret_cast<HasWindowFn>(
            GameNativeClientAddress(kHasWindowAddress));
        return hasWindow(manager, windowId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SlotImagesLive(const PluginWindowClip& slot)
{
    for (const ClippedImage& image : slot.images)
    {
        if (image.image && UsesImageDraw(image.image) &&
            !IsItemControl(image.image))
            return true;
    }
    return false;
}

void PruneClosedWindows()
{
    g_pluginWindows.erase(std::remove_if(g_pluginWindows.begin(),
        g_pluginWindows.end(), [](PluginWindowClip& slot) {
            if (!slot.window)
                return true;
            if (WindowIsOpen(slot.windowId))
            {
                slot.seenOpen = true;
                slot.keptWithoutHasWindow = false;
                return false;
            }
            if (SlotImagesLive(slot))
            {
                if (!slot.keptWithoutHasWindow)
                {
                    slot.keptWithoutHasWindow = true;
                    GameNativeLog(
                        L"[clip] keep window=%p id=%d after HasWindow=0",
                        slot.window, slot.windowId);
                }
                return false;
            }
            return slot.seenOpen;
        }), g_pluginWindows.end());
}

void InvalidateSlotOrigin(PluginWindowClip& slot)
{
    slot.localsCaptured = false;
    slot.lastOriginX = 0x7FFFFFFF;
    slot.lastOriginY = 0x7FFFFFFF;
    slot.lastDeltaScreenX = 1;
    slot.lastDeltaPosX = 1;
    slot.lastDeltaScreenY = 1;
    slot.lastDeltaPosY = 1;
    slot.settleFrames = kSettleFrames;
}

void RebindSlotImages(PluginWindowClip& slot)
{
    slot.images.clear();
    BindTemplateImages(slot);
    InvalidateSlotOrigin(slot);
}

bool ValuesAgree(const int* values, int count)
{
    for (int index = 1; index < count; ++index)
    {
        if (values[index] != values[0])
            return false;
    }
    return true;
}

int MajorityValue(const int* values, int count)
{
    int best = values[0];
    int bestVotes = 0;
    for (int index = 0; index < count; ++index)
    {
        int votes = 0;
        for (int other = 0; other < count; ++other)
        {
            if (values[other] == values[index])
                ++votes;
        }
        if (votes > bestVotes)
        {
            best = values[index];
            bestVotes = votes;
        }
    }
    return bestVotes * 2 >= count ? best : values[0];
}

int MapLayout(int layout, int deltaScreen, int deltaPos)
{
    if (deltaPos == 0 || deltaScreen == deltaPos)
        return layout;
    return static_cast<int>(
        (static_cast<long long>(layout) * deltaScreen) / deltaPos);
}

void InferAxisScale(const int* screen, const int* pos, int count, int& deltaScreen,
    int& deltaPos)
{
    deltaScreen = 1;
    deltaPos = 1;
    int best = 0;
    for (int first = 0; first < count; ++first)
    {
        for (int second = first + 1; second < count; ++second)
        {
            const int posDelta = pos[first] - pos[second];
            const int posSpan = posDelta < 0 ? -posDelta : posDelta;
            if (posSpan <= best)
                continue;
            best = posSpan;
            deltaPos = posDelta;
            deltaScreen = screen[first] - screen[second];
        }
    }
}

int InferOriginAxis(const int* screen, const int* pos, int count, int tile,
    int deltaScreen, int deltaPos)
{
    if (count <= 0)
        return 0;
    if (count == 1)
        return screen[0] - MapLayout(tile + pos[0], deltaScreen, deltaPos);

    std::vector<int> fromControl(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
        fromControl[index] = screen[index] -
            MapLayout(tile + pos[index], deltaScreen, deltaPos);

    if (ValuesAgree(fromControl.data(), count))
        return fromControl[0];
    if (ValuesAgree(screen, count))
        return screen[0] - MapLayout(tile, deltaScreen, deltaPos);
    return MajorityValue(fromControl.data(), count);
}

bool ReadImageLayout(void* image, int& screenX, int& screenY, int& posX,
    int& posY)
{
    __try
    {
        auto* bytes = static_cast<unsigned char*>(image);
        screenX = *reinterpret_cast<const int*>(bytes + kControlScreenXOffset);
        screenY = *reinterpret_cast<const int*>(bytes + kControlScreenYOffset);
        posX = *reinterpret_cast<const int*>(bytes + kControlLocalXOffset);
        posY = *reinterpret_cast<const int*>(bytes + kControlLocalYOffset);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

int AbsDiff(int left, int right)
{
    return left >= right ? left - right : right - left;
}

bool WriteClipRect(ClippedImage& image, int left, int top, int right,
    int bottom, bool writeSize)
{
    if (!image.image || !UsesImageDraw(image.image) ||
        IsItemControl(image.image))
        return false;
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0)
        return false;
    __try
    {
        auto* bytes = static_cast<unsigned char*>(image.image);
        *reinterpret_cast<int*>(bytes + kImageClipLeftOffset) = left;
        *reinterpret_cast<int*>(bytes + kImageClipRightOffset) = right;
        *reinterpret_cast<int*>(bytes + kImageClipTopOffset) = top;
        *reinterpret_cast<int*>(bytes + kImageClipBottomOffset) = bottom;
        if (writeSize)
        {
            *reinterpret_cast<int*>(bytes + kControlWidthOffset) = width;
            *reinterpret_cast<int*>(bytes + kControlHeightOffset) = height;
        }
        image.writtenLeft = left;
        image.writtenTop = top;
        image.writtenRight = right;
        image.writtenBottom = bottom;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void StoreLocalsFromMappedTemplate(ClippedImage& image, int deltaScreenX,
    int deltaPosX, int deltaScreenY, int deltaPosY)
{
    image.localLeft = MapLayout(image.clipLeft, deltaScreenX, deltaPosX);
    image.localRight = MapLayout(image.clipRight, deltaScreenX, deltaPosX);
    image.localTop = MapLayout(image.clipTop, deltaScreenY, deltaPosY);
    image.localBottom = MapLayout(image.clipBottom, deltaScreenY, deltaPosY);
}

bool FollowOriginDelta(PluginWindowClip& slot, int originX, int originY)
{
    if (slot.lastOriginX == 0x7FFFFFFF || slot.lastOriginY == 0x7FFFFFFF)
    {
        slot.lastOriginX = originX;
        slot.lastOriginY = originY;
        return true;
    }

    const int deltaX = originX - slot.lastOriginX;
    const int deltaY = originY - slot.lastOriginY;
    slot.lastOriginX = originX;
    slot.lastOriginY = originY;
    if (deltaX == 0 && deltaY == 0)
        return true;

    bool allApplied = true;
    for (ClippedImage& image : slot.images)
    {
        allApplied = WriteClipRect(image,
            image.writtenLeft + deltaX, image.writtenTop + deltaY,
            image.writtenRight + deltaX, image.writtenBottom + deltaY,
            false) &&
            allApplied;
    }
    return allApplied;
}

bool WriteLocals(PluginWindowClip& slot, int originX, int originY)
{
    bool allApplied = true;
    for (ClippedImage& image : slot.images)
    {
        allApplied = WriteClipRect(image,
            originX + image.localLeft, originY + image.localTop,
            originX + image.localRight, originY + image.localBottom,
            true) &&
            allApplied;
    }
    return allApplied;
}

bool ApplyMappedTemplate(PluginWindowClip& slot, int originX, int originY,
    int deltaScreenX, int deltaPosX, int deltaScreenY, int deltaPosY)
{
    for (ClippedImage& image : slot.images)
    {
        StoreLocalsFromMappedTemplate(image, deltaScreenX, deltaPosX,
            deltaScreenY, deltaPosY);
        if (!WriteClipRect(image,
                originX + image.localLeft, originY + image.localTop,
                originX + image.localRight, originY + image.localBottom,
                true))
            return false;
    }
    slot.localsCaptured = true;
    slot.lastOriginX = originX;
    slot.lastOriginY = originY;
    slot.lastDeltaScreenX = deltaScreenX;
    slot.lastDeltaPosX = deltaPosX;
    slot.lastDeltaScreenY = deltaScreenY;
    slot.lastDeltaPosY = deltaPosY;
    GameNativeLog(
        L"[clip] remap origin=%d,%d scaleX=%d/%d scaleY=%d/%d",
        originX, originY, deltaScreenX, deltaPosX, deltaScreenY, deltaPosY);
    return true;
}

bool InferChildOrigin(PluginWindowClip& slot, int& originX, int& originY,
    int& deltaScreenX, int& deltaPosX, int& deltaScreenY, int& deltaPosY)
{
    const int count = static_cast<int>(slot.images.size());
    if (count <= 0)
        return false;

    std::vector<int> screenX(static_cast<size_t>(count));
    std::vector<int> screenY(static_cast<size_t>(count));
    std::vector<int> templateX(static_cast<size_t>(count));
    std::vector<int> templateY(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        int livePosX = 0;
        int livePosY = 0;
        if (!ReadImageLayout(slot.images[index].image, screenX[index],
                screenY[index], livePosX, livePosY))
            return false;
        templateX[index] = slot.images[index].posX;
        templateY[index] = slot.images[index].posY;
    }

    InferAxisScale(screenX.data(), templateX.data(), count, deltaScreenX,
        deltaPosX);
    InferAxisScale(screenY.data(), templateY.data(), count, deltaScreenY,
        deltaPosY);
    originX = InferOriginAxis(screenX.data(), templateX.data(), count,
        slot.tileX, deltaScreenX, deltaPosX);
    originY = InferOriginAxis(screenY.data(), templateY.data(), count,
        slot.tileY, deltaScreenY, deltaPosY);
    return true;
}

bool ApplyWindowClips(PluginWindowClip& slot)
{
    if (slot.images.empty())
        return false;

    RefreshImagePointers(slot);

    int originX = 0;
    int originY = 0;
    int deltaScreenX = 1;
    int deltaPosX = 1;
    int deltaScreenY = 1;
    int deltaPosY = 1;
    if (!InferChildOrigin(slot, originX, originY, deltaScreenX, deltaPosX,
            deltaScreenY, deltaPosY))
        return false;

    const bool scaleChanged = slot.localsCaptured &&
        (deltaScreenX != slot.lastDeltaScreenX ||
            deltaPosX != slot.lastDeltaPosX ||
            deltaScreenY != slot.lastDeltaScreenY ||
            deltaPosY != slot.lastDeltaPosY);
    const bool originJump = slot.localsCaptured &&
        slot.lastOriginX != 0x7FFFFFFF &&
        (AbsDiff(originX, slot.lastOriginX) > kMaxOriginJump ||
            AbsDiff(originY, slot.lastOriginY) > kMaxOriginJump) &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;
    const bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool remapping = !slot.localsCaptured ||
        (!mouseDown && (scaleChanged || originJump || slot.settleFrames > 0));

    if (slot.localsCaptured && mouseDown)
        return FollowOriginDelta(slot, originX, originY);

    if (remapping)
    {
        const bool originSame = slot.localsCaptured &&
            originX == slot.lastOriginX && originY == slot.lastOriginY;
        if (!ApplyMappedTemplate(slot, originX, originY, deltaScreenX,
                deltaPosX, deltaScreenY, deltaPosY))
            return false;
        if (originSame && !scaleChanged)
        {
            if (slot.settleFrames > 0)
                --slot.settleFrames;
        }
        else
        {
            slot.settleFrames = kSettleFrames;
        }
        return true;
    }

    if (originX == slot.lastOriginX && originY == slot.lastOriginY)
        return true;

    slot.lastOriginX = originX;
    slot.lastOriginY = originY;
    return WriteLocals(slot, originX, originY);
}

bool ParseVector2(const char* text, int* x, int* y)
{
    if (!text || !x || !y)
        return false;
    char* end = nullptr;
    const long left = std::strtol(text, &end, 10);
    if (end == text)
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end == ',')
        ++end;
    while (*end == ' ' || *end == '\t')
        ++end;
    char* endY = nullptr;
    const long top = std::strtol(end, &endY, 10);
    if (endY == end)
        return false;
    *x = static_cast<int>(left);
    *y = static_cast<int>(top);
    return true;
}
}

bool GameNativeExtractTileOffset(const char* text, int* x, int* y)
{
    if (!text || !x || !y)
        return false;
    static const char* kKeys[] = {
        "TileOffset_=\"VECTOR:",
        "TileOffset=\"VECTOR:",
    };
    for (const char* key : kKeys)
    {
        const char* found = std::strstr(text, key);
        if (!found)
            continue;
        found += std::strlen(key);
        while (*found == ' ' || *found == '\t')
            ++found;
        if (ParseVector2(found, x, y))
            return true;
    }
    return false;
}

bool ParseAttrInt(const char* start, const char* end, const char* key,
    int* value)
{
    if (!start || !end || !key || !value || start >= end)
        return false;
    const char* found = std::strstr(start, key);
    if (!found || found >= end)
        return false;
    const char* number = found + std::strlen(key);
    if (number >= end)
        return false;
    char* parsed = nullptr;
    const long parsedValue = std::strtol(number, &parsed, 10);
    if (parsed == number)
        return false;
    *value = static_cast<int>(parsedValue);
    return true;
}

void GameNativeExtractClippedLayout(const char* text)
{
    std::vector<ClippedImageTemplate> parsed;
    int windowW = 0;
    int windowH = 0;
    if (text)
    {
        const char* popup = std::strstr(text, "<POPUPWINDOW");
        if (popup)
        {
            const char* popupEnd = std::strchr(popup, '>');
            const char* size = std::strstr(popup, "Size=\"VECTOR:");
            if (size && (!popupEnd || size < popupEnd))
                ParseVector2(size + 13, &windowW, &windowH);
        }
        const char* cursor = text;
        while ((cursor = std::strstr(cursor, "<CNUIControlImage")) != nullptr)
        {
            const char* tagEnd = std::strchr(cursor, '>');
            if (!tagEnd)
                break;
            ClippedImageTemplate layout;
            if (ParseAttrInt(cursor, tagEnd, "ClipRectLeft=\"NUM:",
                    &layout.clipLeft) &&
                ParseAttrInt(cursor, tagEnd, "ClipRectTop=\"NUM:",
                    &layout.clipTop) &&
                ParseAttrInt(cursor, tagEnd, "ClipRectRight=\"NUM:",
                    &layout.clipRight) &&
                ParseAttrInt(cursor, tagEnd, "ClipRectBottom=\"NUM:",
                    &layout.clipBottom) &&
                ParseAttrInt(cursor, tagEnd, "ID=\"STR:",
                    &layout.controlId))
            {
                const char* pos = std::strstr(cursor, "Pos=\"VECTOR:");
                if (pos && pos < tagEnd)
                {
                    char* endX = nullptr;
                    layout.posX = static_cast<int>(
                        std::strtol(pos + 12, &endX, 10));
                    if (endX)
                    {
                        while (*endX == ' ' || *endX == '\t' || *endX == ',')
                            ++endX;
                        layout.posY = static_cast<int>(
                            std::strtol(endX, nullptr, 10));
                    }
                }
                parsed.push_back(layout);
            }
            cursor = tagEnd + 1;
        }
    }

    AcquireSRWLockExclusive(&g_clipLock);
    g_pendingTemplates = std::move(parsed);
    g_pendingWindowW = windowW;
    g_pendingWindowH = windowH;
    const int count = static_cast<int>(g_pendingTemplates.size());
    ReleaseSRWLockExclusive(&g_clipLock);
    GameNativeLog(L"[clip] layout images=%d window=%dx%d", count, windowW,
        windowH);
}

void GameNativeNoteTileOffset(int x, int y)
{
    AcquireSRWLockExclusive(&g_clipLock);
    g_pendingTileX = x;
    g_pendingTileY = y;
    ReleaseSRWLockExclusive(&g_clipLock);
}

void GameNativeNotePluginWindow(void* window, int windowId)
{
    if (!window || windowId < 0)
        return;

    PluginWindowClip slot;
    slot.window = window;
    slot.windowId = windowId;

    AcquireSRWLockExclusive(&g_clipLock);
    slot.tileX = g_pendingTileX;
    slot.tileY = g_pendingTileY;
    slot.templates = g_pendingTemplates;
    ReleaseSRWLockExclusive(&g_clipLock);

    BindTemplateImages(slot);
    slot.collected = true;

    AcquireSRWLockExclusive(&g_clipLock);
    PruneClosedWindows();
    auto existing = std::find_if(g_pluginWindows.begin(),
        g_pluginWindows.end(), [window](const PluginWindowClip& current) {
            return current.window == window;
        });
    if (existing == g_pluginWindows.end())
    {
        g_pluginWindows.push_back(std::move(slot));
        existing = g_pluginWindows.end() - 1;
    }
    else
    {
        *existing = std::move(slot);
    }
    ApplyWindowClips(*existing);
    InterlockedExchange(&g_pluginWindowCount,
        static_cast<LONG>(g_pluginWindows.size()));
    GameNativeLog(L"[clip] plugin window=%p id=%d images=%d templates=%d tile=%d,%d",
        window, windowId,
        static_cast<int>(existing->images.size()),
        static_cast<int>(existing->templates.size()),
        existing->tileX, existing->tileY);
    ReleaseSRWLockExclusive(&g_clipLock);
    GameNativeArmGameThreadPump();
}

void GameNativeRefreshPluginClips()
{
    if (!g_clipReady ||
        InterlockedCompareExchange(&g_pluginWindowCount, 0, 0) == 0)
        return;

    AcquireSRWLockExclusive(&g_clipLock);
    const DWORD now = GetTickCount();
    if (!g_lastPruneAt || now - g_lastPruneAt >= kClipPruneMs)
    {
        PruneClosedWindows();
        g_lastPruneAt = now;
        InterlockedExchange(&g_pluginWindowCount,
            static_cast<LONG>(g_pluginWindows.size()));
    }
    for (PluginWindowClip& slot : g_pluginWindows)
    {
        slot.images.erase(std::remove_if(slot.images.begin(),
            slot.images.end(), [](const ClippedImage& image) {
                return !UsesImageDraw(image.image);
            }), slot.images.end());
        if (slot.images.empty() && slot.collected && slot.window)
            RebindSlotImages(slot);
        if (slot.images.empty())
            continue;
        ApplyWindowClips(slot);
    }
    ReleaseSRWLockExclusive(&g_clipLock);
}

void GameNativeStartClipSupport()
{
    if (g_clipReady)
        return;
    g_clipReady = true;
    GameNativeArmGameThreadPump();
    GameNativeLog(
        L"[clip] plugin-window clips follow origin; official draw not hooked");
}

void GameNativeStopClipSupport()
{
    g_clipReady = false;
    InterlockedExchange(&g_pluginWindowCount, 0);
}
