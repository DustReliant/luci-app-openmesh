# M19 有线回程 LAN 口选择（Phase 6.3）

状态：已实现（2026-11-02；宿主/命令层/前端测试全绿 + SDK 编译零警告，真机 DoD 待演练，见「测试计划」）

## 开工前 10 问

1. **解决**：hybrid 模式的有线腿从未真正落地——现状只把 WiFi mesh 口做成 batman 硬接口（`om_mesh`），LAN 口全留在 br-lan；此时两节点 LAN 口直连网线会与 bat0 桥接形成二层环路（ADR-0002 明示）。用户需要：建网时**指定哪些 LAN 口做 mesh 回程**（下拉选择"全部/指定口"），其余口留给电脑等终端，不被夺走。
2. **原生**：netifd `batadv_hardif` proto 即全部所需（M9 已用同款挂 WiFi 口）；br-lan 成员由 UCI device 段 `list ports` 管理。链路探测/选路**继续完全交给 batman 原生 metric**（ADR-0002 纪律不破）。
3. **复用**：`apply_network_hybrid()` 的 $BR（br-lan device 段）发现逻辑；r21b 命令层测试的 fake-binary（PATH 注入）模式，此次伪造 `uci` 命令做同款测试。
4. **自研**：仅编排策略（选口 → del_list 出 br-lan → 建 `om_eth_<port>` hardif 段）与 30s 事务兜底；不自研任何探测。
5. **交互**：向导新增「有线回程 LAN 口」下拉（仅 hybrid 模式启用，选项：不使用有线 / 全部 LAN 口 / 各物理口）→ `mesh_create` 新增可选 `lan_ports` 参数 → netconf 事务（network 快照已含）→ 会话 CONFIG JSON 推送 → 代理端同参数应用。
6. **存储**：`openmesh.backhaul` 段新增 `lan_ports` 选项（重启后向导可回显、join 复用）；network 侧 `om_eth_<port>` 段带 `om_managed='1'`；事务回滚靠既有 network 快照整体复原。
7. **ubus**：`mesh_create` 增可选 `lan_ports`（`""`=不用有线，默认；`"all"`=全部 LAN 口；`"lan1,lan2"`=逗号列表）；新增只读 `get_lan_ports` → `{ports:[...]}`（读 br-lan device 段现有 ports，滤除 om_* 虚拟口）——端口名全部动态获取，禁止硬编码（workflow §7）。
8. **恢复**：30s 不确认自动回滚 network 快照（被移出的口随文件复原）；r11 防御 purge 与 r20 快照感知均扩展识别 `om_eth`；重复应用先"re-home"（旧 `om_eth_*` 的 device 加回 br-lan、删旧段）再落新选择，保证任意两次配置间迁移安全；脚本缺失/失败 → 事务失败回滚，无线腿不受影响。
9. **测试**：宿主 `test_lanports.c`（纯校验函数）；`test_hybrid_lanports.sh`（fake uci + 假 /sys 执行**真实生产脚本**：无参/指定口/all/迁移/幂等/坏口跳过/注入防御）；`test_wizard_lanports.js`（下拉逻辑）；真机 DoD：AX3000T 选 lan1 建 hybrid → `batctl meshif om_bat interface add` 侧可见 eth hardif、lan1 脱离 br-lan、lan2/3 终端照常上网、30s 回滚后三口俱全。
10. **兼容**：DSA（lan1..3）与 x86（eth0/eth1）命名差异——控制器下发的指定口名在代理端不存在时**跳过该口不报错**（无线腿照常），日志明示；纯 wired（模式 C）不在本片范围。

## 关键设计

