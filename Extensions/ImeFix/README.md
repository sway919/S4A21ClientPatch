# ImeFix

现代输入法兼容插件

- 中文等非英文输入法可以正常输入并发送消息。
- 选词、退格或取消输入后，候选窗口会正确消失。
- 已启用兼容模式的输入法继续使用游戏原生处理。
- 不修改系统输入法设置，也不需要替换输入法文件。

## 安装

将 `ImeFix.dll` 和 `ImeFix.ini` 放到 `GameGaurd.dll` 所在目录，并在同目录的
`GameGaurd.ini` `[Plugins]` 中加入：

```ini
Plugin6=ImeFix.dll
```

插件不依赖 `GameNative`。修改插件文件或配置后，完全退出客户端再重新启动。

## 配置

`ImeFix.ini` 只包含两个选项：

```ini
[General]
Enabled=1
Debug=0
```

`Enabled=0` 可停用插件。`Debug=1` 仅用于排查问题，会在客户端目录生成
`ImeFix.log`；正常使用请保持 `Debug=0`。

## 构建

在 `ClientPatch.sln` 中构建 `Release|Win32`。
