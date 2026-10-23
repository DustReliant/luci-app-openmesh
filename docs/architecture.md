# OpenMesh Manager 总体架构

| | |
|---|---|
| 版本 | v1-draft |
| 日期 | 2026-10-23 |
| 状态 | 架构师已提出，待 Owner 评审冻结 |
| 评审要点 | ① Controller/Agent 模型 ② 回程三模式与 V0.2 默认模式 B ③ 加密后端（ADR-0004）④ V0.1 含原生 802.11s（ADR-0006）⑤ 仓库名是否改 openmesh-manager（ADR-0005） |

## 1. 总体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│ 用户浏览器                                                        │
│  LuCI (luci-app-openmesh, 仅控制器安装)                           │
│   首页/节点/拓扑/向导 —— 纯 JS 视图 (23.05/24.10/25.x 兼容)        │
└───────────────┬─────────────────────────────────────────────────┘
                │ ubus JSON-RPC (经 rpcd ACL 鉴权)
┌───────────────▼─────────────────────────────────────────────────┐
│ openmeshd (C, libubox/libubus/libuci/libsodium, ~150KB)           │
│ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌───────���─┐ ┌─────────────┐  │
│ │ubus_api │ │discovery│ │ session │ │ netconf │ │   monitor   │  │
│ │对象+事件 │ │UDP ann. │ │TCP 帧协议│ │配置事务  │ │ iwinfo/FDB  │  │
│ └─────────┘ └─────────┘ └─────────┘ └─────────┘ └─────────────┘  │
│        ↑全部基于 uloop 单线程事件循环, per-peer 隔离               │
└──┬──────────┬──────────┬──────────┬──────────┬──────────────────┘
   │ubus      │uci       │netifd    │hostapd/  │iwinfo
   ▼          ▼          ▼          ▼          ▼
