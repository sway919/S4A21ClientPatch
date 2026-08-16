# 多开

解除客户端的单实例限制。

## 安装

1. 将 `MultiInstance.dll` 和 `MultiInstance.ini` 放到 `GameGaurd.dll` 所在目录。
2. 在同目录的 `GameGaurd.ini` 中注册插件：

   ```ini
   [Plugins]
   Plugin1=MultiInstance.dll
   ```

3. 完整退出所有客户端并重新启动。

## 配置

```ini
[MultiInstance]
Enabled=1
Debug=0
```

- `Enabled`：插件总开关。设为 `1` 启用。
- `Debug`：设为 `1` 时写入 `MultiInstance.log`，并发送 OutputDebugString。

## 开发说明

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。插件入口为 `ClientPatchPluginInit`。
