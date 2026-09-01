# S4A21 ClientPatch

S4A21 客户端过壳补丁。用 `az.dll` 和 `GameGaurd.dll` 替换壳相关模块，游戏本体和官方 `ijl15.dll` 保持不变。

| 目录 | 作用 |
| --- | --- |
| `az/` | 过壳入口，修复 EXE IAT 后加载 `GameGaurd.dll` |
| `GameGaurd/` | 静态兼容补丁和插件加载器 |
| [`Extensions/`](Extensions/) | 功能扩展插件源码 |

### 调用链路

```text
az.dll
  → Hook ___security_init_cookie
  → 按 iat_table.h 修复 EXE IAT
  → LoadLibraryA("GameGaurd.dll")

GameGaurd.dll
  → 静态兼容补丁
  → Cipher::Encrypt / Decrypt 改为 memcpy 透传（服务端发明文）
  → nengine::ISocket::Write 改为 staging 追加（官方原版环形缓冲完整移植）
  → 回城修复：MTUPD::StartListening 直接返回成功（0x28A0600 处 6 字节），UDP 监听线程不再创建
  → 按 GameGaurd.ini [Plugins] 加载功能插件
```

客户端通过 `GetModuleHandleW("az.dll")` 取得假 TxSafe 的 `CreateObj`。GameGaurd 会把对应加密字符串写成明文 `"az.dll"`。

### 构建和部署

Visual Studio 2022 打开 `ClientPatch.sln`，构建 `Release|Win32`。全部工程静态链接 CRT（`/MT`），不依赖 VC++ 运行库。将生成的 `az.dll`、`GameGaurd.dll` 和 `Extensions/` 下各插件的 dll、ini 放到客户端根目录。依赖游戏原生能力的功能插件写在 `GameNative.dll` 后面。

### 静态补丁

- 跳过 TCLS / QQSafeStorage / 启动校验
- 跳过 CHANNELINFO 加密 key 段
- 版本签名 / 游戏状态校验
- 还原 packer 破坏的函数序言
- NetworkProc 固定 XOR 解密 NOP（服务端明文）
- n5 校验入口改为直接返回成功
- hook内嵌页面地址指向

### Soul 再次挑战兼容边界

Soul（PVF 同时标记 `ancient dungeon` 与 `risk dungeon`）在标准结算后由服务端发送
原生 `EPLP_RECHALLENGE(0x0105)`，body 为单字节 `09`。A21 客户端已经实现该通知的
消费；ClientPatch 不应拦截、改写或重复生成它。

`WorldMap/UI/Common/Common.xui` 是魂图与 Anton 模式按钮共用的系统布局。功能插件不得
通过 GameNative 挂载、缓存或去重这个路径；未挂载的系统 XUI 必须继续交给客户端原生
loader。排查再次挑战后持续内存增长时，可临时设置 `GameNative.ini` 的 `Debug=1`，检查
`GameNative.log` 中该路径是否快速重复加载；诊断完成后恢复为 `0`。

### 功能扩展

功能插件源码在 [`Extensions/`](Extensions/)，由 `GameGaurd.ini` 的 `[Plugins]` 按顺序加载。DLL 和 ini 仍拷到客户端根目录（与 `GameGaurd.dll` 同级）。换装的 XUI 和方案数据放在同级的 `EquipmentSwap` 文件夹。

| 插件 | 说明 |
| --- | --- |
| [GameNative](Extensions/GameNative/README.md) | 游戏原生能力：聊天公告、外挂 XUI、窗口 ID。没有独立界面，供其他功能插件调用。 |
| [ImeFix](Extensions/ImeFix/README.md) | 现代输入法兼容插件 |
| [AutoFire](Extensions/AutoFire/README.md) | 键盘连发、组合动作和鼠标连点。自动判断游戏中的输入状态，打字时不合成。 |
| [EquipmentSwap](Extensions/EquipmentSwap/README.md) | 游戏内换装面板。聊天公告和窗口走 GameNative。 |
| [DpsMeter](Extensions/DpsMeter/README.md) | 地下城伤害统计。独立窗口；载入和开关公告走 GameNative。 |
| [CombatPower](Extensions/CombatPower/README.md) | 个人信息页战力侧栏。载入公告走 GameNative。 |
| [PreventMinimize](Extensions/PreventMinimize/README.md) | 阻止客户端启动时最小化其他窗口。 |
| [MultiInstance](Extensions/MultiInstance/README.md) | 解除客户端单实例限制。 |

依赖 `GameNative` 的插件必须写在它后面。`GameGaurd.ini` 不能带 UTF-8 BOM。详情见各插件目录里的 README。
