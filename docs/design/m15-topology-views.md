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

## r16 真机验收回填（2026-10-25，192.168.40.1 / AX3000T / 24.10-SNAPSHOT）

**结论：r16 DRILL-OK，M15 真机验收通过。**

- 验收路径：main(4e463d8) 重建双包（openmeshd 0.0.1-r18 + luci-app + zh_Hans i18n）→ 部署 → 验证 → 活网演练。
- **踩坑一（构建）**：本地 openwrt 树里 `package/luci-app-openmesh` 是 setup-dev.sh 的**拷贝**而非软链（luci.mk 的 po 发现不吃软链），改前端源码后不重跑 setup-dev.sh 会构建出旧 ipk（本次构建出 9-13 的化石包）。脚本注释本有此警告——命中。
- **踩坑二（部署考古）**：设备上原 luci-app 是 09726fd（M6/M7 时代）构建、openmeshd 却是 r18——长期"后端新前端旧"混搭，M8/M11/M13 的前端改进从未上过这台设备；本次一并归一。
- **发现并修复真缺陷**：mesh_ifname 采用"见过就保留"语义，30s 回滚下线后仍残留上报（Mesh 云会显示不存在的接口）→ 改为每轮 poll 清空重记（与 links 同语义，最长滞后一个 announce 间隔 10s）。修复后活网验证：mesh_create → 13s 后 mesh_ifname=phy1-mesh0 上报 ✓；apply_rollback → 13s 后 mesh_ifname 清除 ✓；get_links 全程无错（单节点空表）✓；wireless 与演练前逐字节一致 ✓。
- **断言时序教训**：mesh_ifname 依赖 monitor 周期轮询（10s），演练断言必须留足一个轮询窗口（首版 5s 断言误报失败）。
- 首次演练失败退出后无人善后，30s 事务自动回滚兜底生效——M7 安全网再次自证。
- 遗留（不阻塞）：浏览器端渲染目检（页面 JS 已部署且经 uhttpd 提供、语法校验通过，但未做带登录的 GUI 截图验证——留待用户顺手确认）；mesh 云信道展示仍留 V0.3。
