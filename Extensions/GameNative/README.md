# GameNative

功能插件用的游戏原生能力基础组件。没有独立界面。连发、换装等业务 DLL 通过这里调用客户端原有能力。

## 能力

- 聊天栏公告。
- 从客户端目录外挂载 `.xui` 布局。
- 为业务插件分配不冲突的窗口 ID，并把窗口创建分发给对应插件。
- 为插件窗口处理布局里声明的图像裁剪。

## 安装

1. 将 `GameNative.dll` 和 `GameNative.ini` 放到 `GameGaurd.dll` 所在目录。
2. 在同目录的 `GameGaurd.ini` 里把它写在所有依赖它的功能插件之前：

   ```ini
   [Plugins]
   Plugin1=GameNative.dll
   Plugin2=AutoFire.dll
   Plugin3=EquipmentSwap.dll
   Plugin4=DpsMeter.dll
   ```

3. 完整退出所有客户端并重新启动。

## 配置

```ini
[General]
Debug=0
```

`Debug` 只控制是否写入 `GameNative.log`。

## 系统 XUI 边界

GameNative 只接管插件显式挂载的虚拟路径；没有挂载的路径必须原样调用客户端 loader。
不要挂载或拦截 `WorldMap/UI/Common/Common.xui`。该布局由魂图与 Anton 难度切换共用，
屏蔽或缓存它会让合法的模式切换使用陈旧界面。

服务端完成 Soul 标准结算时应发送原生 `EPLP_RECHALLENGE(0x0105)`、body `09`；
GameNative 不提供替代通知或客户端侧重试状态机。需要诊断异常重载时临时启用 `Debug=1`，
按 `[load]` 记录检查上述路径，完成后关闭调试日志。

## 开发说明

业务插件包含 `GameNativeApi.h`，调用 `ClientPatchBindGameNative`，或自己 `GetProcAddress("ClientPatchGetGameNativeApi")`。

聊天公告：

```cpp
ClientPatchGameNativeApi api = {};
if (ClientPatchBindGameNative(&api) && api.postChatNotice)
    api.postChatNotice(L"连发已开启", api.chatRgb(255, 72, 220));
```

启动时的「插件已载入」用 `postLoadNotice`。各插件只交一次，GameNative 等官方聊天可写后再统一等 5 秒，按入队顺序写出。运行中的开关、刷新仍走 `postChatNotice`，聊天一可写就发。

`chatReady` 在官方聊天可写时为真。布局窗口：

1. `mount` 挂载虚拟 XUI 路径。
2. `reserveWindowIds` 申请窗口 ID。
3. `registerWindowFactory` 注册窗口工厂。
4. 卸载时依次关闭窗口、注销工厂、释放窗口 ID、卸载路径。

`owner` 必须稳定且唯一。虚拟路径用 `/` 的相对 `.xui` 路径，磁盘路径必须是绝对路径。窗口层级见 [WINDOW_ID_CATALOG.md](WINDOW_ID_CATALOG.md)。

使用 Visual Studio 2022 构建 `ClientPatch.sln` 的 `Release|Win32` 配置。插件入口为 `ClientPatchPluginInit`。
