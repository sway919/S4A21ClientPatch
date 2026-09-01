# 功能扩展

本目录是 `GameGaurd` 加载的功能插件源码。过壳和兼容补丁在仓库根目录的 `az`、`GameGaurd`。

| 目录 | 作用 |
| --- | --- |
| [GameNative](GameNative/README.md) | 游戏原生能力，供其他插件调用 |
| [ImeFix](ImeFix/README.md) | 现代输入法兼容插件 |
| [AutoFire](AutoFire/README.md) | 连发 |
| [EquipmentSwap](EquipmentSwap/README.md) | 游戏内换装 |
| [DpsMeter](DpsMeter/README.md) | 地下城伤害统计 |
| [CombatPower](CombatPower/README.md) | 个人信息页战力侧栏 |
| [PreventMinimize](PreventMinimize/README.md) | 阻止启动时最小化其他窗口 |
| [MultiInstance](MultiInstance/README.md) | 多开 |

在 `ClientPatch.sln` 里这些工程位于 Extensions 分组。各插件的功能说明见上表对应 README。

## GameGaurd.ini 扩展顺序

插件由客户端根目录、与 `GameGaurd.dll` 同级的 `GameGaurd.ini` 加载。`[Plugins]` **按文件里的书写顺序**依次加载，不是按 `Plugin0`、`Plugin1` 的数字大小。

`GameNative.dll` 必须写在所有依赖它的插件前面。初始化时会按这个名字查找已加载模块；写在它前面或漏写时，连发、换装、DPS、战力会绑不上原生能力，表现为没有聊天公告、换装窗口挂不上。

推荐写法与仓库 `GameGaurd/GameGaurd.ini` 一致：

```ini
[Plugins]
Plugin0=PreventMinimize.dll
Plugin1=MultiInstance.dll
Plugin2=GameNative.dll
Plugin3=AutoFire.dll
Plugin4=EquipmentSwap.dll
Plugin5=DpsMeter.dll
Plugin6=CombatPower.dll
Plugin6=ImeFix.dll
```

| 插件 | 和 GameNative 的顺序 |
| --- | --- |
| PreventMinimize | 不依赖，可写在前面 |
| MultiInstance | 不依赖，可写在前面 |
| GameNative | 依赖它的插件都写在它后面 |
| AutoFire | 必须在 GameNative 之后 |
| EquipmentSwap | 必须在 GameNative 之后 |
| DpsMeter | 必须在 GameNative 之后 |
| CombatPower | 必须在 GameNative 之后 |
| ImeFix | 不依赖，可写在任意位置 |

新增依赖 GameNative 的插件时，追加到 `GameNative.dll` 那一行下面，不要插到它上面。不要给 `GameGaurd.ini` 加 UTF-8 BOM，否则整节插件都不会加载。
