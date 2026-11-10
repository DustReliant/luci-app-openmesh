# M22 有线回程写路径 port_apply（Phase 7.2 第一片）

状态：已实现（2026-11-10；C 门禁 54 项 + 前端 20 项全绿，全套回归通过，SDK 编译零警告 ipk r21。真机执行 NOT TESTED——两台设备不在可达网络；写路径只有在设备部署并被用户显式调用时才会触碰网络配置）

> 前置：m21（发现/预检/预览）已交付。本片把预览变成**受门禁的真实事务**。

## 开工前 10 问（摘要）

1. **解决**：用户在向导选定网口后能真正把它挂到 batman 有线腿——但只在服务端重新校验通过、且走既有 30 秒事务兜底的前提下。
2. **原生**：执行复用 m19 hybrid-netconf.sh（om_eth 生成/re-home/幂等已验收）；不新写任何网络逻辑。
3. **复用**：om_netconf_mesh_apply 事务（快照/确认/回滚）、om_snet_push_mesh 会话推送、om_port_plan 预检。
4. **自研**：仅 `om_port_apply_gate` 纯门禁（ALLOW/NEED_ACK/DENY）+ ubus `port_apply` 编排 + 前端按钮/风险弹窗。
5. **交互**：向导选口 → port_plan 预览 → 应用按钮（ok 直达；mgmt_unknown 先 ui.showModal 风险确认）→ port_apply → 30s 内「保留配置」确认或自动回滚。
6. **存储**：`openmesh.backhaul.lan_ports` 更新为所选口（既有字段）；wired_mode 意图仍不落盘（纯 UI 意图，本片不实现仅有线模式）。
7. **ubus**：`port_apply {device, ack_unknown?}`（write ACL）→ `{result: applied|need_ack|denied|no_mesh, device?, reason?, confirm_window}`。
8. **恢复**：全程 m19 事务——network 快照 + 30s 定时回滚 + 断电恢复；失败路径零残留（mesh_apply 内部已处理）。
9. **测试**：门禁纯函数 17 项断言（含"所有其他 blocked 原因 ack 后仍 DENY"矩阵）+ 前端门控 5 项（P16-P20）+ 全套回归。
10. **兼容**：代理端口名不同（lan1↔eth0）沿用 m19 语义（推送后对端跳过缺失口）；对端按自身 facts 重新择口留后续。

## 关键设计决定

- **唯一可确认的风险是 mgmt_unknown**：`om_port_apply_gate` 中 mgmt/wan/vlan/dsa_cpu/mesh/virtual/master/not_exist 一律 DENY，`ack_unknown` 也无法翻越——只有"多个 br-lan 成员有 carrier 无法排除管理路径"允许用户显式确认后通过（弹窗列明后果）。测试固化此矩阵。
- **服务端重新校验**：`port_apply` 调用 `om_port_plan` 重新枚举 sysfs/UCI——前端下拉的 eligible 只是显示提示；两次校验之间端口状态变化以服务端为准（need_ack/denied 即时返回）。
- **挂在存量 Mesh 上**：无线腿参数（mesh_id/key/band）取自 `openmesh.backhaul` 持久化——没有 Mesh 时返回 `no_mesh`（先建网再加腿）。应用即把现有 Mesh 升级为 hybrid（wifi-only 也兼容，脚本幂等）。
- **推送代理**：成功后 `om_snet_push_mesh(..., "hybrid", device)`——m19 通道，代理端重新校验+缺失口跳过。
- **前端按钮门控与后端一致**：`applyEnabled = plan.ok || planNeedsAck(plan)`（peer unknown 之类不解锁）；`need_ack` 回复会重新弹窗而绝不静默应用。

## 已知边界 / 后续

- wired_only / wired_preferred 模式的实际行为（不建无线腿等）未实现——仍是意图展示；7.2 后续片处理。
- 对端按自身 facts 重新择口（解决 lan1↔eth0）未做，沿用 m19 跳过语义。
- mgmt_unknown 弹窗只提示文字，未列出具体哪些口有 carrier（ports API 已有数据，前端增强留后续）。
- 真机 DoD（x86 单机 dry-run → 双机真线缆 → AX3000T）待设备可达后执行，r21 ipk 就绪。
