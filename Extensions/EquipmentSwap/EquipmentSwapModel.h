#pragma once

#include <array>
#include <string>

#include <windows.h>

namespace equipment_swap
{
constexpr int kProfileCount = 6;
constexpr int kEquipmentSlotCount = 12;
constexpr int kAvatarSlotCount = 11;
constexpr int kPetSlotCount = 4;
constexpr int kMedalSlotCount = 1;
constexpr int kSlotCount = kEquipmentSlotCount + kAvatarSlotCount +
    kPetSlotCount + kMedalSlotCount;

struct SlotDefinition
{
    const wchar_t* label;
    short destinationSlot;
    unsigned char sourceListType;
    short sourceFirstSlot;
    short sourceLastSlot;
};

// A21 在槽 11 插入光环皮肤后，装备槽整体 +1。时装仍只覆盖 0-10，
// 光环只读槽 9；不读光环皮肤 11，也不读副武器 24。
// 宠物装备 26-28 源在宠物背包（listType 7）140-188；公会勋章 31 源在
// 勋章背包（listType 38）0-48。服务端按 PVF 类型 == 槽位号精确校验。
inline constexpr std::array<SlotDefinition, kSlotCount> kSlotDefinitions = {{
    { L"武器", 12, 0, 3, 64 },
    { L"称号", 13, 0, 3, 64 },
    { L"上衣", 14, 0, 3, 64 },
    { L"头肩", 15, 0, 3, 64 },
    { L"下装", 16, 0, 3, 64 },
    { L"鞋", 17, 0, 3, 64 },
    { L"腰带", 18, 0, 3, 64 },
    { L"项链", 19, 0, 3, 64 },
    { L"手镯", 20, 0, 3, 64 },
    { L"戒指", 21, 0, 3, 64 },
    { L"辅助装备", 22, 0, 3, 64 },
    { L"魔法石", 23, 0, 3, 64 },
    { L"帽子", 0, 1, 0, 209 },
    { L"头发", 1, 1, 0, 209 },
    { L"脸部", 2, 1, 0, 209 },
    { L"上衣装扮", 3, 1, 0, 209 },
    { L"下装装扮", 4, 1, 0, 209 },
    { L"鞋装扮", 5, 1, 0, 209 },
    { L"胸部", 6, 1, 0, 209 },
    { L"腰部", 7, 1, 0, 209 },
    { L"皮肤", 8, 1, 0, 209 },
    { L"光环", 9, 1, 0, 209 },
    { L"武器装扮", 10, 1, 0, 209 },
    { L"宠物", 25, 7, 0, 139 },
    { L"宠物红装", 26, 7, 140, 188 },
    { L"宠物蓝装", 27, 7, 140, 188 },
    { L"宠物绿装", 28, 7, 140, 188 },
    { L"勋章", 31, 38, 0, 48 },
}};

struct HotkeyBinding
{
    UINT modifiers = 0;
    UINT virtualKey = 0;

    bool IsValid() const
    {
        return virtualKey != 0;
    }
};

struct SlotSetting
{
    int itemId = 0;
    int qualitySeed = 0;
    short preferredSourceSlot = -1;
};

struct ProfileSetting
{
    std::wstring name;
    HotkeyBinding hotkey;
    std::array<SlotSetting, kSlotCount> slots = {};
};

using ProfileCollection = std::array<ProfileSetting, kProfileCount>;

inline void ResetProfiles(ProfileCollection& profiles)
{
    profiles = {};
    for (int index = 0; index < kProfileCount; ++index)
        profiles[index].name = L"方案 " + std::to_wstring(index + 1);
}

inline bool IsEquipmentDestinationSlot(short destinationSlot)
{
    // 武器 12、防具/首饰/特殊装备 14-23、宠物装备 26-28、勋章 31 按品质种子
    // 匹配，避免同模板假成功。称号 13 不按装备品质种子匹配。
    // 装备部分（含称号）只扫快捷栏 3-8 和装备页 9-64。
    return destinationSlot == 12 ||
        (destinationSlot >= 14 && destinationSlot <= 23) ||
        (destinationSlot >= 26 && destinationSlot <= 28) ||
        destinationSlot == 31;
}

// 换装执行顺序：时装 0-10 → 装备 12-23 → 宠物 25 → 宠物装备 26-28 → 勋章 31。
// 宠物装备必须在宠物本体之后换。跳过光环皮肤 11、副武器 24。
inline constexpr std::array<int, kSlotCount> kSwapSlotOrder = {{
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, // 时装 dest 0-10
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,       // 装备 dest 12-23
    23,                                         // 宠物 dest 25
    24, 25, 26,                                 // 宠物装备 dest 26-28
    27,                                         // 勋章 dest 31
}};

static_assert(kSlotDefinitions[kSwapSlotOrder[0]].destinationSlot == 0);
static_assert(kSlotDefinitions[kSwapSlotOrder[11]].destinationSlot == 12);
static_assert(kSlotDefinitions[kSwapSlotOrder[23]].destinationSlot == 25);
static_assert(kSlotDefinitions[kSwapSlotOrder[24]].destinationSlot == 26);
static_assert(kSlotDefinitions[kSwapSlotOrder[26]].destinationSlot == 28);
static_assert(kSlotDefinitions[kSwapSlotOrder[27]].destinationSlot == 31);
}
