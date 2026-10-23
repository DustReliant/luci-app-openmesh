# ADR-0004 节点通信加密后端：libsodium（倾向）vs mbedtls 3.6

- 状态：**提议中——M1 用实验定夺，本 ADR 届时更新为已接受**
- 影响版本：V0.1（M1）

## 决策（暂定）

倾向 **libsodium**（OpenWrt packages feed 已收录，ISC 许可）。备选 **mbedtls 3.6.7**（base 树已验证存在；因 wpad-*-mbedtls 已依赖 libmbedtls，选择它则零新增 flash）。

## 协议需求（后端必须提供的原语）

| 原语 | 用途 |
|---|---|
| Ed25519 签名/验签 | 节点身份、announce 签名、握手互验 |
| X25519 ECDH | 会话密钥协商 |
| HKDF / 密钥派生 | ECDH 输出 → 会话密钥 |
| ChaCha20-Poly1305（或 AES-GCM） | 帧加密 + 认证 |
| 64-bit 计数器 + 滑动窗口 | 防 replay |

## 对比

| 维度 | libsodium | mbedtls 3.6.7 |
|---|---|---|
| API 复杂度 | 极简（misuse-resistant） | 较繁 |
| 体积 | ~300KB 独立库 | 0 新增（系统已带） |
| Curve25519/Ed25519 | 原生一等支持 | 3.x 支持，**但 OpenWrt 编译配置是否开启待验证** |
| 依赖链 | 新增一个 feed 包 | 无新增 |

## M1 定夺方法（写死，不可跳过）

1. 读 `package/libs/mbedtls/Makefile` 与其 config 文件，确认 `MBEDTLS_ED25519_C`/X25519 是否启用
2. 写 ~10 行测试程序在 SDK 下编译链接两后端，验证签名/ECDH 往返
3. 产出：本 ADR 更新结论 + 测试代码入 `tests/`

## 后果

- 定夺前任何 M1 代码不得合并
- 若选 mbedtls 且开关缺失，评估向上游提 config 补丁的成本，超限则回退 libsodium