┌─────────────────────────────────────────────────────────────────┐
│ OpenWrt 原生数据面 (我们不修改、不重写)                            │
│  br-lan ── bat0 (V0.2+) ── [ 802.11s mesh0 (wpad-mesh) │ eth ]   │
└─────────────────────────────────────────────────────────────────┘
```

管理面：**Controller/Agent 星型模型**（Agent 主动出站 TCP 连 Controller，避免入站方向/NAT 问题；管理面挂了数据面照常工作 = fail-static）。数据面：节点间对等（11s/batman）。控制器冗余 → V0.4（ADR-0001）。

## 2. 模块划分

```
openmesh-manager/  (mono-repo, 每个子目录 = 一个 OpenWrt ipk)
├── openmeshd/          # 核心 daemon (C)：discovery/session/crypto/netconf/monitor/ubus
├── luci-app-openmesh/  # LuCI 界面 (仅控制器需要；Agent 可 headless 省 flash)
├── openmesh-proto-docs/# 协议规范文档 + 消息定义（并入 docs/ 亦可，M0 定）
├── scripts/  tests/  docs/  README.md
```

依赖：`luci-app-openmesh → openmeshd → libubus, libubox, libblobmsg-json, libuci, libsodium`；V0.2 追加 `wpad-mesh-mbedtls, kmod-batman-adv(feeds/routing), mesh11sd(feeds/routing)`。

## 3. daemon 架构

单进程 `openmeshd`，uloop 单线程事件循环（无锁、省内存；ubus/定时器/网络 IO 同一 loop）：

| 模块 | 职责 |
|---|---|
| identity | 首启生成 Ed25519 keypair（`/etc/openmesh/`，0600），node_id=SHA256(pubkey) |
| discovery | UDP :7689 announce 收发（签名、去重、replay 窗口、限速） |
| session | TCP :7688（controller 监听 / agent 出站长连接），帧解析与状态机 |
| netconf | 配置事务引擎：snapshot → apply → confirm(30s) → commit / 自动 rollback |
| monitor | 定时采集 iwinfo CLI（RSSI/速率/客户端）、bridge FDB（有线链路推断）、/proc 资源 |
| store | 运行态 `/var/run/openmesh/state.json`（tmpfs 无磨损）；静态度 UCI；密钥文件 |
| ubus_api | 注册 `openmesh` 对象 + 发送 `openmesh.*` 事件 |
| log | ulog → syslog（限速防刷盘） |

资源预算（CI 冒烟断言）：二进制 ≤150KB、RSS ≤3MB、announce ≤512B、state.json ≤32KB。
健壮性铁律：所有网络输入防御性校验；单 peer 出错只丢弃帧/断连，绝不让 daemon 退出；procd respawn 兜底。

## 4. LuCI 架构

标准 23.05+ JS 视图（luci-base rpc.js/uci.js/form.js 已确认存在，三版本兼容）：

```
luci-app-openmesh/
├── htdocs/luci-static/resources/view/openmesh/
│   ├── overview.js / nodes.js / topology.js / node-detail.js / wizard.js
├── usr/share/luci/menu.d/luci-app-openmesh.json
├── usr/share/rpcd/acl.d/luci-app-openmesh.json   # 最小权限：仅 openmesh.* ubus + 相关 uci
└── Makefile (DEPENDS:=+openmeshd)
```

- 数据获取全走 `ubus openmesh.*`（`rpc.declare`）；状态轮询用 LuCI 原生 `poll.add`（5s）
- 拓扑图自绘 SVG（按跳数分层布局），不引入 vis-network 等重型图库
- 不用 WebSocket（rpcd 不支持）

## 5. 节点通信架构

| 平面 | 载体 | 内容 |
|---|---|---|
| 发现面（无连接） | UDP 广播/组播 :7689，10s±抖动 | 签名 announce：node_id/pubkey/角色/IP/型号/固件/能力集/ts/nonce/sig |
| 会话面（有连接） | TCP :7688，Agent 出站长连接；帧= magic(2)+ver(1)+type(1)+len(4)+payload(JSON)+MAC | 握手/领养/配置推送/心跳/指标上报 |

payload 用 JSON（blobmsg-json 已在依赖里；节点 ≤32，性能无关紧要，调试友好）。
超时：连接 5s / 握手 10s / 心跳 15s×3 判离线 / 配置确认 30s。
V0.1 拓扑链路来源：agent 上报 iwinfo assoclist（无线）+ `bridge fdb` 对端 MAC 落在物理口（有线推断）。

## 6. Mesh 配置架构（回程三模式，ADR-0002）

```
模式 A 纯无线:   br-lan ── mesh0 (802.11s, SAE)          ← V0.1 默认
模式 B 有线+无线: br-lan ── bat0 ──┬─ mesh0 (802.11s)    ← V0.2 默认(推荐)
                                  └─ eth