- **选口语义**：`""`=现状（无 eth hardif，完全后向兼容，老调用零变化）；`all`=br-lan 当前全部物理口；指定列表=仅这些口。`wan` 口不在 br-lan ports 中，天然不参与。
- **注入防御**（shell 经 system() 执行，这是红线）：端口名白名单 `[A-Za-z0-9_]`、段长 ≤15、总长 ≤63、空段拒绝——校验放纯函数 `om_lan_ports_valid()`（lanports.c，零依赖可宿主测试），ubus 入口与代理 CONFIG 入口双重把关；值经 `setenv("OM_LAN_PORTS")` 传递，**绝不拼进命令字符串**；脚本内全程引号 + `/sys/class/net/$P` 存在性二次过滤（`OM_NET_SYS` 可覆盖，测试钩子）。
- **shell 逻辑外置**：hybrid 网络编排从 C 字符串字面量迁移到独立脚本 `files/usr/share/openmesh/hybrid-netconf.sh`（ipk 随包安装）——6.3 的 shell 复杂度（re-home/选择/幂等）已超出 C 字符串可维护范围，且外置后测试可执行**逐字节相同的生产脚本**。netconf 只负责 setenv + `system("sh …")` + rc 检查 + 提交后自检（om_bat.proto==batadv）。
- **段落编号**：`om_eth_<port>`（如 `om_eth_lan1`），段名字符集与端口名一致，uci 安全。
- **回滚链扩展**：`snapshot_has_om()` 与 `purge_om_sections()` 认得 `om_eth`；快照含已提交 om 段时照旧跳过 purge（r20 语义原样延伸）。

## 数据面/接口变更

```
mesh_create {mesh_name, band, key, mode, lan_ports?}        ← 新增第 5 参
get_lan_ports {} → {ports: ["lan1","lan2","lan3"]}
om_netconf_mesh_apply(…, mode, lan_ports)                    ← 签名扩展
om_snet_push_mesh(…, mode, lan_ports)                        ← CONFIG JSON 增 "lan_ports"
om_store_backhaul_set/get(…, lan_ports)                      ← UCI backhaul 段持久化
```

## 测试计划

宿主：`tests/test_lanports.c`（白名单/空段/超长/`all`/遍历回调）+ `tests/test_hybrid_lanports.sh`（fake-uci 状态机执行真实脚本，8 场景）+ `tests/test_wizard_lanports.js`（下拉选项构建/过滤/gating）。
真机 DoD（进 v0.2-plan 回填）：指定 lan1 双机 hybrid → 网线 lan1↔eth0 → `batctl o` 显示 eth 硬接口链路（TQ 有线应高于无线）→ lan2 接电脑正常上网 → 30s 回滚三口俱全 → 重下 lan2 再建迁移正确。

## 后期计划：LAN 口自动识别（列入开发，本片不做）

Phase 6.4+ 候选「自动有线回程」：
1. **识别**：discovery 的 UDP announce 接收侧改用 `recvmsg(IP_PKTINFO)` 取 ingress 接口；若来自**已领养节点**的签名 announce 从 br-lan 成员口进入（而非 bat0/om_mesh），即判定"对端 mesh 设备在线缆对端"。（备选信号：该口出现 batman OGM。）
2. **动作**：默认仅**提示**（LuCI 横幅"lan1 检测到 mesh 对端，一键启用有线回程"）；用户开启「自动模式」（openmesh UCI 开关）后才自动走 30s 事务把该口转 `om_eth_<port>`，失败自动回滚。
3. **回退**：口链路消失（carrier down）只告警不自动还原（避免插拔抖动反复事务），由用户或后续策略处理。
4. **前置依赖**：本片的 `om_eth_*` 段结构 + re-home 机制即自动模式的执行底座。

## 已知边界（本片如实登记）

- 控制器与代理端口名空间不同（lan1 vs eth0）：指定口名在代理端不存在则跳过，有线腿仅控制器侧生效，需代理侧后续按节点配置（V0.3 每节点覆盖）。
- `mesh_join` 仍按 wifi 模式应用（既有行为，未动）；lan_ports 已随 backhaul 持久化，join 复用留给后续。
- 会话 CONFIG JSON 对 mesh_id/key 未做 JSON 转义（既有问题，字符集受限未爆雷，登记不动）。
- 纯有线模式 C（ADR-0002）仍不在范围。
