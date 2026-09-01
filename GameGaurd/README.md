# GameGaurd

`GameGaurd.dll` 负责客户端兼容补丁和插件加载。功能插件源码在仓库 [`Extensions/`](../Extensions/)。插件 DLL 和 ini 放到本文件所在的客户端目录；换装的 XUI 和方案数据放在同级的 `EquipmentSwap` 文件夹。

```ini
[Plugins]
Plugin0=PreventMinimize.dll
Plugin1=GameNative.dll
Plugin2=AutoFire.dll
Plugin3=EquipmentSwap.dll
Plugin4=DpsMeter.dll
Plugin5=CombatPower.dll
Plugin6=ImeFix.dll

[Debug]
; 打开后会在客户端目录写入 GameLog.log 和 CipherPacket.log
Enabled=1
```

插件按 `[Plugins]` 中的配置项顺序加载，支持相对路径和绝对路径，最多加载
`64` 个。插件导出 `ClientPatchPluginInit` 时会在加载后调用。依赖游戏原生能力的功能插件必须写在 `GameNative.dll` 后面。`GameGaurd.ini` 不能带 UTF-8 BOM，否则整节插件都不会加载。

`[Debug] Enabled=1` 时会写入：

- `GameLog.log`：客户端 GameLog
- `CipherPacket.log`：收发报头

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。