模式 C 纯有线:   br-lan ── eth
```

- 模式 A 直接桥接 11s 无环路（HWMP 是 L2 路径选择）
- 模式 B 由 batman-adv metric 天然实现有线优先/无线备份/自动切换，**不自研任何切换逻辑**
- 配置生成器只输出 UCI（wireless/network/batman-adv），经 netifd/hostapd 生效
- **端口名一律从 `board.json` / `ubus call network.device status` 动态获取**（x86 无交换机、MT7981 是 DSA，禁止硬编码）

## 7. 数据结构设计

```c
// 节点状态机: PENDING → ADOPTING → ONLINE ↔ OFFLINE → REVOKED
struct om_node {
    uint8_t id[32];            // SHA256(pubkey)
    char name[32]; uint8_t role, status, hops;
    uint8_t *pubkey;           // Ed25519, base64 存 UCI
    uint32_t last_seen, adopted_at;
    struct om_iface { char ifname[16]; uint8_t type; int8_t rssi; uint32_t rate; } ifaces[8];
    struct om_metric { uint16_t clients; uint8_t load; } m;
    struct avl_node avl;       // libubox AVL, id 为键
};
struct om_link {               // 拓扑边, 双向去重（小 id 在前）
    uint8_t a[32], b[32]; uint8_t type;   // WIRED / MESH11S / BATADV
    char ifname[16]; int8_t rssi; uint32_t rate; mtime_t age;
};
// 配置事务: generation++ → snapshot(受影响 uci 文件 .om-bak) → apply
//           → confirm timer(30s) → commit / 自动 rollback
```

序列化：ubus 出口走 blobbuf，TCP 出口走 JSON；同一字段表（docs 内字典）生成两用，防漂移。

## 8. ubus API 初步设计

对象 `openmesh`（controller/agent 同对象按角色暴露方法；LuCI ACL 只授 `openmesh.*`）：

| 方法 | 参数 → 返�� | 说明 |
|---|---|---|
| `status` | → role/version/mesh_name/nodes_online/nodes_total | 首页 |
| `list_nodes` | → nodes[]（id/name/status/ip/model/fw/hops/last_seen） | 节点表 |
| `node_info` | {id} → radios/ifaces/links/clients/resources | 详情 |
| `topology` | → nodes[] + links[] | 拓扑 |
| `list_pending` | → 待领养节点[] | 领养横幅 |
| `adopt` / `revoke` | {id, name?} | 领养/移除（走加密会话） |
| `mesh_create` | {mesh_name, band, key?, backhaul_mode} | 一键建网 |
| `mesh_join` / `mesh_leave` | {} | Agent 侧 |
| `get_config` / `set_config` | UCI blob | 设置 |
| `apply_confirm` / `apply_rollback` | {} | 配置事务安全网 |
| `health` | → 自检 | 测试 |

事件：`openmesh.node.new / node.up / node.down / node.adopted / config.applied / config.rolledback / link.change`。V0.3 预留 `roam_events`、`get_clients`。

## 9. UCI 配置设计

`/etc/config/openmesh`（密钥不进 UCI——权限与轮转都不合适）：

```
config openmesh 'main'
    option enabled '1'
    option role 'auto'              # auto|controller|agent
    option mesh_name 'home-mesh'
    option announce_interval '10'
    option session_port '7688'
    option announce_port '7689'

config backhaul
    option mode 'wireless'          # wireless(V0.1) | auto | wired (V0.2)
    option mesh_radio ''            # 空=自动选 5G radio
    option encryption 'sae'
    option mesh_key ''              # 建网时生成，会话通道下发

config node                          # 每个 adopted 节点一条（重启可恢复）
    option id '<sha256>'
    option name 'bedroom'
    option pubkey '<base64>'
    option granted_role 'agent'
    option granted_at '<unix-ts>'

config monitor
    option interval '5'
    option offline_after '45'        # 3×心跳
