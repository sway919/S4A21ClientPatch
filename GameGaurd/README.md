# GameGaurd

`GameGaurd.dll` 负责客户端兼容补丁和插件加载。

```ini
[Plugins]
Plugin0=PreventMinimize.dll

[Debug]
; 打开后会在客户端目录写入 GameLog.log 和 CipherPacket.log
Enabled=1
```

插件按 `[Plugins]` 中的配置项顺序加载，支持相对路径和绝对路径，最多加载
`64` 个。插件导出 `ClientPatchPluginInit` 时会在加载后调用。

`[Debug] Enabled=1` 时会写入：

- `GameLog.log`：客户端 GameLog
- `CipherPacket.log`：收发报头

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。
