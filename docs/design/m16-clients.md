# M16 客户端列表（第一片：控制器本机 AP）

状态：已交付（r17 真机 DRILL-OK：真实客户端逐字段对照通过）

## 开工前 10 问

1. **解决**：用户看不到"谁在我的网络上"。第一片先做控制器本机 AP 的在线客户端表（MAC/SSID/RSSI/速率）；mesh 节点侧客户端聚合（经会话通道拉取）留双机阶段。
2. **原生**：OpenWrt 无统一"全网客户端"视图；本机数据源 `iwinfo assoclist`（24.10 实测格式）+ `iwinfo info`（ESSID 行），均为项目已依赖的 iwinfo。
3. **复用**：monitor.c 既有轮询骨架（popen + 防御式解析）与 next_slot 模式。
4. **自研**：仅解析与只读 ubus 方法；不自研采集协议（V1.0 才做计数类）。
5. **交互**：monitor 轮询（随 announce 间隔）→ `get_clients` → LuCI overview 新增"本机客户端"表；拓扑页不动（M15 刚稳定）。
6. **存储**：零持久化，纯运行态快照（与 links 同生命周期）。
7. **ubus**：`get_clients`（无参）→ `{clients:[{mac, ifname, ssid, rssi, rx_kbps, tx_kbps}]}`；ACL read 追加。
8. **恢复**：只读无状态；iwinfo 缺失/格式漂移 → 空表不报错（防御式解析）。
9. **测试**：宿主机回归全绿；SDK 编译；真机 r17：设备上有真实客户端（实测 phy1-ap0 两台：-49/-58 dBm），`ubus get_clients` 必须逐字段对上 `iwinfo assoclist` 原文。
10. **兼容**：assoclist 输出格式以 24.10 实测为准（`MAC -49 dBm / -92 dBm (SNR 43) ms ago` + `RX:/TX: MBit/s` 行）；23.05 老格式（每字段一行）不匹配时退化为空表，不崩溃。

## 实测格式锚点（2026-10-25，192.168.40.1 / 24.10-SNAPSHOT）

```
iw dev:  Interface phy1-ap0 … type AP        ← AP 口枚举（mesh 是 type mesh）
iwinfo <if> info:     ESSID: "XiaoMi_AX3000T_5G"
iwinfo <if> assoclist:
5C:66:6C:5F:38:43  -49 dBm / -92 dBm (SNR 43)  3630 ms ago
	RX: 6.0 MBit/s                                   695 Pkts.
	TX: 780.0 MBit/s, VHT-MCS 9, 80MHz, VHT-NSS 2       364 Pkts.
```

注意：AP 站点格式与 mesh peer（`signal:`/`tx bitrate` 每字段一行）**完全不同**——这正是 monitor.c 现有解析器不能直接复用的原因。

## 实现后回填（2026-10-25，r17 真机验收）

**结论：r17 DRILL-OK。** 设备 phy1-ap0 两个真实客户端（5C:66:…38:43 / CE:17:…91:87）经 `ubus get_clients` 输出与 `iwinfo assoclist` 原文逐字段对照一致（RX 6.0 MBit/s→6000 kbps、TX 780.0→780000、1200.9→1200900，RSSI 随时间正常漂移 ±2 dBm）。

实现落点：
- `monitor.c`：`poll_ap_clients()`（`iw dev` 枚举 `type AP` 口 → `iwinfo info` 取 ESSID → `iwinfo assoclist` 逐站解析）；`om_sta` 表上限 32，随 announce 间隔整体重记。
- `ubus_api.c`：`get_clients` → `{clients:[{mac, ifname, ssid, rssi, rx_kbps, tx_kbps}]}`；ACL read 已补。
- `overview.js`：状态卡之后新增「本机客户端」表（5s 轮询，复用页内既有 poll）。zh_Hans +5 条。

真机排障两课（八轮之前就停手了，这次三轮定位，靠的是"字节级取证 + 宿主机复刻"）：
1. **RX/TX 解析全空（真缺陷，已修）**：assoclist 的 `RX:`/`TX:` 行行首是 **TAB**，sscanf 字面量 `"RX: …"` 不跳前导空白 → 永不命中。修为 `" RX: …"`（格式串前导空格可吞任意空白 run，含 TAB）。这正是 mesh 解析器当年用 `" signal:"` 前导空格的同款坑——**新解析器落地前先 `sed -n l`/base64 看行首字节**。
2. **ssid 偶发缺失（非缺陷，已定位）**：daemon 重启后**第一次**轮询里 `iwinfo info` 偶发空输出（无线栈未稳/首访竞态），下一轮（10s）自愈；日志实证后续每轮 ssid 正常。LuCI 5s 轮询下用户无感，不做补丁。

已知边界：mesh 节点侧客户端（经会话通道聚合）留双机阶段；`connected_time` 未采（`ms ago` 字段语义是"最后活动距今"，不是关联时长，不采不错）。

## 实现后回填（客户端上拓扑画布，Phase 6.2 增量）

**结论：`get_clients` 数据接入拓扑画布，控制器 AP 客户端以扇形围绕卡片下方呈现。**

- `topology.js`：并行拉取 `get_clients`（初始渲染 + 5s 刷新各一次）；新增 `client-layer` SVG 层与 `renderClients()`——客户端圆点（r=16，RSSI 三档着色：≥-60 绿 / ≥-75 橙 / 红）在控制器卡片底边为锚、半径 100 的下半圆弧上均布；细线连接 AP；点击选中（橙色外环）并在详情面板展示 MAC/SSID/接口/RSSI/收发速率（复用 M16 字段，kbps→Mbps 四舍五入）。
- **几何约束（测试暴露的真坑）**：相邻客户端弦长 = 2·R·sin(π/(2(N+1)))，必须 > 2·CLIENT_R=32px，否则圆点重叠——初版 R=75/N=12 弦长仅 18px（测试 C6 拦下）；定稿 R=100/N=8（弦长 34.7px）。初版角度公式多加 π/2 导致整扇偏在节点左侧，已改为对称于正下方。
- 溢出：每 AP 最多渲染 8 个，超出显示"+N 更多客户端"；拖拽节点时客户端跟随（pointermove 中重绘）。
- 测试：`tests/test_topology_clients.js` 13 项——从 topology.js 真实源码正则提取 `clientColor/clientPos` 在 VM 中执行（非复刻），覆盖阈值/对称性/半径/防重叠/溢出/选中/详情格式化/平移不变性。
- 已知边界：客户端全部挂控制器（M16 目前只有本机 AP 数据）；mesh 节点侧客户端聚合后，改用 announce MAC 关联各 AP 再分扇。
