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

## 第二片：回滚事务审计与加固（2026-11-10，r22）

**真机状态：AX3000T 与 x86 均不在开发机可达网络（SSH/HTTP 实测不通）→ 全部真机项 BLOCKED，未编造任何验证结果。本片按任务书以静态审计 + 宿主机模拟完成。**

### 回滚事务审计结果（任务 §三 10 问）

| # | 问题 | 结论 |
|---|---|---|
| 1 | 回滚由谁执行 | daemon 内 `txn_timer_cb`（uloop 30s 定时器）或 `apply_rollback` 手动触发 |
| 2 | 是否依赖主循环 | 定时器路径依赖 uloop；**daemon 崩溃时兜底是 boot 时 `om_netconf_recover`**（闪存 marker + 快照） |
| 3 | 是否依赖 LuCI/ubus | 否——定时器与恢复均不经过 RPC；RPC 断开不影响回滚 |
| 4 | daemon 崩溃后能否回滚 | 能（procd respawn → recover 读 marker 恢复快照）；崩溃窗口内坏配置存活数秒 |
| 5 | network reload 后能否回滚 | 能——回滚只操作文件 + 再次 reload，与 netifd 状态无耦合 |
| 6 | 重启后未完成事务恢复 | 有（TXN_MARKER 含 files 数；recover 逐个还原） |
| 7 | 两个并发 port_apply | 安全——`nc->active` 互斥，第二个直接 -1（设备脚本 S11 步验证） |
| 8 | 旧确认误确认新事务 | **原缺陷**：confirm/rollback 不带事务身份 → 已修复（gen 绑定，见下） |
| 9 | 快照是否覆盖全部受影响配置 | wireless+network 双文件快照；**发现遗漏**：`openmesh.backhaul.lan_ports` 在 apply 时立即写入、回滚后不同步 → 已修复 |
| 10 | 回滚后 bridge 成员/接口状态恢复 | 文件级恢复 + reload 重建；**发现两个残留路径**（uci staging 残留、purge 不清 ports 列表项）→ 已修复 |

### 审计发现与最小修复（5 项）

- **F1 事务 ID 绑定**（§三"确认操作应绑定唯一事务 ID"）：`apply_confirm/apply_rollback` 接受可选 `gen`（不匹配即拒绝）；`mesh_create/port_apply/roaming_apply` 回复新增 `generation`；向导确认/回滚按钮携带 gen（旧页面不可能误确认新事务）；代理自动确认固定确认自身当前事务（gen 0/false）。CLI 不带 gen 仍确认当前事务（向后兼容，文档注明）。
- **F2 apply 失败路径不恢复快照**（原代码只删快照不还原——wireless 已 commit 的 om 段会残留）：fail 路径改为先逐个恢复快照再删除，并清 staging。
- **F3 uci staging 残留**：hybrid 脚本中途失败时 uci-CLI 增量滞留 `/tmp/.uci/network`，会被下一次**任何人**的 commit 合并污染已恢复的文件（r11 事故的变体）。新增 `drop_uci_staging()`：rollback / recover / apply-fail 三路径在文件恢复后清 staging。
- **F4 purge 不清 bridge ports 列表项**（M9 踩坑 #3 的回滚侧重现）：purge 现先 del_list om_bat 与各 om_eth device，再删段。
- **F5 回滚后 backhaul 意图不同步**：新增 `om_store_sync_backhaul_ports()`——rollback/recover 后按 network 中实际幸存的 om_eth 段重写 `backhaul.lan_ports`。

### 宿主机模拟（fake-uci 升级为两阶段 staging 语义）

`test_hybrid_lanports.sh` 的伪 uci 升级为真实 uci CLI 语义（变更入 `$STATE.stag`、读可见、commit 合并、可丢弃），新增 TX 系列 16 项断言：commit 中途失败已提交文件零变化且残留 staging 存在（TX1）、丢弃 staging 后干净可继续（TX2）、回滚序列=快照恢复+staging 丢弃+purge（含 ports 清理）全量复原（TX3）、回滚=字节级快照还原（TX4）、五项源级保证（TX5）。

### 交付物（待设备可达即执行）

- `tests/evidence/om-evidence.sh`：设备侧证据采集（network/wireless/openmesh/dhcp 配置 + ip/bridge/ubus/batctl/topology 运行态 + SHA256，打 tar 包）。
- `tests/port_txn_device.sh`：§五 x86 单机验收 S0-S12 步进脚本（发现正确→plan→错误 gen 拒绝→30s 自动回滚→文件字节比对→运行态→重复/并发→正确 gen 确认），任何失败立即中止写入。
- ipk r22（mipsel_24kc 零警告）；真机清单见下方四态表。
