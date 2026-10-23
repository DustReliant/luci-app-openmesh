# M2 发现协议 v1

状态：已交付（分支 feat/m2-discovery，合并回 main）

## 开工前 10 问

1. **解决什么问题**：零配置前提下，节点在 L2 局域网内互相发现；控制器获得"待领养节点"列表（M6 页面数据源）。这是用户目标 1（自动发现）的地基。
2. **OpenWrt 原生能力**：umdns 存在但承载弱、不可签名、非默认安装（ADR-0003 已否决）；无其他可用机制。
3. **可复用项目**：密码学全部 libsodium（M1 已接入）；协议本体自研（属编排层核心职责）。
4. **为什么自己开发**：发现报文必须带 Ed25519 签名（LAN 不可信原则），通用发现协议均无签名承载。
5. **模块交互**：discovery 不依赖 uloop/UCI（纯 POSIX+sodium，宿主机可测）；main 负责接线（uloop fd/timer 驱动 `recv_ready`/`tick`）与 UCI 装配（config.c）；新节点回调目前只打日志，M3 接 ubus 事件。
6. **配置存储**：只读 UCI（main.enabled/role/announce_interval/announce_port），本模块不写任何配置；表在内存，无落盘。
7. **ubus API**：M1/M2 均无（M3 注册 `openmesh` 对象时把 node 表暴露为 `list_pending`）。
8. **出错恢复**：包解析全程防御（长度/magic/版本/签名/ts 窗口/nonce 环五道闸），坏包只丢不崩；socket 非阻塞，sendto 失败记日志继续；表满淘汰最旧；节点 3×interval 未见面被 GC。
9. **测试**：宿主机 `test_discovery.c`（协议单测 + 同进程双端点 socket 互通 + 淘汰/GC）；真机 `tests/loopback.sh`（双实例互发现，DoD：2s 内）；升级后系统实例 + 手动实例真机演示。
10. **版本兼容**：纯 POSIX UDP + libuci 稳定 API（已从 24.10 staging 头文件核实 `uci_lookup_option`）；dest 地址可注入（默认 255.255.255.255，测试用 127.0.0.1），避免 CI 环境真发广播。

## 设计

**报文**（≤512B，二进制头 + JSON 体）：

```
0    magic "OMAN"(4) | ver(1) | ts u64BE(8) | nonce(24) | node_id(32) | pubkey(32)   = 101B
101  body: {"role":"..","model":"..","fw":".."}\0   （变长，含 NUL 参与签名）
尾    Ed25519 sig(64)，覆盖 [5, body 末]；node_id 必须 == SHA256(pubkey)（防身份嫁接）
```

**五道校验闸**（顺序即成本递增）：长度/魔数/版本 → id 与 pk 绑定 → 签名 → ts 窗口 ±90s（本机时钟可信即 >2023 才启用，防出厂无 NTP 设备误杀）→ nonce 环（每节点 8 槽 LRU，防重放）。

**节奏**：interval=10s ±20% 抖动（随机库=randombytes，防同步风暴）；启动后首个 tick 立即发一次（新设备 2s 内可见）；GC = max(30s, 3×interval)。

**表**：≤32 节点，满则淘汰 last_seen 最旧者；新节点触发一次 `on_new` 回调。

## 已知取舍

- 每包必验签（Ed25519 verify ≈ 数十 µs@MT7981，32 节点×10s 周期无压力）；广播洪水攻击下的验证限流留到 V0.4 安全加固。
- body 用极简扁平 JSON 扫描器（role 必需，model/fw 可选默认 unknown）；转义字符不支持（自有协议，字段不携带引号）。

## 实现后回填

- 宿主机：`test_discovery.c` **212 项断言**通过（+ identity 28 项回归通过），构建零警告。
- 真机（AX3000T，0.0.1-r4）：`tests/loopback.sh` 双实例 **2 秒内互相发现**（DoD 达成），announce 携带 role/model/fw（型号自动识别 "Xiaomi Mi Router AX3000T (OpenWrt U-Boot layout)"、fw=24.10-SNAPSHOT）。
- 实现期踩坑：loopback 最初依赖"本机广播回环"，但 255.255.255.255 走默认路由 egress（WAN），本机其他端口监听者收不到 → 增加 `-A dest_addr` 参数（生产默认仍为 255.255.255.255，测试用 127.0.0.1）。此坑同样解释了真机双设备场景为何无影响（广播真实出接口）。
- 防重放/防篡改在单测中逐项验证：同 nonce 重放拒绝、篡改 body/pubkey/魔数/长度/时间戳拒绝、签名有效但缺 role 字段拒绝、表满淘汰最旧、GC 生效。
- 遗留：广播洪水下的验证限流（V0.4）；`-A` 参数仅限调试/测试用，不做 UCI 化。
