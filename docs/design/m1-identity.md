# M1 身份与密钥模块

状态：已交付（分支 feat/m1-identity，合并回 main）

## 开工前 10 问

1. **解决什么问题**：给每个节点一个唯一的、可验证的身份（Ed25519 keypair），作为 M2 发现协议签名与 M4 领养/会话认证的信任根基。没有它，后续一切"防伪造节点"都是空谈。
2. **OpenWrt 原生能力**：无节点身份概念。dropbear/hostapd 的主机密钥是服务密钥，语义不同，不可复用。
3. **可复用项目**：libsodium（feeds/packages 1.0.19，已缓存于本地 dl/）；Ed25519/SHA256 全部用它。
4. **为什么自己开发**：只自研"身份生命周期"粘合层（生成/持久化/原子写/防篡改校验）；密码学零自研。
5. **模块交互**：main 启动时 `om_identity_init()`（加载或生成）；M2 discovery 用 `om_identity_sign`；M4 会话用 pk/id；M3 ubus status 将暴露 node_id。
6. **配置存储**：`/etc/openmesh/identity`（41 字节：`OMID` magic + ver + 32B seed + 4B 完整性 tag）。目录 0700、文件 0600；生成时 tmp+fsync+rename 原子写（防掉电半文件）。
7. **ubus API**：M1 不注册（M3 的 `status` 返回 node_id hex）。
8. **出错恢复**：文件不存在 → 首启生成；文件存在但损坏/校验不符 → **拒绝启动并报错，绝不静默重新生成**（防止控制器身份漂移导致全网节点失配）；掉电安全靠原子写。
9. **测试**：`tests/test_identity.c`（宿主机 x86）：生成+权限+大小 / 重载一致性 / 签名验签往返 / 篡改消息拒绝 / 错误公钥拒绝 / 损坏文件拒绝加载——共 18 项断言；真机验证文件权限与日志 node_id。
10. **版本兼容**：libsodium 1.0.x API 冻结（1.0.18/1.0.19 双版本在 23.05~master feed 均可用）；C 代码沿用 M0 文件头约定（`_POSIX_C_SOURCE 200809L`）。

## 设计

```c
struct om_identity {
    bool generated;          /* 本次启动新生成？ */
    uint8_t pk[32];          /* Ed25519 公钥（对外身份） */
    uint8_t sk[64];          /* 私钥（仅内存） */
    uint8_t id[32];          /* node_id = SHA256(pk) */
};

int  om_identity_init(struct om_identity *, const char *dir);   /* 0 / -1 */
int  om_identity_sign(const struct om_identity *, const uint8_t *m, size_t, uint8_t sig[64]);
int  om_identity_verify(const uint8_t pk[32], const uint8_t *m, size_t, const uint8_t sig[64]);
void om_id_to_hex(const uint8_t id[32], char out[65]);
```

- 私钥落盘只存 32B seed（sk 可由 seed 确定性导出），减少敏感材料。
- tag = SHA256(seed) 前 4 字节，检测位翻转/截断（非安全边界，是可用性护栏）。
- `sodium_init()` 在 `om_identity_init` 内调用（幂等）。

## 实现后回填

- 宿主机单测：`tests/host-build.sh` → **28 项断言通过**（libsodium 1.0.19 静态编译，源码取自 OpenWrt dl/ 缓存，无需 root；注意 1.0.19 官方包根目录名为 `libsodium-stable`，脚本用 `--strip-components=1` 兼容）。
- 实现期单测抓到的 bug：`om_identity_init` 加载路径未清零结构体，`generated` 为栈上垃圾值 → 入口 `memset` 修复。
- 真机（AX3000T，0.0.1-r2 + libsodium 1.0.19-r1）：
  - 首启生成 `/etc/openmesh/identity`（41B，0600，目录 0700）
  - 日志：`node_id=f15cbd55...a36c64`，heartbeat 正常
  - 损坏演练：篡改 seed 字节 → 重启 → **拒绝启动**（进程为空，日志提示修复方法）；还原备份 → 同一 node_id 回归，身份未漂移
- ADR-0004 随本模块定稿：libsodium（实证 OpenWrt mbedtls 包无 Ed25519）。
