# 版本路线图

> 纪律：只拆解当前版本；下一版本的细粒度任务在进入该版本前才允许拆。
> 任务编号 M0–M8 对应 V0.1；每项任务 = 一次完整的开发循环（见 workflow.md §2）。

## 1. 版本定义

| 版本 | 主题 | 范围 | 明确不含 |
|---|---|---|---|
| V0.1 | 管理面闭环 | 发现/信息/授权/领养/建网/入网/状态/拓扑；**原生 UCI 静态 802.11s**（ADR-0006） | mesh11sd、batman-adv、k/v/r |
| V0.2 | 数据面完整 | mesh11sd 集成、batman-adv 集成、有线/无线回程、回程自动选择（原生 metric） | 控制器冗余 |
| V0.3 | 无线体验 | 802.11k/v/r、客户端归属展示、漫游监控、RSSI 监控 | 信道自动优化 |
| V0.4 | 可靠性 | 故障检测、自动恢复、节点健康度、回程优化展示、信道分析 | — |
| V1.0 | 完整 Controller | 多节点管理强化、VLAN、IoT、Guest、QoS、流量统计、网络诊断 | — |

## 2. V0.1 任务拆解（M0–M8）

| # | 任务 | 范围 | DoD（验收标准） | 依赖 |
|---|---|---|---|---|
| M0 | 仓库与构建骨架 | mono-repo 三包 Makefile（openmeshd / luci-app-openmesh 空壳）、CI 三版本矩阵、License 决定、seed 配置修正（**wpad-basic-mbedtls → wpad-mesh-mbedtls**） | 空壳 ipk 在 24.10 SDK 编译产出；CI 绿；README 声明齐全 | — |
| M1 | 身份与密钥模块 | 首启生成 Ed25519 keypair、node_id=SHA256(pubkey)、签名/验签；**ADR-0004 加密后端在此定夺**（libsodium vs mbedtls 3.6.7，用测试程序验证 Ed25519/X25519 可用性） | 宿主机 x86 单测通过：生成/签名/验签/篡改拒绝；密钥文件 0600 | M0 |
| M2 | 发现协议 v1 | UDP :7689 签名 announce（10s±抖动）、去重、replay 窗口、pending 表 | loopback 双进程：A 2s 内发现 B；伪造/重放包被丢弃 | M1 |
| M3 | daemon 骨架 + ubus | uloop 主循环、procd init（respawn）、`openmesh` ubus 对象（status/health）、rpcd ACL | `ubus call openmesh status` 通；`kill -9` 后自动重生 | M0 |
| M4 | 会话通道 + 领养 | TCP :7688 帧协议、ECDH 握手、Ed25519 互验、ADOPT 全流程、状态机 PENDING→ONLINE | 双 VM 完成领养；中间人/重放/伪造全部被拒；每源 IP 限速生效 | M1, M3 |
| M5 | 状态持久化 + 恢复 | adopted 节点入 UCI、运行态 state.json（tmpfs）、崩溃/重启一致性校验 | 重启后 adopted 列表与状态完好；损坏 state.json 可自愈 | M4 |
| M6 | LuCI 首页 + 节点管理 | overview（节点表+待领养横幅）、nodes（领养/改名/移除）、menu.d + ACL | 浏览器完成 pending→adopted 全流程，无控制台报错 | M4 |
| M7 | Mesh 创建/加入 + 事务回滚 | wireless UCI 原生 802.11s（mode=mesh + mesh_id + SAE）、netconf 快照/confirm/自动回滚（30s）、LuCI 向导 | 两节点真出 mesh0 且互通；不 confirm 自动回滚可复现 | M5, M6 |
| M8 | 拓扑页 + 联调 + 发布 | 自绘 SVG 拓扑（有线=FDB 推断、无线=iwinfo）、2×AX3000T（或 QEMU×3）联调、tag v0.1、README 定稿 | 拓扑正确显示链路类型/RSSI；V0.1 出口条件全过（workflow.md §5） | M7 |

## 3. V0.2+ 概要（到达时再细拆）

- **V0.2**：模式 B 组网（11s + eth 同挂 bat0）；mesh11sd 接管动态 radio 管理；回程选择展示页（读 batman-adv originators/mesh 状态）；72h 稳定性测试。
- **V0.3**：hostapd 802.11k/v/r UCI 选项编排（**先在 01 调研确认各版本可用选项，禁止编造**）；dawn/usteer 评估（目标 15/16 可能直接复用它们而非自研）；客户端列表聚合。
  - 已开工：M14 漫游编排（已交付，见 `design/m14-roaming.md`）；M15 拓扑双视图（网络拓扑 + Mesh 拓扑分视图，见 `design/m15-topology-views.md`）；M16 客户端列表第一片（本机 AP 客户端，见 `design/m16-clients.md`）。
- **V0.4**：健康度模型、故障注入测试框架、控制器接管流程（手动指定备份）。
- **V1.0**：VLAN/Guest/IoT 编排、QoS、流量统计（复用 nl80211/ubus 计数，不自研采集协议）。
