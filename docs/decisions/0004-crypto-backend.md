# ADR-0004 节点通信加密后端：libsodium

- 状态：已接受（2026-10-23，M1 实证后定稿）
- 影响版本：V0.1（M1）

## 决策

**libsodium**（OpenWrt packages feed，1.0.19，ISC 许可）。协议所需原语全部使用其一等公民 API：Ed25519（身份/签名）、X25519（会话 ECDH）、BLAKE2b/HMAC-SHA256、ChaCha20-Poly1305、HKDF（M4 会话层实现）。

## 定稿实证（2026-10-23，本地 openwrt-24.10 树）

OpenWrt 的 mbedtls 3.6.7 包（`package/libs/mbedtls/Makefile`）不可用，原因是硬性缺失而非取舍：

- 构建选项全集中**无任何 Ed25519 模块**；
- PSA 仅启用 `CONFIG_MBEDTLS_PSA_CRYPTO_CLIENT`（客户端接口），未启用完整 `MBEDTLS_PSA_CRYPTO_C` 实现，而 mbedtls 3.6 的 Ed25519 只能经完整 PSA 栈提供；
- Curve25519 仅作为 ECDH 曲线可选（`ECP_DP_CURVE25519_ENABLED`），不含签名。

Ed25519 身份签名是协议（发现 announce 签名、领养 challenge、会话互验）的硬需求 → libsodium 是当前 OpenWrt 生态内唯一不引入自建加密代码的选项。

## 协议原语映射

| 原语 | libsodium API |
|---|---|
| 节点身份签名/验签 | `crypto_sign_ed25519_detached` / `crypto_sign_ed25519_verify_detached` |
| node_id 派生 | `crypto_hash_sha256` |
| 会话密钥协商（M4） | `crypto_scalarmult_curve25519` |
| 帧加密（M4） | `crypto_aead_chacha20poly1305_ietf` |
| 密钥派生（M4） | `crypto_kdf_blake2b` |

## 后果

- openmeshd 新增依赖 `+libsodium`（约 +300KB flash，可接受；RSS 增量微小）
- 节点协议的安全属性上限 = libsodium 的默认参数（misuse-resistant，符合嵌入式团队维护定位）
