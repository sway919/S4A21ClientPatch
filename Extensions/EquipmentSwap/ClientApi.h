#pragma once

#include <string>

#include <windows.h>

namespace equipment_swap::client_api
{
// 地址与签名均按当前 A21 客户端核对。校验失败时只禁用读装和换装，不影响开窗。
bool ValidateClient();
bool IsReady();
int ReadItemId(int listType, int slot);
std::wstring CurrentCharacterName();
// 官方本地用户 uniqueId：19E3290(*03A5C980) 后读 user+8 的 uint16。
// 读不到或为 0 视为未进角色。
unsigned int CurrentCharacterId();

// 读取角色当前穿戴列表中的物品 ID；装备列表为 3。
// 时装 0-10（光环是 9），装备 12-23，宠物 25，宠物装备 26-28，勋章 31。
// 不读光环皮肤 11、副武器 24。
int ReadEquippedItemId(short destinationSlot);

// 品质种子在 info+0x2CC（ItemCore.Value）。时装/宠物上同字段另有语义，换装匹配仅对普通装备使用。
int ReadItemQualitySeed(int listType, int slot);
int ReadEquippedQualitySeed(short destinationSlot);

// 复用手动拖拽装备使用的原生穿戴事务：校验、组包、本地交换和界面刷新均由客户端完成。
bool MoveItemNative(int sourceListType, int sourceSlot,
    int destinationSlot);
}
