#define NOMINMAX
#include "ClientApi.h"

#include <algorithm>
#include <cwchar>
#include <initializer_list>

namespace equipment_swap::client_api
{
namespace
{
constexpr uintptr_t kPreferredImageBase = 0x00400000;
// 01648AC0：cdecl(listType)。0/1/7 分别返回装备背包/时装/宠物列表，已核实；
// 38（勋章背包）按同一函数处理，未经实机断点验证，返回空时换装按失败计数。
constexpr uintptr_t kGetInventoryList = 0x01648AC0;
// 01648BA0：cdecl(listType, slot)；type 3/17 走角色当前穿戴。
constexpr uintptr_t kGetItemAt = 0x01648BA0;
// 016FA5F0：thiscall(list, src, dest, 1, 6, 0)。015B4762 调用点为 push 0; push 6; push 1。
constexpr uintptr_t kMoveItemNative = 0x016FA5F0;
constexpr uintptr_t kCharacterPointer = 0x03B3DFB8;
// 018A8030：thiscall，`mov eax,[ecx+0x374]; ret`，返回角色名 wchar_t*。
constexpr uintptr_t kGetCharacterName = 0x018A8030;
// 本地用户管理器。19E3290 按加密全局 uniqueId 取出当前用户对象。
constexpr uintptr_t kUserManagerGlobal = 0x03A5C980;
constexpr uintptr_t kGetLocalUser = 0x019E3290;
constexpr size_t kUserUniqueIdOffset = 8;
constexpr size_t kMaxCharacterName = 64;
// 物品对象 vtable[10] 返回 info；info+24 为模板 ID，品质/实例值在 info+0x2CC。
constexpr size_t kItemInfoIdOffset = 24;
constexpr size_t kItemInfoRepeatIdOffset = 0x2C8;
constexpr size_t kItemInfoQualityOffset = 0x2CC;
constexpr size_t kItemInfoVtableSlot = 10;

using GetInventoryList = void* (__cdecl*)(int);
using GetItemAt = void* (__cdecl*)(int, unsigned int);
using GetItemInfo = void* (__thiscall*)(void*);
using GetCharacterNameFn = const wchar_t* (__thiscall*)(void*);
using GetLocalUserFn = void* (__thiscall*)(void* manager);
using MoveItemNativeFunction = bool(__thiscall*)(void*, int, unsigned int,
    int, int, int);

bool g_ready = false;

uintptr_t ClientAddress(uintptr_t preferredAddress)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base + (preferredAddress - kPreferredImageBase);
}

bool ValidateBytes(uintptr_t preferredAddress,
    std::initializer_list<unsigned char> expected)
{
    if (!preferredAddress)
        return false;
    const auto address = reinterpret_cast<const unsigned char*>(
        ClientAddress(preferredAddress));
    __try
    {
        return std::equal(expected.begin(), expected.end(), address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

int ReadItemIdObject(void* item)
{
    if (!item)
        return 0;
    __try
    {
        auto vtable = *reinterpret_cast<uintptr_t**>(item);
        if (!vtable || !vtable[kItemInfoVtableSlot])
            return 0;
        void* info = reinterpret_cast<GetItemInfo>(
            vtable[kItemInfoVtableSlot])(item);
        return info ? *reinterpret_cast<int*>(
            static_cast<unsigned char*>(info) + kItemInfoIdOffset) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

int ReadItemQualitySeedObject(void* item)
{
    if (!item)
        return 0;
    __try
    {
        auto vtable = *reinterpret_cast<uintptr_t**>(item);
        if (!vtable || !vtable[kItemInfoVtableSlot])
            return 0;
        void* info = reinterpret_cast<GetItemInfo>(
            vtable[kItemInfoVtableSlot])(item);
        if (!info)
            return 0;
        const auto* bytes = static_cast<const unsigned char*>(info);
        const int itemId = *reinterpret_cast<const int*>(
            bytes + kItemInfoIdOffset);
        const int repeatId = *reinterpret_cast<const int*>(
            bytes + kItemInfoRepeatIdOffset);
        if (itemId <= 0 || repeatId != itemId)
            return 0;
        return *reinterpret_cast<const int*>(bytes + kItemInfoQualityOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool IsNameChar(wchar_t value)
{
    if (value >= L'0' && value <= L'9')
        return true;
    if (value >= L'A' && value <= L'Z')
        return true;
    if (value >= L'a' && value <= L'z')
        return true;
    return value >= 0x4E00 && value <= 0x9FFF;
}

void* CurrentCharacterObject()
{
    if (!kCharacterPointer)
        return nullptr;
    __try
    {
        return *reinterpret_cast<void**>(ClientAddress(kCharacterPointer));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

bool ReadCharacterNameRaw(wchar_t* output, size_t capacity)
{
    if (!output || capacity < 2)
        return false;
    output[0] = L'\0';
    if (!ValidateBytes(kGetCharacterName,
            { 0x8B, 0x81, 0x74, 0x03, 0x00, 0x00, 0xC3 }))
        return false;
    __try
    {
        void* character = CurrentCharacterObject();
        if (!character)
            return false;

        const wchar_t* source = reinterpret_cast<GetCharacterNameFn>(
            ClientAddress(kGetCharacterName))(character);
        if (!source)
            return false;
        size_t length = 0;
        while (length + 1 < capacity && source[length])
        {
            if (!IsNameChar(source[length]) || length >= kMaxCharacterName)
                return false;
            output[length] = source[length];
            ++length;
        }
        if (length == 0)
            return false;
        output[length] = L'\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        output[0] = L'\0';
        return false;
    }
}

}

bool ValidateClient()
{
    g_ready =
        ValidateBytes(kGetInventoryList,
            { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x85, 0xC0 }) &&
        ValidateBytes(kGetItemAt, { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 }) &&
        ValidateBytes(kMoveItemNative,
            { 0x55, 0x8B, 0xEC, 0x6A, 0xFF });
    return g_ready;
}

bool IsReady()
{
    return g_ready;
}

int ReadItemId(int listType, int slot)
{
    if (!g_ready)
        return 0;
    __try
    {
        void* item = reinterpret_cast<GetItemAt>(
            ClientAddress(kGetItemAt))(listType, static_cast<unsigned int>(slot));
        return ReadItemIdObject(item);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

std::wstring CurrentCharacterName()
{
    wchar_t name[kMaxCharacterName + 1] = {};
    if (ReadCharacterNameRaw(name, _countof(name)))
        return name;
    return L"";
}

unsigned int CurrentCharacterId()
{
    if (!CurrentCharacterObject())
        return 0;
    if (!ValidateBytes(kGetLocalUser,
            { 0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x54,
                0x00 }))
        return 0;
    __try
    {
        void* manager = *reinterpret_cast<void**>(
            ClientAddress(kUserManagerGlobal));
        if (!manager)
            return 0;
        void* user = reinterpret_cast<GetLocalUserFn>(
            ClientAddress(kGetLocalUser))(manager);
        if (!user)
            return 0;
        const unsigned int id = *reinterpret_cast<const unsigned short*>(
            static_cast<const unsigned char*>(user) + kUserUniqueIdOffset);
        return id;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

int ReadEquippedItemId(short destinationSlot)
{
    if (!g_ready || destinationSlot < 0 || destinationSlot > 0x7FFF)
        return 0;
    return ReadItemId(3, destinationSlot);
}

int ReadItemQualitySeed(int listType, int slot)
{
    if (!g_ready)
        return 0;
    __try
    {
        void* item = reinterpret_cast<GetItemAt>(
            ClientAddress(kGetItemAt))(listType, static_cast<unsigned int>(slot));
        return ReadItemQualitySeedObject(item);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

int ReadEquippedQualitySeed(short destinationSlot)
{
    if (!g_ready || destinationSlot < 0 || destinationSlot > 0x7FFF)
        return 0;
    return ReadItemQualitySeed(3, destinationSlot);
}

bool MoveItemNative(int sourceListType, int sourceSlot,
    int destinationSlot)
{
    // 源列表：0 装备背包、1 时装、7 宠物（含宠物装备 140-188）、38 勋章背包。
    // 目标槽：身上装备 0-31（含宠物 25、宠物装备 26-28、勋章 31）。
    if (!g_ready ||
        (sourceListType != 0 && sourceListType != 1 && sourceListType != 7 &&
            sourceListType != 38) ||
        sourceSlot < 0 || destinationSlot < 0 || destinationSlot > 31)
        return false;
    __try
    {
        void* sourceList = reinterpret_cast<GetInventoryList>(
            ClientAddress(kGetInventoryList))(sourceListType);
        if (!sourceList)
            return false;
        return reinterpret_cast<MoveItemNativeFunction>(
            ClientAddress(kMoveItemNative))(sourceList, sourceSlot,
                static_cast<unsigned int>(destinationSlot), 1, 6, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
}
