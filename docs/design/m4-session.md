# M4 会话通道与节点领养

状态：已交付（r6 真机 ADOPT-OK）

## 开工前 10 问

1. **解决什么问题**：控制器与节点间的加密管理通道 + 领养（用户目标 3：授权/认领）。发现协议只解决"看见"，领养解决"信任建立"：身份互验 → 网络密钥安全分发 → 持续在线管理。
2. **OpenWrt 原生能力**：ubus/uci 用于本地集成；无跨节点安全会话组件可复用（TLS 栈太重且证书体系不匹配自组网）。自建最小安全通道（Noise 风格），密码学全部 libsodium。
3. **可复用项目**：OMM 的 mTLS 思路被否（证书分发复杂）；借鉴其"enrollment"语义。
4. **为什么自己开发**：协议即产品核心；380 行内可实现完整握手+加密帧。
5. **模块交互**：session.c 纯逻辑（无 uloop/无 socket，宿主机可测）；session_net.c 挂 uloop（控制器 accept/agent 出站连接、心跳、退避重连）；ubus_api 暴露 list_pending/list_adopted/adopt；discovery 表是 pending 的来源。
6. **配置存储**：UCI 增读 main.controller_addr/main.session_port/main.join_open；网络密钥与控制器 pin 落节点状态目录（M5 再迁 UCI 白名单）。
7. **ubus API**（新增）：
   - `list_pending {}` → 发现表减去已领养
   - `list_adopted {}` → {id,name,online,last_seen}
   - `adopt {id, name?}` → 进入待领养；事件 `openmesh.node.adopted`
   - ACL 增补相应方法
8. **出错恢复**：每对端独立状态机，解析失败/验签失败/解密失败只断开该对端；连接超时 5s、握手步骤 10s、心跳 15s×3 判死；agent 未领养快速重试（5s），已领养指数退避 5→120s；daemon 崩溃由 procd 兜底（M0 已验）。
9. **测试**：宿主机 test_session.c（帧编解码/双方密钥一致/加解密往返/密文篡改/错钥/计数器错位/签名伪造）；真机 tests/adopt-test.sh：系统实例=控制器，手动实例=agent(-j)，list_pending→adopt→list_adopted online→agent 落 network.key。
10. **版本兼容**：纯 POSIX TCP + libsodium；无版本差异点。

## 设计

**帧**（统一头）：`magic u32("OMS1") | type u8 | counter u32 | len u32 | payload`；握手帧 counter=0 明文，会话帧 ChaCha20-Poly1305（len 含 16B tag）。

**握手**（agent 发起，3.5 步）：
```
A→C HELLO    { eph_pub[32], id[32], pk[32], nonce1[24] }        明文
C→A CHALLENGE{ eph_pub[32], nonce2[24] }                        明文
A→C PROOF    { sig[64] = Ed25519(nonce1‖nonce2) }               明文（防重放靠双 nonce）
C→A GRANT    { network_key[32], ctrl_id[32] }                   加密（首次领养）
    / WELCOME{ }                                                加密（已领养节点回归）
A→C ACK      { }                                                加密
```
密钥：`shared=X25519(eph_priv,peer_eph)`；`K=BLAKE2b(shared‖n1‖n2‖agent_pk‖ctrl_pk)`；方向密钥 `K_A2C=BLAKE2b(K‖"A2C")`、`K_C2A=BLAKE2b(K‖"C2A")`；帧 nonce=方向计数器（u64 LE）+4B 零。中间人不可行：双向 pk 均被 pin（agent 侧 pin 首次 GRANT 的 ctrl_id；controller 侧只认白名单/pending id）。

**控制器判定**：id 在 adopted → CHALLENGE；id 在 pending（用户已点 adopt）→ CHALLENGE+GRANT；未知 → 断开（agent 5s 后再来）。**agent 开关**：仅 join_open=1（uci 或 -j）时接受 GRANT 并落盘——"未领养节点不接受配置"红线。

**角色规则**：role=auto 时：有 controller_addr → agent；无 → controller（监听 session_port）。

## 实现后回填

（待填）

## 实现后回填

- 宿主机 test_session 35 断言通过（含抓出的 seal 缓冲区溢出 bug）。
- 真机 ADOPT-OK：pending→adopt→加密会话→network.key 落 agent →节点 online；伪造/未知节点被控制器直接断开。
- 实现修正：KDF 输入去掉 controller 长期公钥（首联时 agent 不可知），改为握手期双方已知量 + agent 侧 pin GRANT 内 ctrl_id。
- 已知限制（记录在案）：首联存在"抢先领养"竞窗（TOFU 模型），join_open 仅在入网窗口开启缓解，M6 的 PIN 确认流将关闭此窗口；agent 状态丢失而控制器保留时，需用户重新 adopt（V0.1 无自动重协商）。
