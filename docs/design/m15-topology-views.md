# M15 拓扑双视图（网络拓扑 + Mesh 拓扑）

状态：已交付（代码齐备 + SDK 双包编译零警告 + 宿主机回归全绿；真机验收待 r16）

## 开工前 10 问

1. **解决**：当前拓扑页只有一种图（控制器中心的管理视图），网络物理结构与 Mesh 逻辑结构混在一起。拆为两个视图：「网络拓扑」看被管网络全貌（控制器/已领养/已发现/回程链路），「Mesh 拓扑」只看 802.11s/batman-adv 逻辑网（mesh 云 + peer 边 RSSI + originator 选路）。
2. **原生**：LuCI 无现成双拓扑视图；数据源全部来自本项目 ubus（list_nodes/list_adopted/list_pending/get_links）+ 新增 get_mesh_info（读已持久化的 backhaul 段，无新状态）。
3. **复用**：复用 M13 拓扑 v2 的绑定算法（MAC↔node）与 RSSI 配色；不引入图库（workflow §1 红线：自绘 SVG）。
4. **自研**：仅前端视图拆分 + 一个只读 ubus 方法；无新协议、无新存储。
5. **交互**：ubus `get_mesh_info` ← store（/etc/config/openmesh backhaul 段）；LuCI topology.js 双 tab 切换，poll 共用。
6. **存储**：零新增。mesh_id/band/mode 已由 mesh_create 持久化（store.c backhaul 段）；get_mesh_info 只读。
7. **ubus**：`get_mesh_info`（无参）→ `{configured, mesh_id, band, mode, key_set, mesh_ifname}`；ACL read 追加 `get_mesh_info` 与 `get_links`（后者为 M11 遗漏，真机 root 会话未暴露，受限用户会被拒——本次一并修复）。
8. **恢复**：只读接口无状态无回滚；mesh_ifname 由 monitor 轮询时缓存，缺失时返回空串（前端显示未知）。
9. **测试**：宿主机回归（host-build.sh 全绿）；`node --check` JS 语法；SDK 双包编译；真机验收留待 r16（单机：向导建 mesh 后 Mesh 视图出现 mesh 云；未建 mesh 时显示引导空态）。
10. **兼容**：纯 blobmsg 只读方法 + 前端字符串，无版本差异点；新 msgid 同步 zh_Hans po。

## 设计

### 前端（topology.js）

- 页首 tab 条（复用 LuCI `cbi-tabmenu` 样式，内存态切换，poll 时保持当前 tab）。
- **网络拓扑**：现 v2 图原样迁移（CTRL 中心 + 已领养内环 + 未绑定外环 + 待领养底条）+ 回程链路表。
- **Mesh 拓扑**（新 `drawMesh`）：
  - `configured=false` → 引导空态（提示去向导页建网）；
  - `configured=true` → 顶部 mesh 云节点（mesh_id + band + mode），下挂 peer 边：wifi 边按 RSSI 配色标注 dBm/速率，batman 边蓝色标注 via 下一跳；未绑定 peer 以虚线呈现在外环；无任何链路时显示"单节点/等待对端"提示。

### 后端（openmeshd）

- `monitor.c`：poll_wifi_links 已枚举 mesh 接口，顺手缓存 `mesh_ifname`。
- `ubus_api.c`：新增 `om_get_mesh_info`（store backhaul 段 + monitor 缓存拼装）。

## 实现后回填（2026-10-25）

- **后端**：`ubus get_mesh_info`（无参只读）→ `{configured, mesh_id, band, mode, key_set[, mesh_ifname]}`。mode 由新增 `om_store_backhaul_mode()` 推断（network 包存在 `proto batadv` 接口 = hybrid，否则 wifi）；mesh_ifname 由 monitor 轮询 mesh 接口时顺手缓存（无 mesh 时为空）。密钥不回传（只回 key_set 布尔）。
- **前端**：topology.js 顶部 `cbi-tabmenu` 双 tab，内存态切换（poll 5s 保持当前 tab）。「网络拓扑」= 原 v2 图 + 回程链路表原样迁移；「Mesh 拓扑」= mesh 云（mesh_id + band/mode/接口名）→ CTRL → 半圆排布 peer 边（wifi 按 RSSI 配色、batman 蓝色标 via），未托管 peer 外环虚线，零链路时提示单节点；未建 mesh 时显示引导空态并给导向导页的按钮。
- **顺带修复**：rpcd ACL read 缺 `get_links`（M11 遗漏——root 会话不受影响，受限用户会被拒），本次连同 `get_mesh_info` 一并补入。
- **i18n**：zh_Hans po 新增 5 条（网络拓扑/未建 mesh 引导两句/等待对端/未托管对端）。
- **验证**：`node --check` JS 语法通过；JSON/po 校验通过；宿主机单测 276 断言全绿（identity 28 + session 35 + discovery 213）；24.10 树 `build.sh` 双包 clean+compile 零警告；smoke：openmeshd 65585B ≤ 150KiB 预算。
- **已知边界**：Mesh 视图数据源仍是本机 get_links（单机视角），双机后 peer/originator 真数据自然进入两视图，无需再改；mesh 云不显示信道（iwinfo 信道采集留 V0.3 监控任务一并做）。
- **真机验收清单（r16）**：未建 mesh 时拓扑页 Mesh tab 显示引导；向导建 mesh 后显示 mesh 云与接口名；hybrid 模式 mode=hybrid。
