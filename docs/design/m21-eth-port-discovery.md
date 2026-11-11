# M21 物理网口发现与有线回程配置界面（Phase 7.0/7.1）

状态：已实现（2026-11-10；自动化 52 项新测试全绿 + 92 项回归全绿 + SDK 编译零警告 ipk r20。真机验收 NOT TESTED——AX3000T 与 x86-node 均不在开发机可达网络，待接入后按 §九 补验）

## 开发前架构审计（任务 §二 10 项）

1. **Mesh 创建链**：LuCI 向导 → `ubus mesh_create` → `om_netconf_mesh_apply`（wireless libuci 加 802.11s iface；hybrid 走 `hybrid-netconf.sh`）→ 30s 事务确认/回滚。
2. **BATMAN softif**：`om_bat`（UCI network.om_bat，proto batadv，脚本创建）。
3. **无线 hardif**：`om_mesh`（proto batadv_hardif master=om_bat）；wifi-iface 经 `network=om_mesh` 挂载。
4. **br-lan 成员**：UCI device 段 `name='br-lan'` 的 `ports` 列表（hybrid 后含 om_bat）——m19 的 `om_store_lan_ports` 同源。
5. **管理网络**：LuCI/SSH 全部经 br-lan（AX3000T 192.168.40.x；x86 192.168.31.x/eth0）。
6. **WAN**：UCI wan 接口引用的网口（AX3000T wan；x86 eth1 dhcp 空闲）。
7. **DSA 用户端口**：AX3000T lan1-3（sysfs `phys_port_name` 存在即 DSA 用户口）。
8. **虚拟接口**：br-*（bridge 目录）、phy*-ap0/mesh0（phy80211）、om_*、bat*（mesh 目录）、eth0.N（VLAN 子接口）、lo。
9. **上层依赖**：现网无 VLAN/bond/PPPoE 在用；发现层仍防御识别（vlan 段/子接口名/pppoe proto）。
10. **回滚**：netconf 快照（wireless+network 双文件）+ 30s 定时回滚 + 快照感知 purge——m19 已含 om_eth；本阶段零写入，回滚机制不受影响。

## 开工前 10 问（摘要）

1. **解决**：让用户能在 LuCI 看到真实物理网口并表达"未来用哪个口做 Ethernet Mesh 回程"的意图——为 7.2 的真实切换提供发现/预检/预览底座。
2. **原生**：无现成"端口择优"API；数据源用 sysfs（/sys/class/net）+ UCI network + libuci，全部只读。
3. **复用**：m19 的 br-lan 解析思路、hybrid-netconf.sh 的配置语义（预览文本与之对齐）。
4. **自研**：纯分类器 `ports_classify.c`（零依赖可宿主测试）+ 发现层 `ports.c`（sysfs/UCI→facts）+ ubus `ports`/`port_plan` + 向导预览区。
5. **交互**：向导加载双取数（lan_ports + ports）；选择/模式/开关任一变化→`port_plan` 重查（后端即时重新发现，前端结论只是提示不是许可）。
6. **存储**：零写入（含 UCI openmesh）——本阶段意图不落盘，7.2 再定持久化。
7. **ubus**：`ports`（无参）→ `{ports:[{device,label,type,carrier,speed_mbps?,bridge,role,management,eligible,reason?}]}`；`port_plan {device,mode}` → 10 项预检查 + 预览（uci_changes/ops/interruptions/mgmt_risk/rollback）+ `preview_only:true`。ACL read 追加两方法。
8. **恢复**：无任何状态变更，天然无需恢复；预览明确展示 7.2 将继承的事务回滚策略。
9. **测试**：`test_ports_classify.c` 37 项（分类器全场景）+ `test_ports_ui.js` 15 项（下拉/标签/ubus 异常/恶意输入/后端重复校验）+ 既有全套回归。
10. **兼容**：不假设端口名——DSA（phys_port_name 判定用户口/CPU 口）与 x86 独立网卡（无 DSA 则 ethX 即真实网卡）同一套规则；management/eligible 一律证据判定，不靠名字猜。

## 关键设计决定

