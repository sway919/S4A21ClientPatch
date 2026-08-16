# PreventMinimize

阻止客户端启动时最小化其他窗口。

## 安装

1. 将 `PreventMinimize.dll` 放到 `GameGaurd.dll` 所在目录。
2. 在同目录的 `GameGaurd.ini` 中注册插件：

   ```ini
   [Plugins]
   Plugin0=PreventMinimize.dll
   ```

3. 完整退出并重新启动客户端。

## 开发说明

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。插件入口为 `ClientPatchPluginInit`。