```

文件布局：`/etc/openmesh/identity`（Ed25519 私钥，0600）、`/etc/openmesh/network.key`（领养后分发）、`/var/run/openmesh/state.json`（运行态）。uci-defaults 负责初始迁移。

## 10. 节点发现方案（ADR-0003）

自定义 UDP 签名广播（拒绝 umdns：承载弱/不可签名/维护差；拒绝 avahi：重；拒绝 LLDP：语义不符）：

- 各节点每 10s（±随机抖动防同步风暴）广播 `{v, node_id, pubkey, role, ip, model, owrt_ver, caps[], ts, nonce, sig}`，sig = Ed25519(ts‖nonce‖body) → **收到即验真伪，无需先建会话**
- Controller 维护 pending 表；同签名重复 announce 去重 + replay 窗口
- 领养后该节点转 TCP 会话管理，announce 降频为存在指示
- 未领养节点 role=`unadopted` → LuCI 首页横幅

## 11. 节点认证方案

原则：**LAN 不可信**。

1. **身份**：首启生成 Ed25519 keypair；node_id=SHA256(pubkey)；预留厂商预置密钥扩展
2. **领养（对抗伪造节点）**：LuCI 看到 pending → 发 challenge（签名回执证明持私钥）→ 可选物理确认（新节点页面显示 Controller 生成的 6 位 PIN；V0.1 先做此路径）→ 通过后经安全会话下发 `network.key`，pubkey 入 UCI 白名单
3. **会话加密**：X25519 ECDH → HKDF → ChaCha20-Poly1305 双向加密；每帧 64-bit 计数器 + 接收侧滑动窗口防 replay；握手双方 Ed25519 签名互验（pubkeys 双向 pin，中间人不可行）
4. **加密后端**：倾向 libsodium；备选 mbedtls 3.6.7（零新增 flash，但 Ed25519/X25519 编译开关待验证）—— M1 定夺（ADR-0004）
5. 未领养节点只广播、不接受任何配置类消息；所有入口按源 IP 限速

## 12. 故障恢复方案

| 故障 | 机制 |
|---|---|
| 配置应用后失联/变砖 | netconf 事务：snapshot → apply → 30s 无 confirm 自动 rollback（LuCI 同款倒计时交互）；**掉电**：启动发现 unconfirmed generation → 回滚 |
| openmeshd 崩溃 | procd respawn（阈值内无限重启，3600s 重置）；启动校验 state.json 与 UCI 一致性 |
| 单节点异常 | per-peer 状态机 + 输入防御校验；peer 断连只影响该 peer |
| Agent 失联 Controller | fail-static：数据面照常；指数退避重连 5s→120s；恢复后增量同步 |
| 节点离线 | 3×心跳判 offline → ubus 事件 → LuCI 告警；回来自动 re-online |
| 回程链路故障 | V0.1 模式 A：11s 自愈；V0.2 模式 B：batman-adv 自动切换（只展示） |
| Controller 硬件损坏 | V0.1/V0.2：手动指定新控制器（network.key 已在各节点，可平滑接管，协议预留）；V0.4 自动化 |

## 13. OpenWrt 版本兼容策略

| 维度 | 23.05 | 24.10（本地已验证） | 25.x/master | 策略 |
|---|---|---|---|---|
| LuCI JS API | ✅ | ✅ 资源文件已确认 | ✅ | 唯一 UI 形态 |
| rpcd ACL JSON | ✅ | ✅ | ✅ | 静态文件无差异 |
| wpad-mesh-mbedtls | ✅ | ✅ | ✅ | 固定依赖 |
| batman-adv | feeds/routing | ✅ 2024.3 | ✅ | V0.2，仅用稳定 UCI 接口 |
| mesh11sd | ✅ | ✅ feeds/routing | ✅ | V0.2 |
| hostapd ubus | ✅ | ✅ 600-ubus patch | ✅ | 方法清单 01 调研摘录 |
| DSA/端口 | — | mt7981=DSA | — | 动态读 board.json |

硬保障：GitHub Actions 矩阵 = {23.05, 24.10, master} × {x86_64, mediatek/filogic} SDK，每 PR 三端编译 + 冒烟。版本差异代码集中 `compat.h/compat.c`。

## 14. V0.1 开发任务拆解

见 [roadmap.md](roadmap.md) §2（M0–M8，含 DoD）。

## 附录 A：已本地验证事实（2026-10-23，openwrt-24.10 树）

- mesh11sd、batman-adv 2024.3 均在 `feeds/routing/`（batman-adv 源自 downloads.open-mesh.org —— 命名撞车实证）
- mbedtls 3.6.7（`package/libs/mbedtls`）
- hostapd 带 `600-ubus_support.patch`
- luci-base JS API（rpc.js/uci.js/form.js…）齐全
- 本地 `.config`：xiaomi ax3000t-ubootmod（MT7981）+ **wpad-basic-mbedtls（无 mesh 能力，M0 必须改 wpad-mesh-mbedtls）**
- umdns/lldpd 存在但本项目不采用

## 附录 B：待验证清单（M0/M1 落实，禁止编造）

- hostapd ubus 各方法名清单（从 600-ubus_support.patch 逐行摘录进 docs）
- mbedtls 3.6.7 OpenWrt 编译配置是否含 Ed25519/X25519（MBEDTLS_ED25519_C 等）
- `iwinfo --json` 在 23.05 的可用性与输出格式（无则退回文本解析）
- netifd `network.wireless status` 在 23.05/24.10 的返回结构（真机 `ubus call` 抓样本入 docs）
- 802.11k/v/r 的 hostapd UCI 选项在三个版本的实际可用集（V0.3 前完成）