- **分层**：`ports_classify.c` 纯函数（facts→verdict，宿主可测）∥ `ports.c` 发现层（sysfs+UCI 只读）∥ `ubus_api.c` 薄序列化。发现层不可宿主测试（libuci 依赖），已登记；分类逻辑 100% 被覆盖。
- **DSA CPU 口识别**：系统存在任意 `phys_port_name` 网口（DSA 用户口）时，无该属性的裸 ethX 即交换机 CPU 口——移出会断整个交换机，blocked（`dsa_cpu`）。x86 无 DSA 标记 → ethX 为独立网卡，正常参与。
- **管理路径证据链**（不靠名字）：br-lan 物理成员中**恰好一个**有 carrier → 它极可能是管理路径（`management:yes`，blocked）；多个有 carrier → 无法区分（`unknown`，blocked `mgmt_unknown`）；本口无 carrier → 必不是管理路径（`no`）。无 carrier ≠ 不可配置（将来可预配置）。
- **阻塞顺序**（首个命中生效）：虚拟/无线/已 enslaved → **用户保护口（UCI `openmesh.main.protected_ports` 列表，绝对拒绝，任何确认不可翻越——业主设备如 NAS 上联口靠此保护）** → DSA CPU → 已用于 Mesh → WAN/PPPoE → VLAN 依赖 → 管理路径 yes/unknown → **eligible**。保护口在**两处强制**：分类器（port_apply 路径）+ `hybrid-netconf.sh` 咽喉点（m19 mesh_create 路径，含 'all' 展开过滤）——所有通往有线腿的路径无死角。
- **后端重复校验**：`port_plan` 每次调用重新枚举 sysfs+UCI 再分类——前端下拉的 eligible 只是显示提示，7.2 的任何写操作必须（并将）重新走此校验（任务 §五红线，测试 P12 固化）。
- **预览非命令**：uci_changes/ops 全部为描述性文本（与 hybrid-netconf.sh 语义对齐），任何代码路径都不会把它们交给 shell。
- **安全边界（§七）**：ports.c 零 system()、零写文件、零 reload——只有 opendir/stat/fopen(读)/libuci load。向导区只渲染。本阶段不持久化意图。

## 测试与验收

- 宿主：test_ports_classify 37 PASS（§八 1-13/15/16 分类器侧）；test_ports_ui 15 PASS（14/17/18 前端侧）；全套回归 identity/session/batman/topology/lanports/hybrid/discovery + 前端 5 套全绿。
- SDK：mipsel_24kc 编译零警告，ipk `openmeshd_0.0.1-r20`。
- 真机：**NOT TESTED**（两台设备均不可达）。待验证清单（§九）随 r20 ipk 部署执行：AX3000T/x86 各自 `ubus call openmesh ports` 对照 `ls /sys/class/net` + `uci show network`；确认 `/etc/config/network` md5 前后一致、802.11s/BATMAN/LuCI 会话不受影响。

## Phase 7.2 实施建议

1. **写路径**：`port_plan` 通过后才允许新的 `port_apply`（ubus，write ACL）；内部复跑 `om_ports_discover` 校验（绝不信任前端），再走既有 30s 事务（hybrid-netconf.sh 已支持 om_eth_* 生成，7.2 只需把 plan→lan_ports 参数接上并先 `om_port_plan` 门禁）。
2. **意图持久化**：`openmesh.backhaul` 增 `wired_mode`（wireless_only/wired_only/wired_preferred）与 `wired_port`；wired_* 模式的实际选路仍交 batman metric（ADR-0002 纪律），"仅有线"= 不建无线 mesh 腿，非自研切换。
3. **管理路径保护前置确认**：mgmt_unknown 的口要求用户在 LuCI 弹窗二次确认（显示哪些口有 carrier），或提供"从 WiFi 管理会话执行"的建议提示。
4. **对端同步**：port_apply 后经会话通道推送（m19 通道现成）；对端口名不同设备（lan1↔eth0）按 7.1 发现的 facts 在对端重新择口而非盲推名字。
5. **自动识别（Phase 6.4 既定路线）**：discovery recvmsg IP_PKTINFO 取 ingress 口，检测对端 mesh 设备后与 ports API 的 carrier/eligible 联动给出"一键启用"建议。
6. **验收顺序**：先 x86（网口少、风险低）单机 dry-run → 双机真线缆 → AX3000T。72h 稳测窗口内观察 carrier 抖动事件。
