# M18 统一拓扑数据模型（Phase 3）

状态：已交付（真机验证：双节点 + wifi/batman 双链路，稳定 ID）

## 开工前 10 问（摘要）

1. **解决**：上层不再依赖 batctl/iwinfo 输出——统一 Node/Link 模型 + `ubus topology` 快照 API，为交互式拓扑（Phase 4+）提供稳定数据接口。
2. **原生**：OpenWrt 无统一拓扑视图；数据源仍为已验证的 monitor/batman_parse 采集层。
3. **复用**：r21 验收过的 batman_parse 纯解析单元 + monitor 采集；announce 的 MAC（mesh 口优先）作对端解析键。
4. **自研**：仅组装策略（MAC→node 解析、方向归一、去重、外来节点合成）。
5. **交互**：batctl/iwinfo → parser → om_link → om_topology_build（纯函数）→ ubus `topology` → LuCI（Phase 4+）。
6. **存储**：快照即算即弃，零持久化；**布局（x/y/zoom）明确不在模型中**——未来独立 TopologyLayout（Phase 10），网络态与 UI 态永不混合。
7. **ubus**：`topology`（无参）→ `{timestamp, nodes[], links[]}`；ACL read 已加。节点字段 id/hostname/model/ip/mac/role/status/uptime/client_count/last_seen；链路 id/source/target/type/status/interface/tq/rssi/tx_rate/last_seen（0=未知省略）。source/target 一律引用 Node.id。
8. **恢复**：只读快照，无事务。
9. **测试**：宿主 test_topology（15 断言：双机 batman/单节点/离线恢复/链路消失恢复/多 originator/ethernet 混合/坏数据跳过/空拓扑/外来节点/大小写���敏感/去重/方向归一/全长 ID 无碰撞）+ test_batman（解析 11 场景）+ 真机 topology 双链路验证。
10. **兼容**：`get_links` 原样保留（Phase 2 验收产物），topology 为新增统一面。

## 关键设计决定

- **稳定 ID**：Node.id = 64 位 hex node_id（Ed25519 派生，r18 验证的身份体系）；不用 hostname（可变）、不让前端用 MAC 做 UI ID。外来节点（活着但未被管理）合成 `mac-<小写mac>` 占位节点，保证每条链路引用真实节点。
- **链路 ID 方向归一**：`min(src,dst)|max(src,dst)|type|iface`——无向关系在两个方向下同一 ID；去重按 ID。
- **真机踩坑记录**：链路 ID 缓冲区最初 80 字节，装不下 两个 64 位 node_id+type+iface（≈145）→ 截断后两条链路撞 ID 被"去重"误杀——加宽至 160 并补全长回归测试。这是"短 ID 测试通过、真 ID 现场"才能暴露的坑。
- **解析/组装分层**：batman_parse.c（零依赖纯函数，宿主可测）→ topology.c（纯组装，中性输入）→ ubus_api.c（薄壳：daemon 结构→中性输入→序列化）。daemon 侧 shell 采集一律 `timeout 5` + 落文件。

## 实现后回填（r21）

- 真机：`ubus call openmesh topology` → 2 节点（controller+x86-node 全字段）+ 2 链路（wifi rssi -26 / batman tq 215），source/target 均为 node_id 引用。
- 排障插曲：临时探针用脚本清除误伤 free 循环 → 双重 free → daemon 崩溃（ubus 超时）——已手工修复；教训：**临时调试代码一律手工删，不用正则批量清**。
- 已知边界：节点 cpu/memory/temperature 字段已预留未填（待代理遥测，V0.3+）；agent 的 client_count 暂为 0；控制器 hostname 用字面 "controller"（待接系统 hostname）。

## 下一阶段（Phase 4+，未开工）

ubus topology → LuCI 拓扑页消费（现有 M15 双视图页保留并行）→ 无限画布/拖拽/缩放/自动布局/布局持久化（TopologyLayout 独立结构）→ 实时更新（先 polling；事件订阅接口已预留语义）。

## Phase 1/2 最终验收（r21b，2026-10-26）

- **分层落地**：`batman_collect.c`（执行层：跑 batctl、可配置超时、不解释任何字节）∥ `batman_parse.c`（纯解析：字符串入/FILE 入双入口、零运行时依赖、不执行命令/不碰文件/不识 ubus/UCI）。fixture 真机采集存 `tests/fixtures/`。
- **超时防御**（唯一确认缺失项）：UCI `openmesh.main.batctl_timeout`（默认 2s、钳位 1..30）→ `timeout` 杀卡死 batctl；system() 收割 shell 零僵尸；主循环最坏停顿=超时值（batctl 正常 <50ms；全异步需重构已验证的 monitor 循环，明确不做，见"剩余风险"）。
- **COLLECT_FAILED ≠ 离线**：采集失败时 monitor **内存保留**最近已知 batman 链路 90s（`batman_stage`），`get_links` 新增 `batman_collect: ok|stale|failed`（增量字段，LuCI 无感）；节点在线状态来自会话层，与采集完全解耦。日志策略=状态迁移一次性告警（失败 WARN/恢复 NOTE），不刷屏。注意：shell 重定向会先截断传输文件，故保留只能在内存——命令层测试固化了该契约。
- **测试**：parser 16 场景（含 CRLF/异常空白/消失恢复/12 行批量/字符串 API 等价性）+ 命令层 3 场景（假 batctl：成功复放 fixture/挂死被杀/命令缺失 + 僵尸检查 + 超时界 2s）。
- **真机回归 vs r20/0967c98 基线**：get_links（wifi -27/batman via）、topology（双节点/tq 231/interface）、M9→M10 与 M10→M9 originators 全部一致；唯一差异=新增 `batman_collect` 字段（设计内增量）。
