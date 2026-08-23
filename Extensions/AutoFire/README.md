# 连发

- 键盘连发、组合动作和鼠标连点。
- 聊天、搜索、起名等输入态暂停连发。中文输入法开着但不在输入框时照常连发；按住连发键期间暂时拆掉窗口 IME。

## 安装

1. 将 `AutoFire.dll` 和 `AutoFire.ini` 放到 `GameGaurd.dll` 所在目录。
2. 先加载 `GameNative.dll`（聊天公告走它），再注册连发：

   ```ini
   [Plugins]
   Plugin1=GameNative.dll
   Plugin2=AutoFire.dll
   ```

3. 完整退出所有客户端并重新启动。

## 配置

```ini
[General]
Enabled=1
ToggleKey=PGUP
ReloadKey=PGDN
ForegroundOnly=1
Debug=0
KeyPressDurationMs=16
HoldThresholdMs=0

[RapidFire]
X=1
```

- `Enabled`：启动时总开关。
- `ToggleKey`：连发总开关，默认 `PGUP`。
- `ReloadKey`：重新读取配置，默认 `PGDN`。
- 载入、`PGDN` 刷新、`PGUP` 开关时，通过 `GameNative` 写客户端聊天栏公告。
- `ForegroundOnly`：设为 `1` 时仅当前台客户端工作。
- `Debug`：设为 `1` 时写入 `AutoFire.log`。
- `KeyPressDurationMs`：合成按下保持时间。战斗按帧读键，建议 16。
- `HoldThresholdMs`：只作用于键盘连发和鼠标连点。默认 0。组合按下立刻执行。输入状态由插件自动判断，不必在这里配置。
- `[Mouse]`：`Trigger=MButton`，按住中键连点。总开关仍是 `PGUP`。
- `[RapidFire]`：`物理键=连发周期毫秒`。按住时改 DirectInput 键表。
- `[Combo0]` 到 `[Combo15]`、`[Mouse]` 的完整说明见 `AutoFire.ini`。

## 开发说明

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。插件入口为 `ClientPatchPluginInit`。
