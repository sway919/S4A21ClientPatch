# 游戏内换装插件

`EquipmentSwap` 以插件目录放到客户端，提供“姆Q换装”面板。每个角色按官方角色 ID 保存 6 套装备方案并绑定快捷键。方案包含普通装备、时装、宠物、宠物装备（红/蓝/绿）和公会勋章，共 28 个槽位。

插件依赖 `GameNative.dll` 挂载界面、分配窗口，并通过它写游戏内聊天公告。

## 功能

- 为每个角色保存 6 套装备、时装、宠物、宠物装备和公会勋章方案。
- 支持通过面板按钮或方案快捷键执行换装。
- 背包位置变化后先按品质种子查找，找不到再按物品 ID。
- 逐件复用客户端原生穿戴流程。官方校验失败时继续其余槽，并提示可能在副本换装 CD。

## 安装

客户端目录结构：

```
GameGaurd.dll
GameNative.dll
GameNative.ini
EquipmentSwap.dll
EquipmentSwap.ini
EquipmentSwap\
  UI\
    EquipmentSwap.xui
  Data\
    <角色ID>.mq
```

1. 把 `GameNative.dll`、`GameNative.ini`、`EquipmentSwap.dll`、`EquipmentSwap.ini` 放到客户端根目录，和 `GameGaurd.dll` 同级。
2. 把 `EquipmentSwap` 文件夹放到客户端目录，里面只放 `UI` 和 `Data`。
3. 在 `GameGaurd.ini` 中先注册 `GameNative.dll`，再注册 `EquipmentSwap.dll`。
4. 设置 `EquipmentSwap.ini` 的 `Enabled=1`，然后完整退出并重新启动客户端。

`Data` 会在首次保存时自动创建。方案按官方角色 ID 写成 `<ID>.mq`，不使用昵称，也不使用数据库。

## 使用

进入角色后，按 `WindowHotkey`（默认 `End`）打开面板：

1. 选择一个方案页签。
2. 先在游戏原生界面穿好目标装备、时装和宠物。
3. 点击“读取”，把当前穿戴保存到方案。
4. 点击“修改”为方案设置快捷键，再点击“保存”。
5. 点击“换装”或直接按方案快捷键执行。

“清空”会删除当前方案记录。切换角色后按新的角色 ID 自动读取对应文件。

副武器和光环皮肤不参与记录和换装。光环只记录光环本身。守护珠走独立的镶嵌流程，不参与换装。

存档格式 v2 起新增宠物装备和勋章槽位；v1 旧档读取时自动迁移（新增槽位为空），下次保存即写成 v2。

## 配置

```ini
[General]
Enabled=1
Debug=0
WindowHotkey=End
```

- `Enabled`：是否启用插件。
- `Debug`：是否写入插件目录下的 `EquipmentSwap.log`。
- `WindowHotkey`：打开或关闭面板的快捷键，默认 `End`。
- `EquipmentSwap\Data\<角色ID>.mq`：该角色的方案、快捷键和槽位记录。

## 开发说明

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。换装在游戏主线程逐件执行，复用客户端原生穿戴流程。服务端仍负责最终验证。游戏内公告走 `GameNative`。
