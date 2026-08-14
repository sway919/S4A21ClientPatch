# S4A21 ClientPatch

S4A21 国服客户端过壳补丁。用 `az.dll` 和 `GameGaurd.dll` 替换壳相关模块，游戏本体和官方 `ijl15.dll` 保持不变。

### 调用链路

```text
az.dll
  → Hook ___security_init_cookie
  → 按 iat_table.h 修复 EXE IAT
  → LoadLibraryA("GameGaurd.dll")

GameGaurd.dll
  → 静态兼容补丁
  → Cipher::Encrypt / Decrypt 改为 memcpy 透传（服务端发明文）
  → nengine::ISocket::Write 改为 staging 追加
```

客户端通过 `GetModuleHandleW("az.dll")` 取得假 TxSafe 的 `CreateObj`。GameGaurd 会把对应加密字符串写成明文 `"az.dll"`。

### 构建和部署

Visual Studio 2022 打开 `ClientPatch.sln`，构建 `Release|Win32`。将生成的 `az.dll`、`GameGaurd.dll` 放到客户端根目录。

### 静态补丁

- 跳过 TCLS / QQSafeStorage / 启动校验
- 跳过 CHANNELINFO 加密 key 段
- 禁止启动时最小化其它窗口
- 代码页 GBK(936) → UTF-8(65001)
- 版本签名 / 游戏状态校验
- 还原 packer 破坏的函数序言
- NetworkProc 固定 XOR 解密 NOP（服务端明文）
- n5 校验入口改为直接返回成功
