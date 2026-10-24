# OpenMesh Manager V0.3 源码研读报告：netifd「剥除」之谜 · usteer 集成设计 · k/v/r 选项×版本矩阵

> **修订版 R2**（2026-10-25）：按独立审读意见清单逐条修订——修复 5 处 P0 内部矛盾、补齐 P1 决策信息缺口（含拼装「向导按钮→30 秒 BSS 不可用窗口」推理链、新增 seed-config 与 .config 变体背离等修订轮新发现）、P2 可追溯性与 P3 编辑性问题；修订说明与逐条对应关系见文末「修订记录」。

- **日期**：2026-10-25（初稿）/ 同日修订 R2
- **基线**：本仓库 `openwrt-24.10` 分支工作区；目标平台 aarch64_cortex-a53_musl（AX3000T）；netifd 版本 pin 为 2025-05-23/7901e66c（`package/network/config/netifd/Makefile:8-9`，实测）
- **方法**：三路源码研读（netifd 剥除审计 / usteer 集成调研 / k/v/r 选项×版本矩阵）各自经独立复核（材料中 `status=verified` 项均有复核人逐条重验记录）；报告撰写与修订两轮对承重结论的锚点做了**抽样复核**（初稿约 20 项、修订轮又新增约 20 项，清单见附录 B；未逐一重验的材料复核项以材料复核记录为准）。个别材料行号与实测有 1~30 行漂移处，正文一律以实测行号为准。
- **引用约定**：
  - `netifd 源码` = `build_dir/target-aarch64_cortex-a53_musl/netifd-2025.05.23~7901e66c/`；`rpcd 源码` = `build_dir/target-aarch64_cortex-a53_musl/rpcd-2025.09.01~bba95191/`；`uci 源码` = `build_dir/target-aarch64_cortex-a53_musl/uci-2025.01.20~16ff0bad/`（三者均已解包存在，实测）。
  - `usteer 源码` = `/tmp/usteer-src/usteer-7d2b17c91baf67419c0ce63dc6c65a7659ab6a5c/`——feed 构建时从 git.openwrt.org 拉取（`feeds/packages/net/usteer/Makefile:11`）；研读快照因 git.openwrt.org 直连 TLS 失败改从 **GitHub 官方镜像同 commit** 下载，并经与 sources.openwrt.org 官方 tarball diff 验证内容一致（材料复核记录）。
  - 未带前缀的相对路径均为本仓库工作区路径；✦ 标记 = 报告撰写/修订期亲自实测。
  - `.config` 为生成文件、行号易漂移，一律以 `CONFIG_*` 键名为主、行号为辅。
- **材料可追溯性**：三路研读材料及其逐条复核记录由工作流以任务输入形式提供（运行记录 `.zcode/workflow-runs/dwfrun-de0797b3-8e1b-4edc-8ea2-628ebc5eb798.mjs`；工作流定义 `.zcode/workflow-drafts/V0.3-前置研究源码三路研读.dwf.ts`），**未单独落盘为文件**——本报告引用其结论与证据行号；如需归档建议将材料汇入 `docs/research/`。
- **状态标注**：正文事实默认来自「已复核」材料或实测；凡材料标注 `unconfirmed` 的结论一律标 **『未确认』**；两处经复核发现原表述有硬伤的（usteer UCI 选项全集、hostapd 变体前提），正文按**复核修正后**的口径给出；修订轮源码裁决的 9 个审读 openQuestions 已逐项落定并移入正文（见 §4.1 开头），勘误细节见附录 A。

---

## 0. 执行摘要

1. **「netifd reload 重写 /etc/config/wireless」在源码层面证伪**：netifd、wifi-scripts、hostapd.uc/wpa_supplicant.uc 在 reload 链路上对该文件零写点；真正会整文件还原它的是 **openmeshd 自身的漫游事务回滚**（以及 rpcd 的 uci apply 回滚）——30 秒无确认即自动回滚，且回滚末尾自己再发一次 `ubus call network reload`，恰好制造「我一 reload 选项就消失」的表象（§1）。
2. **usteer 可以集成，且与 openmeshd 天然正交**（静态能力位 vs 运行时转向），但有一个实测确认的硬前提：**当前 .config 选中的 wpad-basic-mbedtls 不含 WNM，`bss_transition_request` ubus 方法缺失**——usteer 的 802.11v 引导全部不可用（11k 邻居同步/测量仍可用，见 §2.8 按位 no-op 分析）。**且项目自己的构建种子 `seed-config-ax3000t-24.10:47-48` 已明文规定 `CONFIG_PACKAGE_wpad-mesh-mbedtls=y`（即 WNM 开启的 full 变体），当前工作区 .config 与种子背离**——统一到种子即可自动满足该前提（§2.8）。
3. **k/v/r 选项矩阵 23.05 与 24.10 几乎完全一致**：k/v/r 声明段逐字节一致；生成核心段 diff 仅 1 处（24.10 新增 FT 无密钥报错）；stationary_ap 在 24.10 升级为设备级默认开；声明段全集 24.10 另多 4 项非 k/v/r 选项。变体门槛：11r 需 basic 及以上、11v WNM 仅 full、11k 无门控、mbo/rnr 不可用（§3）。与摘要 2 联动：**当前 basic 构建上向导漫游按钮写出的 `bss_transition='1'` 行无法被 hostapd 消化，可能造成最长 ~30 秒的 BSS 不可用窗口后被回滚「自愈」（源码推论，§1.7 推理链）**。

---

## ① netifd「剥除」之谜：答案与规避方案

### 1.1 裁决

openmeshd 源码注释（`package/openmeshd/src/netconf.c:279-285`，实测原文）称：

> "Bypasses uci set/commit because **the netifd wireless reload pipeline rewrites /etc/config/wireless** and was observed leaving some options only in the volatile uci delta (never in the file). The transaction snapshot/rollback covers us."

三路逐行审计 + 独立复核 + 两轮抽查证明该归因**不成立**：netifd 对 `/etc/config/wireless` 是纯读者，reload 全链路没有任何 uci 落盘写操作。真凶是 openmeshd 自身的 30 秒事务自动回滚（外加 rpcd uci apply 回滚与「delta 幻象」两个次要机制）。当年观察到的「选项消失」，静态读码能给出的最强解释是自身（或 LuCI/rpcd）回滚窗口所致——机制充分且是系统内仅存的写者；但因无法还原当年运行态（原始现象记录缺失，见 §1.3），此归因属「机制充分、最可能的解释」而非字面证明（上机裁决见 §4.2 A 组）。

### 1.2 证据链：reload 管线对 /etc/config/wireless 零写点（逐组件洗清）

| 组件 | 结论 | 关键证据（✦ = 实测） |
|---|---|---|
| netifd C 源码（2025.05.23 与 2024-01-04 两版） | 纯读者；唯一 uci 写点仅 bridge 内存态修补，不落盘、不涉 wireless | ✦ netifd 源码树全部 *.c/*.h 中 `uci_save/uci_commit/uci_add(` grep 零匹配；✦ 唯一 `uci_set` 在 `netifd 源码/config.c:97`（config_fixup_bridge_var，只设 bridge 的 `__has_vlans/vlan_filtering`）；✦ 唯一 `uci_rename` 在 `config.c:122`（ifname→ports，bridge 段）——材料原证据称「uci_rename 零匹配」有误，复核已更正：实有一处，但无 save/commit 不落盘且从不触及 wireless 段。reload 链路：`ubus.c:44-52` netifd_handle_reload → netifd_reload() → config_init_all() → `config.c:885` 只读加载 wireless 包 → `config.c:730-782` config_init_wireless()（uci_foreach_element/uci_lookup_option 只读）→ wireless_start_pending |
| netifd→脚本链路 | 只传 JSON、只回传内存态 | `netifd 源码/wireless.c:463-516` wireless_device_run_handler 以 argv[4]=blobmsg_format_json(config) 执行脚本；脚本回传唯一通道 `ubus call network.wireless notify`（`wifi-scripts/files/lib/netifd/netifd-wireless.sh:128`），CMD_SET_DATA/CMD_PROCESS_ADD 仅更新 netifd 内存 |
| hostapd.uc / wpa_supplicant.uc | 完全无罪 | ✦ `grep -cE "uci\|/etc/config"` 两文件均 0 匹配；config_set（`package/network/services/hostapd/files/hostapd.uc:917-953`）只读 netifd 传入的 /var/run conf 文件；✦ 调用点 `wifi-scripts/files/lib/netifd/wireless/mac80211.sh:995`（`ubus_call hostapd config_set`）；三张字段白名单（hostapd.uc:10-49）只用于运行态 hash 对比与 reload 判定 |
| wifi-scripts（mac80211.sh/hostapd.sh/netifd-wireless.sh） | 不写 /etc/config/wireless | ✦ mac80211.sh 全文唯一真实文件写是 `:562` `cat >> /var/run/hostapd-$phy$vif_phy_suffix.conf`；hostapd.sh 的 `config_add_*` 只是向 netifd dump **声明**选项表（选项在表内，不存在白名单漏配）；`/sbin/wifi` 的 reload 只发 `ubus call network reload` |
| hostapd 包其余脚本 | wpad.init 仅 procd 定义、wpad.json 仅 ujail capabilities | wps-hotplug.sh 只有 WPS 按键事件才 `ubus -S call uci set/commit` wireless（✦ :25/:30-31，只写 encryption/ssid/key）；wifi config 只为未配置 radio 增段；均不在 reload 链路上 **『未确认』**（非决策级，未安排复核） |

**系统内会写 /etc/config/wireless 的组件清单及检索方法**（承重前提，修订轮重做枚举）：

- 检索方法：三路研读以多组模式检索工作区——CLI 写（`uci (set|add|commit|batch) … wireless`）、ubus 写（`ubus call uci set/commit` 且 config=wireless）、直接文件写（重定向/fopen/copy 至该路径）——覆盖 package/、feeds/、scripts/、target/ 的脚本与 C 源码；修订轮逐目录补跑验证（✦ ugrep -r 按单目录可靠；本机 ugrep 7.8.4 的全树 -r 与 find -exec 组合会漏报——同模式直接命中 `package/openmeshd/src/netconf.c` 而全树递归漏之——故弃用，见附录 B 工具注记）。
- 写者清单（构建树内）：**openmeshd**（roam_rewrite_file/mesh_apply/recover 直接文件写 + purge 的 uci CLI，netconf.c）；**rpcd**（uci apply 回滚快照拷回，C 源码）；**WPS 按键**（wps-hotplug.sh:25,30-31）；**rfkill 按键**（`package/base-files/files/etc/rc.button/rfkill:16,29`——✦ 修订轮新发现，材料清单遗漏：按 rfkill 键/开关翻转 wifi-device 的 `disabled` 并 `uci commit wireless`，不涉五项但确属写者）；**phy 热插拔**（10-wifi-detect → mac80211.uc 首启增段 ✦ `print("commit wireless")` :118；及各平台迁移脚本如 AX3000T 所在 filogic 的 `target/linux/mediatek/filogic/base-files/etc/hotplug.d/ieee80211/05-wifi-migrate`——✦ 修订轮新发现，仅迁移 radio `path`）；**LuCI 标准无线页**（feeds/luci wireless.js，手动保存，rpcd uci 通道）。feeds 命中的 dcwapd/travelmate/wifischedule 等 ✦ 均未选入 .config（`# CONFIG_PACKAGE_... is not set`），不进固件。
- 范围限定：以上为**构建树视角**；设备运行期安装的第三方包与用户自定义 hotplug/cron 脚本不在覆盖范围。

### 1.3 真凶机制：openmeshd 漫游事务的 30 秒自动回滚

openmeshd 是**全系统唯一自动认识并改写这组五选项** {ieee80211k, bss_transition, ieee80211r, mobility_domain, ft_psk_generate_local} 的组件（✦ `ft_psk_generate_local` 在工作区 package/、scripts/、target/、feeds/ 检索仅命中 openmeshd 与 wifi-scripts hostapd.sh，后者纯读方；✦ base-files `bin/config_generate` 五项 grep 零匹配；✦ `seed-config-ax3000t-24.10` 为 .config 种子片段、不含任何 UCI 内容；LuCI 标准无线编辑页手动保存是例外路径）。

**原始现象记录（材料口径，修订轮补记）**：黑盒测试轮次中观察到的形态为——`bss_transition` 与 `ieee80211r` **两行整行消失**，`ieee80211k`、`mobility_domain`、`ft_psk_generate_local` **三项存活**（netifd 方向材料第 5 条已复核结论）。**原始测试日志、触发操作序列与当时固件版本未随材料提供**（§4.1-1/4.1-5），读者对照自己设备现象时请注意此缺口。

机制链（两轮逐段实测 `package/openmeshd/src/netconf.c`）：

1. **改写**：`roam_rewrite_file()`（:286-352）对 mode='ap' 的 wifi-iface 段（:400-406 筛选，nsecs<8 上限 :407-408）内 ours[] 五项 skip+continue 删除（数组 :324-327，判定 :331-336），块尾 `roam_emit_options()`（:267-277）重发。**写值语义（修订轮明确）**：:270-272 三行写的是**入参**的 0/1（`k ? 1 : 0`）——经 ubus 参数可只开部分能力（如 k=1/v=0/r=0）；`mobility_domain`/`ft_psk_generate_local` 仅 r=1 时写，且 ft_psk_generate_local 为硬编码 '1'（:275）。向导按钮因默认 k=v=r=true 而全开。
2. **快照先行**：`om_netconf_roaming_apply()`（:354-439）在改写**之前** :370-377 先 `copy_file /etc/config/wireless → /etc/openmesh/wireless.om-bak` 整文件快照，:379-385 写 txn.pending 标记。
3. **apply 即 reload**：:422 `reload_network()`（netconf.c:57-61：`ubus call network reload`，失败 fallback `/sbin/wifi up`）。
4. **30 秒定时**：:426-427 `uloop_timeout_set(OM_TXN_TIMEOUT*1000)`；✦ `netconf.h:12` `#define OM_TXN_TIMEOUT 30`。
5. **自动回滚**：超时 → txn_timer_cb（:63-69，:68 调 rollback）→ `om_netconf_rollback()`（:472-498）：copy_file **整文件还原**快照（:480-487）→ **清理快照与标记**（✦ :486 unlink bak、:489 unlink marker——修订轮补记，回应「回滚后是否残留标记」：不残留，不会出现『一次超时回滚后永久拒绝新事务』）→ purge_om_sections（:462-470，uci CLI commit wireless）→ **:492 再发一次 `ubus call network reload`**——「我一 reload 选项就没了」的表象由此而来。
6. **唯一取消路径**：`apply_confirm` ubus 方法 → `om_netconf_confirm()`（:441-455）取消定时器并删快照+标记（✦ :445-448）。
7. **开机恢复**（✦ 修订轮补读 :500-538）：`om_netconf_recover()` 仅在 txn.pending 存在时（:507-509）还原 wireless（marker 记 files=2 时连 network）→ 删快照（:526/:532）与标记（:535）→ reload（:537）。**标记生命周期结论**：正常 confirm/rollback/开机恢复均清理 txn.pending 与 *.om-bak；两者存在 = 事务正在 30 秒窗口内，或掉电后 openmeshd 尚未执行恢复。

**形态解释（附前提限定，修订轮改写）**：roam_emit_options 任何 k/v/r 组合都会写出前三行（:270-272，值为入参 0/1），不可能输出「bss_transition/ieee80211r 行不存在」。因此——**若现象是「文件中曾出现这两行、之后消失」**：仓库内唯一能产生整行消失的机制是回滚的整文件快照还原；三项存活 = 在事务前快照里，两项消失 = 快照拍摄于其写入之前。**若现象实为「配置生效过但文件里从来没有」**：§1.5 的 delta-only 路径同样成立。两个候选机制无法静态二选一，正是 §4.1-1 需上机裁决的原因；快照里三项的写入来源取决于设备操作历史（源码不可定，§4.1-2）。

**向导按钮是「定时炸弹」**（实测 `package/luci-app-openmesh/htdocs/luci-static/resources/view/openmesh/wizard.js`）：

- :10 `callRoaming` 声明无 params；:81 `callRoaming()` 零实参调用；:81-88 then 回调只写状态行文案 "confirm with Keep"，**无任何 confirm 调用**；
- 前端唯一 confirm 路径是 keepBtn 手点（:52-63）；倒计时组件 setCountdown（:18-37）只有 createBtn 用（:48）——**漫游点击后 UI 无任何倒计时提示**；
- ubus 侧默认值（实测 `package/openmeshd/src/ubus_api.c:402-424`）：:408 `bool k = true, v = true, r = true;`；:421-424 无 domain 或非 4 字符时**随机**生成 4 位 hex mobility_domain——即黑盒测试点一下按钮 = k/v/r 全开 + **每次随机 FT 域** + 30 秒后整文件回滚。多机场景下各节点随机域不同会直接使 11r 失效（§4.2 C2）。

**mesh 组网同样有 30 秒事务**（✦ 修订轮补读，回应「mesh_create/mesh_join 是否也进事务」）：`om_netconf_mesh_apply()`（netconf.c:158-264）先快照 wireless（hybrid 模式加 network，:183-201）、写 marker（:203-210）、改写后 :249-250 装同一 OM_TXN_TIMEOUT 定时器；netconf.h:35-41 注释明示 "Without om_netconf_confirm() within OM_TXN_TIMEOUT seconds everything is restored"。向导 create 按钮有倒计时提示但仍需手动 Keep；agent 侧例外见下。

**两条限定**（不推翻机制结论）：

- agent 角色节点与控制器会话存活时，收到任一加密帧（含控制器每 15s 的 KA）即自动 confirm（session_net.c:468-470）——「必回滚」仅对无活动控制器会话的独立节点（典型黑盒测试场景）成立；而 roaming RPC 仅 controller 角色注册（✦ openmeshd.c:259-268：`if (sc.is_controller) { om_ubus_init(...) }`，注释 "ubus RPC belongs to the controller role only"），controller 上无人 confirm 必然回滚。
- openmeshd **不会被动地被 network reload 触发**：package/openmeshd/src/ 全部 .c/.h 无 ubus_register_event_handler（不订阅任何 ubus 事件），monitor/tick 只读轮询（popen iw/iwinfo/batctl）。对 /etc/config/wireless|network 的改写入口共 **7 个触发点、归 4 类**（✦ 修订轮逐调用点核验，更正初稿「五条路径」计数错误）：① ubus RPC 四方法——mesh_create/mesh_join → mesh_apply（ubus_api.c:269/:296）、roaming_apply（:427）、apply_rollback（:328）；② 远程 OM_SES_CONFIG → mesh_apply（session_net.c:541-565，✦ 载荷仅 mesh_id/band/key/mode 四字段，**不含漫游配置**）；③ 30 秒超时 txn_timer_cb（netconf.c:63-69）；④ 开机恢复 om_netconf_recover（openmeshd.c:269 → netconf.c:500-538）。方向恰相反：openmeshd 主动调 `ubus call network reload`（netconf.c:59）。
- openmeshd 默认自启：`package/openmeshd/files/etc/init.d/openmeshd:5` ✦ START=99，:14 ✦ `config_get_bool enabled main enabled 1` 默认开。

### 1.4 次要机制一：rpcd/LuCI 的 uci apply 回滚（rpcd 裸默认 60s，LuCI ≥90s）

- ✦ `rpcd 源码/include/rpcd/uci.h:33-38`：`RPC_UCI_DIR_PREFIX "/var/run/rpcd"`、`RPC_SNAPSHOT_FILES "/var/run/rpcd/snapshot-files/"`、`RPC_UCI_DIR "/etc/config/"`、`RPC_APPLY_TIMEOUT 60`。
- LuCI 保存流程的 `ubus call uci apply{rollback:true}` 会把涉及配置快照到 snapshot-files/，超时无 confirm 则 `rpc_uci_do_rollback`（`rpcd 源码/uci.c:1508-1548`）把快照**整文件拷回** /etc/config/ 并 commit+trigger——窗口期内其他写入者对同一文件的修改会被旧内容覆盖。
- 两个精确化（✦ 修订轮补锚点）：快照范围 = 该会话有待提交更改的配置（`rpcd 源码/uci.c:1604-1633`，非 /etc/config 全部）；LuCI Save&Apply（checked 路径）传 `timeout = max(+(config.apply.rollback ?? 90), 90)` ≥ 90s（✦ `feeds/luci/modules/luci-base/ucode/controller/admin/uci.uc:47-53`），LuCI JS API 的 `apply()` 默认 timeout=10 且成功后 1s 起 250ms 轮询自动 confirm（✦ `feeds/luci/modules/luci-base/htdocs/luci-static/resources/uci.js:975-1000`）——「60 秒」仅为 rpcd 裸默认值。

### 1.5 次要机制二：「delta 幻象」（解释 netconf.c:283-284 注释的后半句）

- ✦ `uci 源码/uci.h:40`：`#define UCI_SAVEDIR "/tmp/.uci"`。未 commit 的 `uci set` 只写 delta 文件（实际路径 `/tmp/.uci/wireless`，**无** `.delta` 后缀——复核更正）。
- 生产 netifd 用默认 savedir（uci_set_savedir 仅 #ifdef DUMMY_MODE，非 Linux 构建不生效），加载时会 `uci_load_delta` 应用未 commit 改动——所以 **reload 后 wifi 生效 ≠ 选项落盘**；`uci commit` 才重放整个 delta 并整体重写文件。
- 任何写入者走 `ubus uci set` 不 commit，都呈现「生效但文件里永远没有」的幻象。

### 1.6 规避方案

**操作面（时序/验证，已复核）**：

1. 动 wireless 前查未决事务：`ls /etc/openmesh/`（txn.pending、wireless.om-bak 存在 = 事务进行中或掉电遗留未恢复，见 §1.3-7）；有则先 `ubus call openmesh apply_confirm`（或 `apply_rollback` 立即了结）再改。未决事务期间新事务会被拒绝（netconf.c:365-366），且回滚/开机恢复会覆盖手改内容。
2. 走 openmeshd 通道写漫游项：`roaming_apply` 后 **30 秒内** `ubus call openmesh apply_confirm`；多机场景务必带显式 domain 参数（见 §4.2 C2）。
3. LuCI 保存后拆 rpcd 回滚窗：`ubus call uci confirm '{"session":"<sid>"}'`——复核更正：**不能裸调**，rpc_uci_confirm 强制要求 session 参数且须等于发起 apply 的 apply_sid（`rpcd 源码/uci.c:1664-1673`）。**sid 获取**（修订轮补）：`ubus call session list` 枚举会话定位（✦ rpcd 源码 session.c:1351-1353 注册 list 方法）；LuCI 自身流程会自动 confirm（uci.js:975-1000），手工 confirm 仅 CLI 复现时需要。
4. 验证真相用两条命令区分：`uci changes wireless`（有输出 = 改动只在 delta）；`cat /etc/config/wireless`（文件真相）。

**代码面（治本）『未确认』**（未安排复核，改动前需按 §4.2 验证）：roam_rewrite_file 改「合并保留用户已有值、仅补缺」（netconf.c:331-336 一带）；roaming_apply 自动确认或调大 OM_TXN_TIMEOUT（netconf.h:12）；更正 netconf.c:279-285 的错误注释。顺带发现的匿名段 bug：roam_rewrite_file 的 sscanf（:309-310）匹配不上无名 `config wifi-iface` 段头，会静默漏管——实测确认两个 pattern 均要求段有名字；**影响面评估（修订轮落定）**：首启生成的 wifi-iface 段均有名（✦ mac80211.uc:46 `let si = "wireless.default_" + name`），正常配置不受影响，仅手工创建的匿名段会被漏管（低风险，『未确认』项保留治本建议）。

### 1.7 能力面事实（实测确认）+ 向导按钮后果推理链

**当前工作区构建为 wpad-basic-mbedtls**（✦ .config 键 `CONFIG_PACKAGE_wpad-basic-mbedtls=y`、`# CONFIG_PACKAGE_wpad-mesh-mbedtls is not set`；`CONFIG_PACKAGE_openmeshd=y`；`# CONFIG_PACKAGE_usteer is not set`）：

- ✦ `package/network/services/hostapd/files/hostapd-basic.config:151` `CONFIG_IEEE80211R=y`、`:162` `#CONFIG_WNM=y`（注释禁用）——**11r 可用，11v（WNM）不可用**。
- ✦ 对三个 wpad 二进制运行 `strings -a | grep -x` 十个 hostapd ubus 方法名：9 个全部命中，**`bss_transition_request` 缺失**。三个二进制均为**本机构建树产物**（两棵源码树构建输出 `build_dir/.../hostapd-2024.09.15~5ace39b0/wpad`、`.../hostapd-2023-09-08-e5ccbfc6/wpad`，及 ipkg 打包暂存副本 `.../ipkg-aarch64_cortex-a53/wpad-basic-mbedtls/usr/sbin/wpad`）——**非设备取样**，对设备现状的代表性取决于设备固件与本地构建一致（§4.1-3）。

**推理链（源码推论，未实机验证——上机判据见 A5/C1；修订轮按审读意见显式拼装）**：当前 wpad-basic-mbedtls 固件上点一次向导漫游按钮 ⇒ roaming_apply 以默认 k=v=r=true 写入 `bss_transition='1'`（ubus_api.c:408；netconf.c:271 写入参值）⇒ reload 后 hostapd.sh 生成 `bss_transition=1` 行（`wifi-scripts/.../hostapd.sh:916`）⇒ 无 CONFIG_WNM_AP 的 hostapd 将其落入 unknown configuration item 分支（上游 config_file.c:5177-5181，return 1）⇒ **hostapd 拒绝该 BSS 配置、对应 AP 接口可能起不来** ⇒ 30 秒后事务回滚还原文件并再次 reload（netconf.c:480-492），BSS 自愈。即：换变体之前，向导漫游按钮可能造成**最长 ~30 秒的 BSS 不可用窗口**，表象是「网络自己恢复了、漫游选项却没了」——与黑盒观察高度吻合。hostapd 实际拒绝粒度（单 BSS/整 phy）待 C1 实测。

**seed-config 与 .config 的变体背离（修订轮新发现）**：✦ 项目构建种子 `seed-config-ax3000t-24.10:47-48` 明文规定 `CONFIG_PACKAGE_wpad-mesh-mbedtls=y`（注释："802.11s Mesh 回程需要 wpad-mesh 变体(替换默认 wpad-basic-mbedtls)"），而当前工作区 .config 实际选中 basic 且 mesh 未选——**工作区 .config 与项目种子背离**。若按种子构建，本节与 §2.8 的 WNM 前提自动满足；背离原因（.config 未按种子重生成？手动改动？）列入 §4.1-13。

---

## ② usteer 集成设计要点（UCI / ubus / 分工）

### 2.1 基本盘：版本、体积、依赖

- feed 内只有外链 Makefile（✦ `feeds/packages/net/usteer/Makefile`：:6-7 `PKG_SOURCE_DATE:=2022-08-18` / `PKG_SOURCE_VERSION:=7d2b17c91baf...`、:11 源码 URL git.openwrt.org、:25 DEPENDS、:33-40 仅装三产物）；研读快照取自 GitHub 官方镜像同 commit（见文首引用约定）。
- 产物仅三个：`/sbin/usteerd` + `/etc/init.d/usteer` + `/etc/config/usteer`；`PKG_BUILD_DEPENDS:=libpcap` 仅为 ap-monitor/fakeap 测试工具（usteerd 本体不链 pcap，不进镜像）。
- 体积实测（官方 22.03.5 aarch64 ipk：`https://downloads.openwrt.org/releases/22.03.5/packages/aarch64_cortex-a53/packages/usteer_2022-08-18-7d2b17c9-1_aarch64_cortex-a53.ipk`；✦ 修订轮完整解包 control 段核实 `Version: 2022-08-18-7d2b17c9-1`、`Architecture: aarch64_cortex-a53`、`Installed-Size: 33941`、`Depends: libc, libubox20220515, libubus20220601, libblobmsg-json20220515, libnl-tiny1`——与 feed Makefile:6-7 锁定值一致）：ipk 34,721 B；安装后 usteerd（stripped）70,907 B、/etc/config/usteer 4,998 B、init.d 3,003 B。
- 运行时依赖 libubox/libubus/libblobmsg-json/libnl-tiny（传递拉入 libjson-c）——✦ 验证方法：grep .config 六键全 =y（CONFIG_PACKAGE_libubox / libubus / libblobmsg-json / libjson-c / libnl-tiny / libgcc），全部已在 24.10 目标镜像，**无需新增镜像依赖**。
- 内存：源码结构估算空载 RSS <1.5 MB 量级（静态 2×64KB 收包缓冲 [✦ usteer 源码 usteer.h:41 APMGR_BUFLEN=64*1024；remote.c:360/423 两处 static buf] + 每节点 ~1KB + 每 STA 记录 ~300B）——**未实测**，上机校准见 §4.2 B5。

### 2.2 功能面

**三层 802.11k/v 能力**（对每个 hostapd.<ifname> 对象注册时；✦ 实测 `usteer 源码/local_node.c:858-867`——时序上 bss_mgmt_enable **先于** ubus_subscribe，复核更正了「订阅后调用」的先后表述）：先 `notify_response`，再 `bss_mgmt_enable` 一把打开 neighbor_report/link_measurement/beacon_report/bss_transition 四类管理帧；随后状态机（local_node.c:648-672）周期（local_sta_update 默认 1s，main.c:90）拉 **hostapd.<ifname> 的** get_clients/get_status（勿与 §2.3 usteer 对象同名方法混淆）、把本地+远端节点信息聚合经 `rrm_nr_set` 写回 hostapd 邻居报告表，`rrm_nr_get_own` 取自身 NR 用于 AP 间交换。

**漫游决策状态机**（policy.c:321-449 + ubus.c:732-751）：信号低于 roam_scan_snr（或回退 roam_trigger_snr，二者均 0 则漫游检查整体关闭，policy.c:426-431）→ SCAN 态发 802.11k beacon 测量请求（rrm_beacon_req，op_class 1/12 交替扫 2.4/5G）；满 roam_scan_tries 且 roam_scan_timeout=0 → SCAN_DONE 准备踢；找到更优候选（signal_diff_threshold 门限）→ 发 **802.11v bss_transition_request** 并设 kick_time=now+roam_kick_delay；客户端拒绝（response status≠0）则取消 kick 并在 steer_reject_timeout 内不再打扰。

**三类 kick**（均经 hostapd `del_client` deauth=1，reason=load_kick_reason_code 默认 5）：① min_snr 弱信号踢（连续低阈值超 min_snr_kick_delay，默认 5000ms）；② load_kick 负载踢（信道负载 EWMA 持续超阈值，**默认关闭** ✦ main.c:118 load_kick_enabled=false）；③ kick_time 到期强制踢（漫游引导未按期完成的兜底）。

**band steering**（决策逻辑默认开启）：2.4G 接口每 band_steering_interval（默认 120s）对信号持续 ≥ band_steering_min_snr（默认 -60 dBm）且支持 11v 的客户端发 bss_transition_request，邻居表只列本机 5G 同 SSID 接口。**注意（修订轮加限定，消除与 §2.8 的口径冲突）**：其引导同样走 bss_transition_request——**当前 basic 构建上该方法缺失，band steering 实际不可用**（§2.8）；「默认开启」仅指决策逻辑默认运行。probe/auth/assoc 准入控制默认均不拦。

**AP 间协同不走 ubus 而走私有 UDP**：✦ usteer 源码 usteer.h:39 APMGR_PORT=16720、remote.c:504-506 `a.sin_addr.s_addr = ~0`（IPv4 受限广播；ipv6=1 时组播 ff02::4150）；每 remote_update_interval（默认 1s）全量广播本节点全部 BSS 的 ssid/freq/noise/load/n_assoc/max_assoc/bssid/rrm_nr/STA 列表；远端节点 ~10 个周期无报文即删除；local_mode=1 关闭互通。**部署含义：mesh 各节点必须在同一 L2（br-lan）且放行 UDP 16720 广播**（受限广播/链路本地组播均不被路由器转发）。

### 2.3 ubus API 面（✦ 实测 `usteer 源码/ubus.c:573-595`）

- 对象 `usteer`，**11 个方法**：local_info / remote_hosts / remote_info / connected_clients / get_clients（无参）；get_client_info(address)；get_config；**set_config 与 update_config**（同一 policy；set_config 先 `usteer_init_defaults()` 恢复默认再覆盖——✦ ubus.c:259-260；update_config 增量覆盖不重置）；set_node_data(node,data) / delete_node_data(node,names)，node='*' 表示主机级。
- 复核提醒：update_config 对**标量**字段确为增量，但若请求里**省略** ssid_list/event_log_types 两字段会将其重置（回调 setter 对 NULL 的处理是重置而非跳过）——动态调优时要带上。
- **12 类 notify 事件**（✦ 实测 event.c:25-39）：probe_req_accept/deny、auth_req_accept/deny、assoc_req_accept/deny、load_kick_trigger/reset/min_clients/no_client/client、signal_kick；负载含 node/sta/signal/reason/threshold/select_reason/local/remote。openmeshd **只需订阅 `usteer` 对象**即可获得全部转向决策的可观测性流水，无需自己实现（openmeshd 当前源码 grep 无任何 usteer 引用，无冲突）。

### 2.4 UCI 配置面（按复核修正后口径；✦ 实测 init.d 转换表与默认值）

| 类别 | 项 | 备注 |
|---|---|---|
| 布尔（5 项） | syslog / ipv6 / local_mode / load_kick_enabled / assoc_steering | ✦ init.d:67-71 布尔转换表恰此 5 项。**probe_steering 不是 UCI 可设项**（复核推翻原「布尔 6 项」表述：此快照无任何 UCI/ubus 途径可改，仅二进制内部默认 0，main.c:107） |
| 字符串（1） | node_up_script | |
| 列表（3） | ssid_list / event_log_types / network | network 仅 init.d 用于解析监听接口，不进 set_config |
| 数值（33 项） | debug_level、sta_block_timeout=30000、local_sta_timeout=120000、local_sta_update=1000、max_neighbor_reports=8、max_retry_band=5、seen_policy_timeout=30000、measurement_report_timeout=120000、load_balancing_threshold=0、band_steering_threshold=5、remote_update_interval=1000、remote_node_timeout=10、min_connect_snr=0、min_snr=0、min_snr_kick_delay=5000、signal_diff_threshold=0、initial_connect_delay=0、steer_reject_timeout=60000、roam_process_timeout=5000、roam_kick_delay=10000、roam_scan_tries=3、roam_scan_timeout=0、roam_scan_snr=0、roam_scan_interval=10000、roam_trigger_snr=0、roam_trigger_interval=60000、band_steering_interval=120000、band_steering_min_snr=-60、link_measurement_interval=30000、load_kick_threshold=75、load_kick_delay=10000、load_kick_min_clients=10、load_kick_reason_code=5 | ✦ 清点 init.d 数值循环恰 33 项（复核推翻原「38 项」计数）；默认值 ✦ 实测 main.c:83-125（memset 零初始化 + 显式赋值） |
| init.d 专有 | enabled | 默认 1，为 0 不启动（✦ init.d:131-135） |

- init 流程：start → load_config → `ubus -t 10 wait_for usteer` → 把全部 UCI（含 network 解析出的 interfaces 数组）转 JSON 调 **`ubus call usteer set_config`**（全量下发）；reload_service = start + load_config；`procd_add_reload_trigger usteer`（uci commit usteer 自动 reload）+ interface.* 原始触发器（✦ 实测 init.d:97-135）。
- **阈值语义表（修订轮逐参数补齐，✦ 均实测 usteer 源码 policy.c）**：

| 参数 | 语义 | 0 值的确切含义 | 出处 |
|---|---|---|---|
| min_snr | 候选/在连客户端的最低可接受信号；亦是弱信号踢阈值 | 不启用该判据 | policy.c:77-80,461-462 |
| roam_scan_snr | 漫游检查触发阈值（进入扫描态），优先于 roam_trigger_snr | 与 roam_trigger_snr 同时为 0 → 漫游检查整体关闭 | policy.c:426-431 |
| roam_trigger_snr | 双重角色：漫游触发回退阈值 + 候选过滤器（信号低于则候选被否决） | 单独为 0 仅关闭其自身两个作用 | policy.c:82,428-429 |
| signal_diff_threshold | 候选须优于当前信号的差值（dB） | **恒不认为更优**（better_signal_strength 直接 return false）→ 信号类候选永不出、漫游引导不触发——是「关闭」而非「零门限」 | policy.c:45-55 |
| load_balancing_threshold | 负载类候选的关联数差门限 | 恒 false → 负载均衡不生效 | policy.c:28-44 |
| min_connect_snr | probe/assoc 准入最低信号（仅 assoc_steering/probe_steering 开启时到达该判据） | 折算为底噪 -95 dBm，近全放行 | policy.c:195-205,157-168 |
| band_steering_threshold | 频段间候选比较时给跨频段目标的关联数偏置（偏好留在 5G），默认 5 | —（默认非 0） | policy.c:35-38 |

- SNR 换算（✦ policy.c:157-168）：负数按绝对 dBm 信号电平解释、正数按 noise（默认 -95，节点实测 noise 优先）+snr 折算。
- **开箱状态**：min_snr / roam_scan_snr / roam_trigger_snr / min_connect_snr / signal_diff_threshold / load_balancing_threshold 默认全 0 = 按上表语义各自关闭——usteer 只做邻居报告同步 + band steering **决策**（band steering 引导在当前 basic 构建不可用，§2.8），不做任何踢/导。**openmeshd 启用转向必须显式下发阈值**（openmeshd 当前不下发任何 usteer 配置，grep 零匹配）。
- ssid_list 非空 = 仅列表内 SSID 参与转向（缺席 = 管所有 SSID）——集成时应显式设置白名单。

### 2.5 与 openmeshd 的分工与红线

现状分工（天然正交）：

| 层 | 归属 | 内容 |
|---|---|---|
| 静态能力位（持久化） | **openmeshd roaming_apply** | 对 /etc/config/wireless 全部 mode='ap' 段重写五项（绕过 uci 直改文件 + 快照回滚事务）+ reload；不触碰运行时转向。上限 8 个 AP iface（netconf.c:407-408）。**注意：仅 controller 有此 RPC（openmeshd.c:259-268），且无 agent 同步通道（§4.1-12）** |
| 运行时转向 | **usteer** | 每秒级邻居表同步（rrm_nr_set）、测量请求、bss_transition_request、del_client；完全不写 /etc/config/wireless、对 11r/FT 无感知（grep ieee80211r/mobility_domain/ft_psk 零命中），与 FT 共存 |

usteer 注册节点时自调 bss_mgmt_enable 开 11k/v，与 UCI 里 k=1/v=1 方向一致（两侧开同一组能力；hostapd 内部叠加幂等性未逐行核，属合理推断）。

**打架红线**：openmeshd 不得写 hostapd 邻居表（usteer 每秒重写 rrm_nr_set 会覆盖）；不得对 hostapd.* 调 bss_transition_request / rrm_beacon_req / link_measurement_req / del_client 做转向（双写互相打架）。roaming_apply 的五项 UCI 保留现状即可。

### 2.6 集成动作清单

1. 安装 usteer 包（依赖已全在镜像，验证方法见 §2.1）。
2. 写 UCI：`usteer.@usteer[0].network='lan'`（AP 间 UDP 16720 走 br-lan）；`list ssid_list='<受管 SSID>'`（强烈建议）；**必设阈值**（按 §2.4 语义表，默认 0 不动作）；enabled 默认即 1 无需显式设。
3. `/etc/init.d/usteer enable && /etc/init.d/usteer reload`（init 会 wait_for usteer 并 set_config 全量下发）。
4. 运行时调优走 `ubus call usteer update_config '{...}'`（增量；记得带上 ssid_list/event_log_types 防重置）而非 set_config（先重置默认）。
5. 观测走订阅 `usteer` 对象 12 类 notify + connected_clients / local_info / remote_info。

启动次序：usteer init START=50（✦）早于 openmeshd START=99（✦）；usteer 经 ubus.object.add 事件 + 初始 ubus_lookup("hostapd.*") 动态发现 AP iface，hostapd 后起无竞争 **『未确认』**（非决策级，未安排复核）。

### 2.7 已知缺陷（锁定快照 7d2b17c9 内，复核 + 实测确认）

1. **负载择由死代码**：✦ 实测 `usteer 源码/policy.c:110-112` `if (has_better_load(current_node, new_node) && !has_better_load(current_node, new_node))` —— X && !X 恒假，UEV_SELECT_REASON_LOAD 永不置位 → load_kick 的 find_better_candidate 恒返 NULL → 负载踢退化为裸踢、负载均衡转向**实际不生效**。上游 master 已修复该条件（`https://github.com/openwrt/usteer/blob/master/policy.c`——材料复核经 WebFetch 对照；修订轮未重取）。**集成验收不要把「负载均衡」计入预期收益**。
2. **get_client_info 统计错列**：ubus.c:115-116 三档统计全读 `stats[EVENT_TYPE_PROBE]`（应为 `stats[i]`），probe/auth/assoc 三档显示同一份数据。

### 2.8 硬前提：hostapd 变体与 802.11v（实测确认；复核原「无需改 hostapd」结论被推翻）

- usteer 依赖 hostapd.<ifname> 的 10 个 ubus 方法。本树 hostapd 三个变体 config 均开 CONFIG_UBUS，但 **bss_transition_request 方法处于 `#ifdef CONFIG_WNM_AP` 内**（构建树 `build_dir/.../hostapd-2024.09.15~5ace39b0/src/ap/ubus.c:1647-1648`），而 wpad-basic-mbedtls 对应的 hostapd-basic.config:162 是 `#CONFIG_WNM=y`（✦）。
- ✦ strings 实测三个 wpad 二进制：`bss_mgmt_enable/get_clients/get_status/del_client/notify_response/rrm_nr_get_own/rrm_nr_set/rrm_beacon_req/link_measurement_req` 9 个全在，**bss_transition_request 缺失**。
- **`bss_mgmt_enable` 在无 WNM 变体上的失败语义（✦ 修订轮源码落定，回应审读 openQuestion）**：方法注册无条件（ubus.c:1641），其枚举/policy 表中仅 BSS_MGMT_EN_BSS_TRANSITION 被编译掉（:878-882/:955-958 `#ifdef CONFIG_WNM_AP`），handler 用裁剪后的 3 项 policy 解析、消息里的 bss_transition 键被**静默忽略**、恒返回 0（:961-985）——即 **11k 三类能力（neighbor/beacon/link measurement）照常开启，bss_transition 位按位 no-op**。「先装 usteer、暂不换变体」的中间态成立：邻居同步与测量可用，仅 11v 引导与 WNM 帧不可用。
- **后果**：当前 .config 构建上 usteer 的 802.11v 引导（漫游 bss_transition_request、band steering request）全部不可用。要兑现「802.11k/v/r 漫游增强」，必须二选一：
  1. `.config` 换 `CONFIG_PACKAGE_wpad-mesh-mbedtls=y`（✦ `package/network/services/hostapd/Makefile`：patsubst 剥 -mbedtls 后 `ifeq ($(LOCAL_VARIANT),mesh) CONFIG_VARIANT:=full`，即用 hostapd-full.config，WNM=y）——**这也是项目种子 seed-config-ax3000t-24.10:47-48 的既定选择（§1.7），当前 .config 与之背离**；
  2. 或在 hostapd-basic.config:162 启用 CONFIG_WNM 重编（偏离官方变体默认）。
- ieee80211r 侧无碍（basic 已含 CONFIG_IEEE80211R=y，✦）。

---

## ③ k/v/r 选项 × 版本矩阵

### 3.1 架构定位：谁在解析 UCI

- **24.10**：wifi-iface 的 k/v/r 选项声明与 hostapd 配置生成全部在 **wifi-scripts 包** `package/network/config/wifi-scripts/files/lib/netifd/hostapd.sh`（1667 行，装为 /lib/netifd/hostapd.sh）；hostapd 包 files/ 下已无 hostapd.sh；迁移提交 2716853132 "wifi-scripts: add new package, move wifi scripts to a single place"。
- **23.05**：解析器是 hostapd 包内 `files/hostapd.sh`（✦ git show openwrt-23.05：1598 行，装为 /lib/netifd/hostapd.sh，Makefile:715）。
- 两版本的 **hostapd.uc 都是运行在 hostapd/wpad 守护进程内的 ucode 脚本**：不解析任何 UCI wifi-iface 选项；k/v/r 相关仅显式处理 nas_identifier（读入 bss.nasid 并回写），其余所有配置行经 bss.data 数组**原样透传**。
- 配置流：netifd（uci_to_blob 按驱动 dump 的声明表**过滤**未声明选项）→ mac80211.sh/hostapd.sh 生成 `/var/run/hostapd-*.conf`（✦ mac80211.sh:562）→ `ubus call hostapd config_set`（✦ mac80211.sh:995）→ hostapd.uc 读文件（hostapd.uc:917-953 → 717-794）。
- **权威区间（全文统一，修订轮定义）**：k/v/r 声明段 = 24.10:326-339 ↔ 23.05:316-329；生成核心段（11v/11k/11r 生成逻辑）= 24.10:904-1013 ↔ 23.05:858-963。§3.3 表内更细的子区间（如 23.05:858-871 为 11v 生成子段）均落在这两个权威区间内。

### 3.2 变体门槛总表（✦ 实测，file:line 一一对应）

IEEE80211R 行：`hostapd-mini.config:151`（注释）/ `hostapd-basic.config:151`（=y）/ `hostapd-full.config:151`（=y）；`wpa_supplicant-mini.config:397`（注释）/ `wpa_supplicant-basic.config:397`（=y）/ `wpa_supplicant-full.config:397`（=y）。WNM 行：hostapd 三变体 `:162`（mini/basic 注释、full =y）；wpa_supplicant 三变体 `:487`（同构）。MBO 行：hostapd 三变体 `:365` 与 wpa_supplicant 三变体 `:578` 均注释禁用。

| 变体 | 11r（CONFIG_IEEE80211R） | 11v WNM（CONFIG_WNM） | MBO | 11k rrm_*/ftm/lci/civic、time_*、nasid |
|---|---|---|---|---|
| mini | ✗ | ✗ | ✗ | ✓（无编译门控） |
| basic（含 -mbedtls/-openssl/-wolfssl） | ✓ | **✗** | ✗ | ✓ |
| full（含 wpad-mesh-*） | ✓ | ✓ | ✗ | ✓ |

- 上游门控（复核用 ifdef 栈脚本对 build_dir 两版源码逐行核实）：FT 十项在 `#ifdef CONFIG_IEEE80211R_AP`（2024.09 树 hostapd/config_file.c:3296-3361）；bss_transition/wnm_sleep_mode* 在 `#ifdef CONFIG_WNM_AP`（:4244-4249）；rrm_*/ftm/lci/civic/time_*/nas_identifier 无门控；mbo 在 `#ifdef CONFIG_MBO`（:4475）。CONFIG_WNM → -DCONFIG_WNM_AP 映射在 hostapd/Makefile:363-366。
- **推论（源码推论，未实机验证）**：在无 WNM 的变体（mini/basic）上设 `bss_transition=1` 或 `wnm_sleep_mode=1`，生成行落入 unknown configuration item 分支（config_file.c:5177-5181 return 1）→ **hostapd 拒绝该 BSS 配置**（§1.7 推理链的第三环）。
- 与 §1.7/§2.8 联动：当前 wpad-basic-mbedtls 上，openmeshd 向导写出的 `bss_transition='1'` 即属此情形（实机验证项见 §4.2 C1）。

### 3.3 选项明细矩阵（23.05 / 24.10 声明与生成逻辑逐行比对）

引用缩写：24.10 = `package/network/config/wifi-scripts/files/lib/netifd/hostapd.sh`；23.05 = `package/network/services/hostapd/files/hostapd.sh`（✦ 两版对应声明行：24.10:326-339 ↔ 23.05:316-329）。

**802.11k（RRM）——无变体门控，任意变体（含 mini）可用**

| UCI 选项 | 23.05 | 24.10 | 语义 | 出处 |
|---|---|---|---|---|
| ieee80211k | ✓ | ✓ | RRM 总开关；置 1 时自动带默认 rrm_neighbor_report=1、rrm_beacon_report=1 | 24.10:331/919-932；23.05:321/873-886；上游 config_file.c:4785-4795 无 #ifdef |
| rrm_neighbor_report | ✓ | ✓ | 广播邻居报告能力并响应 Neighbor Report Request | 同上 |
| rrm_beacon_report | ✓ | ✓ | 广播 Beacon Report（被动/主动/表）测量能力 | 同上 |
| ftm_responder | ✓ | ✓ | 802.11mc FTM 应答方；置 1 且 `iw phy info` 报告 ENABLE_FTM_RESPONDER 才输出 | 24.10:333/934-944；23.05:323/888-898 |
| stationary_ap | ✓ | ✓（**24.10 新增 device 级**，默认 1 无条件输出；23.05 仅 iface 级且只在 ftm_responder=1 时输出） | 宣告 AP 位置固定 | 24.10:124/252-253/333；23.05:323/888-894 |
| lci / civic | ✓ | ✓ | FTM 地理坐标 / 市政地址上报内容 | 24.10:334/941-942；23.05:324/895-896 |
| rnr | ✓（死选项） | ✓（死选项） | 被 json_get_vars 读取（24.10:919/23.05:873）但**从未 config_add_* 声明**（✦ wifi-scripts 包与 23.05 hostapd 包 `config_add.*rnr` 均零匹配）→ netifd 参数表过滤，UCI 值到不了生成器，恒 0 | 24.10:919/921/932；23.05:873/875/886；过滤机制 netifd wireless.c:799-849 + uci blob.c:103 |

**802.11v（WNM）——bss_transition/wnm_sleep_mode* 仅 full 变体；time_* 无门控；mbo 全变体不可用**

| UCI 选项 | 23.05 | 24.10 | 语义 | 变体门槛 |
|---|---|---|---|---|
| bss_transition | ✓ | ✓ | 802.11v BSS 过渡管理（配合 usteer 类工具主动引导 STA） | **仅 full**（CONFIG_WNM_AP；basic/mini 写入会被拒——源码推论） |
| wnm_sleep_mode | ✓ | ✓ | 允许 STA 发起 WNM Sleep Mode 协商 | 仅 full |
| wnm_sleep_mode_no_keys | ✓ | ✓ | 允许无密钥保护的 WNM 休眠（仅跟随 wnm_sleep_mode=1 输出） | 仅 full |
| time_advertisement | ✓ | ✓ | beacon 中广播 TSF 时间通告（实际广播行为未验证） | 无门控 |
| time_zone | ✓ | ✓ | 随时间通告播发时区串 | 无门控 |
| mbo | ✓（不可用） | ✓（不可用） | 脚本会输出 mbo=1，但 OpenWrt 全部官方变体 CONFIG_MBO 均注释禁用 → hostapd 当未知项拒绝 | **全变体不可用** |

**802.11r（FT）——全部需 basic 及以上（mini 无 CONFIG_IEEE80211R），无需 full**

| UCI 选项 | 23.05 | 24.10 | 语义 / 默认 | 出处 |
|---|---|---|---|---|
| ieee80211r | ✓ | ✓ | FT 总开关；>0 时按加密类型追加 FT-PSK/FT-SAE/FT-EAP/FT-EAP-SHA384/FT-FILS-* 到 wpa_key_mgmt | 24.10:336/946-1013；key_mgmt 段两版本 40-90 行逐字节一致 |
| mobility_domain | ✓ | ✓ | 4 位 hex MDID；缺省由 SSID 的 md5 前 4 字符派生。**多机必须显式同值（§4.2 C2）** | 24.10:964 |
| ft_over_ds | ✓ | ✓ | FT over DS（默认 0 = over-the-air） | 24.10:965 |
| ft_psk_generate_local | ✓ | ✓ | 本地生成 PSK 派生 PMK-R0/R1（单机自包含 FT）；auth_type=psk 默认 1 其余 0；为 0 才需 r0kh/r1kh 列表。openmeshd roam_emit_options 固定写 '1'（netconf.c:275） | 24.10:968-975 |
| reassociation_deadline | ✓ | ✓ | FT 重关联超时（TU），默认 1000 | 24.10:966 |
| r0_key_lifetime | ✓ | ✓ | PMK-R0 寿命（分钟），默认 10000 | 24.10:988 |
| r1_key_holder | ✓ | ✓ | PMK-R1 Key Holder ID | 24.10:1002 |
| pmk_r1_push | ✓ | ✓ | AP 主动推送 PMK-R1（默认 0） | 24.10:989 |
| r0kh / r1kh（list） | ✓ | ✓ | 漫游域 R0/R1 Key Holder 白名单（MAC,ID,KEY 三元组）；未设且 auth_secret/key 存在时自动派生 ft_key=md5(mobility_domain/密钥) 并生成通配条目 r0kh="ff:ff:ff:ff:ff:ff,*" / r1kh="00:00:00:00:00:00,..." | 24.10:996-999 |
| nasid | ✓ | ✓ | nas_identifier（FT 域 AP 标识/RADIUS NAS-ID）；缺省 hostapd.uc 回退去冒号 BSSID | 24.10:676；hostapd.uc:70/76 |
| （自动项 ft_iface） | ✓ | ✓ | 非 UCI，由 network 层 ifname 生成 | 24.10:977（OpenWrt 补丁 730-ft_iface） |

STA 侧：wifi-iface 的 ieee80211r 在 STA 模式复用同一 key_mgmt 追加逻辑（FT-PSK/FT-SAE/FT-EAP），需 wpa_supplicant 侧 basic 及以上 **『未确认』**（非决策级，未安排复核；wpa_supplicant 变体 :397 取值已实测同构）。

### 3.4 版本差异总账（修订轮统一口径；diff 均为材料复核实测，区间见 §3.1）

| 维度 | 差异 | 性质 |
|---|---|---|
| k/v/r 声明段（24.10:326-339 ↔ 23.05:316-329） | 逐字节一致（diff 空） | 无差异 |
| 生成核心段（24.10:904-1013 ↔ 23.05:858-963） | 仅 1 处：FT_KEY_CANT_BE_DERIVED（24.10:992-995） | 行为差异：24.10 在 r0kh/r1kh 未设且无 key/auth_secret 时显式报错失败，23.05 静默按空密钥派生 |
| k/v/r 语义变化 | stationary_ap 24.10 升级为设备级选项（:124/:252-253 默认 1 无条件输出）；23.05 仅 iface 级且只在 ftm_responder=1 时输出 | 语义变化 |
| 声明段全集 | 24.10 恰好多 4 项**非 k/v/r** 选项（apup、apup_peer_ifname_prefix、radius_server_auth_port、radius_server_clients；✦ 修订轮全量提取 + comm diff 复核：23.05 165 项 / 24.10 169 项，无 23.05 独有项） | 非 k/v/r |
| 全文其余 hunk | reg_power/6GHz 功率、apup、SAE/PSK 密码文件、RADIUS server、supplicant 速率等（材料复核实测 16 hunk 逐一归类） | 均非 k/v/r |

- **复核勘误**：原「23.05 与 24.10 BSS 选项声明集合完全一致」不成立（24.10 是超集，多上述 4 项）；k/v/r 支持面两版本相同的结论不受影响。
- 上游 config_file.c 引用行号系统性偏移说明：材料部分引用对应 dl/ 原始 tarball 或不同快照，复核已按 build_dir 实际构建源（含 OpenWrt 补丁）校正，正文采用校正值。

### 3.5 通用逃生门：hostapd_bss_options / hostapd_options

- **wifi-iface 级 list `hostapd_bss_options`**：每个元素被原样追加为该 BSS 的一行 hostapd 配置（24.10:1224-1227；23.05:1162-1165）——给未显式支持的选项的通用逃生门；是否生效取决于变体编译门控。
- **wifi-device 级 list `hostapd_options`**：同构，追加到 interface（radio）段（24.10:127/255-258）。
- **复核注（修订轮源码裁决更正，消除与 §3.3 的矛盾）**：bss_transition/wnm_sleep_mode*/mbo 是已声明选项（config_add_boolean，hostapd.sh:326），经逃生门透传属冗余；**rnr 例外——它从未被声明**（见 §3.3 表），**hostapd_bss_options 逃生门是让 rnr=1 进入 conf 的唯一途径**；rrm_link_measurement_report 同为无任何显式处理的有效示例。初稿「rnr/wnm 系列已是显式声明选项」系笔误，已更正。另：列表按空白分词，含空格的单个元素会被拆成多行。

---

## ④ 遗留问题与双机验证清单

### 4.1 遗留问题（openQuestions）

> 修订轮源码裁决情况：审读提出的 9 个 openQuestions 中，**7 个已由本轮源码核实落定并写入正文**——rnr 未声明（§3.3/§3.5）、回滚/开机恢复清理标记（§1.3-5/7）、roam_emit 写入参值（§1.3-1）、mesh 事务同样 30s（§1.3）、bss_mgmt_enable 按位 no-op（§2.8）、首启 wifi-iface 段均有名（§1.6）、材料存放路径（文首）。**MDID 跨节点同步经核实确认为缺口**（见 12）。以下为仍开放项：

1. **黑盒现象两个候选机制无法静态二选一**：「openmeshd 30s 事务自动回滚」vs「写入者走 ubus uci set 未 commit 的 delta-only 路径」——取决于原始现象是「曾出现后消失」还是「生效但从未在文件」，需上机证据裁决（A 组）。
2. **设备快照基线不可源码确定**：「三项存活、两项消失」形态必然来自快照还原，但快照里三项的写入来源/时间取决于设备操作历史。
3. **设备实际固件版本未核对**：设备上安装的 netifd/rpcd/uci 是否与 build_dir（2025.05.23 / 2025.09.01 / 2025.01.20）一致未验证；本轮 strings 实测的三个 wpad 二进制均为本机构建树产物、非设备取样（§1.7）。已另查 2024-01-04 版 netifd 与 2023-08-10 版 uci 关键函数结论不变，更老固件需另行核对。
4. **「basic/mini 上 bss_transition=1 致 hostapd 拒绝配置」是源码推论**（#ifdef 链 + unknown-item fallback），未实机验证；拒绝粒度（单 BSS/整 phy）待 C1。
5. **原始现象记录缺失**：当年观察的具体操作序列、测试日志与固件版本未随材料提供（§1.3）。
6. **usteerd 常驻内存未实测**（估算 <1.5MB）。
7. **usteer 两处上游缺陷是否在 feed 打补丁**（§2.7）：修则偏离上游，不修则负载均衡承诺需降级表述——V0.3 取舍。
8. **阈值数值属产品调优决策**：源码只给默认值与语义（§2.4 表），具体数值需真实射频环境标定。
9. **feed 锁定的 usteer 是 2022-08-18 快照**：是否升级到更新上游（UCI 面/ubus 面会变）需 V0.3 单独决策。
10. **治本代码改法『未确认』**：roam_rewrite_file 合并保留、自动确认、注释更正（§1.6）；匿名段漏管仅影响手工创建的匿名段（§1.6，低风险）。
11. **time_advertisement/time_zone 的实际 TSF 广播行为未验证**；STA 侧 ieee80211r 细节『未确认』（§3.3）。
12. **漫游配置无 agent 同步通道（修订轮确认为集成缺口）**：roaming_apply 仅 controller 注册（openmeshd.c:259-268），远程 OM_SES_CONFIG 载荷仅 mesh_id/band/key/mode（session_net.c:541-565）——**agent 节点的漫游五项只能经标准 uci/LuCI 手工配置**，多机 11r 需逐节点保证同 mobility_domain，或为 V0.3 补 agent 同步功能。
13. **工作区 .config 与 seed-config 的 wpad 变体背离原因**（§1.7）：种子规定 wpad-mesh-mbedtls、.config 实选 basic——需确认按种子重生成或有意为之。
14. 本地 openwrt-23.05 引用停在 33063b4c（该文件最后实质修改 2024-08-28），上游再有提交行号可能微移（风险极低）。

### 4.2 双机验证清单

> 设备：两台 AX3000T（下称 A=controller、B=agent/对等节点），同一 mesh/同一 br-lan。命令均为设备端 shell。

**A 组（单机即可）——「剥除之谜」上机裁决**

| # | 操作 | 判据 |
|---|---|---|
| A1 | `logread \| grep netconf`（日志串锚定 ✦ netconf.c:428-430 "netconf: roaming k=…"、:451 "transaction %u confirmed"、:482 "rollback of %s FAILED"、:493 "transaction %u rolled back"） | 出现 rolled back = 回滚机制实锤（§1.3-5） |
| A2 | `ls /etc/openmesh/` | txn.pending / wireless.om-bak 存在 = **事务正在进行（30s 窗口内）或掉电后 openmeshd 未及执行开机恢复**；正常 confirm/rollback/恢复均清理两者（netconf.c:445-448/486-489/522-535，§1.3-7） |
| A3 | `uci changes wireless` | 有输出 = 改动只在 /tmp/.uci/wireless（delta 幻象路径） |
| A4 | `ls /var/run/rpcd/snapshot-files/` | 非空 = rpcd 回滚窗口曾开启 |
| A5 | 复现：LuCI 向导点 "Apply roaming enhancement (802.11k/v/r)" → 立即 `cat /etc/config/wireless`（五项应在）→ **不点 Keep**，等 30s → 再 cat + logread；**同步观察 `logread` 中 hostapd 的 unknown configuration item 报错与 BSS 状态（`wifi status` / `ubus call network.wireless status`）** | 文件回到点击前快照态 + netconf rolled back + 第二次 network reload 生效 = §1.3 机制链闭环；若当前固件为 basic 变体，应同时看到 §1.7 推理链的 BSS 被拒与 30s 自愈 |
| A6 | 对照：再点一次漫游按钮 → 30s 内点 Keep → 等 60s 再 cat | 五项保留 = confirm 路径正常 |

**B 组（双机）——usteer 集成验证**

| # | 操作 | 判据 |
|---|---|---|
| B0 | **前置 0——确认设备实际变体**：`ubus list \| grep hostapd.` 后 `ubus list hostapd.<ifname>` 看方法表 | 方法表含 `bss_transition_request` = WNM 变体；不含 = 设备仍是 basic（对应 §1.7 的 .config/seed 背离，先统一构建配置：`.config` 设 `CONFIG_PACKAGE_wpad-mesh-mbedtls=y` 重编，即种子既定选择） |
| B1 | A/B 均 `opkg install usteer`；`uci set usteer.@usteer[0].network='lan'; uci add_list usteer.@usteer[0].ssid_list='<SSID>'; uci set usteer.@usteer[0].min_snr=-75 ...（阈值按 §2.4 语义表与产品定）`；`/etc/init.d/usteer enable && reload` | `ubus call usteer get_config` 返回下发值 |
| B2 | A 上 `ubus call usteer remote_info` | 出现 B 节点（UDP 16720 学到对端） |
| B3 | `tcpdump -i br-lan -n udp port 16720` | 双向周期（~1s）广播报文 |
| B4 | STA 连 A，物理移向 B；`logread -f` + 订阅 `ubus` usteer 事件 | 观察 signal_kick / probe/assoc 事件与 bss_transition_request 引导、STA 完成切换 |
| B5 | `grep VmRSS /proc/$(pidof usteerd)/status` | 校准 §2.1 内存估算（预期 <1.5MB 量级） |
| B6 | 负载均衡**跳过验收** | §2.7 死代码，不纳入预期收益 |

**C 组（双机）——k/v/r 选项行为验证**

| # | 操作 | 判据 |
|---|---|---|
| C1 | 切回 basic 变体（或刷回当前固件）：`uci set wireless.@wifi-iface[N].bss_transition='1'; uci commit wireless; wifi reload`；观察 `logread` 与 `wifi status` | 验证源码推论：hostapd 报 unknown configuration item、对应 BSS 状态（拒绝粒度：单 BSS 还是整 phy）（§3.2/§1.7） |
| C2 | **多机 11r（按 §4.1-12 现实修订）**：A（controller）`ubus call openmesh roaming_apply '{"k":true,"v":true,"r":true,"domain":"<固定 4 位 hex>"}'`（**勿用向导按钮——不带参=随机 domain**）；B（agent）无 roaming RPC，用标准 uci 手工写同值五项（`uci set wireless.@wifi-iface[N].ieee80211r='1'; uci set ...mobility_domain='<同值>'; uci set ...ft_psk_generate_local='1'; uci set ...ieee80211k='1'; uci commit wireless; wifi reload`）；STA 在 A/B 间漫游看 `logread` | 双机同 mobility_domain 下出现 FT 关联记录（11r 在 basic 上可用，§1.7）；顺带验证「agent 无同步通道」结论 |
| C3 | `uci set wireless.@wifi-iface[N].rnr='1'; uci commit wireless; wifi reload; cat /var/run/hostapd-*.conf` | conf 内无 rnr 行 = netifd 过滤死选项实证（§3.3） |
| C4 | 24.10：对无 key/auth_secret 且未设 r0kh/r1kh 的 iface 设 ieee80211r='1' 后 reload | wireless setup 失败并报 FT_KEY_CANT_BE_DERIVED（§3.4 差异项） |
| C5 | 启用 usteer 后在 STA 侧查邻居报告（如 `wpa_cli` / 手机 Wi-Fi 分析工具） | 邻居表含对端 BSS（11k 经 usteer rrm_nr_set 同步，§2.2） |

---

## 附录 A：复核状态与勘误记录

**材料内已复核（verified）且两轮抽查命中的承重结论**：netifd 纯读者（含 uci_rename 一处细节更正）、netifd→脚本只传 JSON、hostapd.uc 无罪、openmeshd 真凶机制链、「两行消失三项存活」形态分析、向导定时炸弹、openmeshd 不被 reload 触发、rpcd 60s 回滚、delta 幻象、注释归因证伪、规避操作面；usteer 版本锁定/体积/依赖、三层能力、漫游状态机、三类 kick、band steering、UDP 16720、11 方法 12 事件、阈值默认值、分工正交、集成清单、两处缺陷；kvr 架构定位、hostapd.uc 角色、11k/11v/11r 矩阵、变体总表、版本一致性定量、透传逃生门。

**复核推翻/修正原表述的（正文已按修正口径）**：

1. usteer UCI 选项全集：原「布尔 6 项（含 probe_steering）、数值 38 项」→ 修正为布尔 5 项、数值 33 项（实测复核）。
2. usteer 集成前提「无需改 hostapd」→ 推翻：wpad-basic-mbedtls 缺 bss_transition_request（strings 三二进制实测 + CONFIG_WNM 事实 + §2.8 失败语义分析）。
3. kvr「23.05/24.10 BSS 选项声明完全一致」→ 推翻：24.10 恰好多 4 项非 k/v/r 选项（comm diff 实测）；k/v/r 支持面相同的结论不受影响。
4. **初稿 §3.5「rnr 已是显式声明选项（透传冗余）」系笔误，与 §3.3 矛盾**（审读 P0-1）→ 修订轮源码裁决：rnr 从未声明，逃生门是其唯一途径（§3.5）。
5. 材料证据细节勘误（不影响结论）：uci_rename 非零匹配（netifd config.c:122）；OM_TXN_TIMEOUT 实为 netconf.h:12；om_roaming_apply 默认值实为 ubus_api.c:408；delta 文件无 .delta 后缀（/tmp/.uci/wireless）；uci confirm 需带 session；LuCI 实际回滚窗 ≥90s；hostapd.sh 实际路径在 wifi-scripts（24.10）。
6. **修订轮新发现（超出原材料）**：写者清单补充 rfkill 按键与平台迁移脚本（§1.2）；工作区 .config 与 seed-config 的 wpad 变体背离（§1.7）；漫游配置无 agent 同步通道（§4.1-12）；「五条路径」计数更正为 7 触发点 4 类（§1.3）；roam_emit_options 写入参 0/1 而非硬编码（§1.3-1）。

**『未确认』条目（决策级需注意的）**：hostapd 包其余脚本洗清细节（§1.2 表末行）；roam_rewrite_file 匿名段 bug 治本改法；usteer 启动次序/发现细节；STA 侧 ieee80211r。其余未确认项均为非决策级。

## 附录 B：撰写/修订两轮抽查清单（✦ 项全部命中）

**初稿轮（约 20 项）**：

| 抽查项 | 方式 | 结果 |
|---|---|---|
| OM_TXN_TIMEOUT=30 / roam_emit_options / ours[] 五项 / roaming_apply 全链 / rollback / 注释原文 | Read netconf.h/netconf.c | 全部命中 |
| ubus 默认 k=v=r=true + 随机 domain / 向导零实参无 confirm / START=99 | Read ubus_api.c/wizard.js/init.d | 命中 |
| netifd uci_save/commit/add 零匹配；uci_set:97/uci_rename:122 仅 bridge | grep + Read netifd 源码 | 命中 |
| hostapd.uc/wpa_supplicant.uc 无 uci；mac80211.sh:562/995 | grep + Read | 命中 |
| 六份变体 config 取值；.config wpad 键；wpad-mesh→full 映射 | sed/Read Makefile | 命中 |
| **wpad 二进制 ubus 方法 strings** | strings -a × 3 二进制 grep -x | 9 命中，bss_transition_request 缺失 |
| usteer feed 锁定/死代码/set_config 重置/bss_mgmt_enable/11 方法/12 事件/默认值/START=50/布尔 5 项/数值 33 项/APMGR_PORT=16720/广播 ~0 | Read /tmp/usteer-src 快照 | 全部命中 |
| 23.05 hostapd.sh 1598 行/声明段/生成行；23.05↔24.10 声明集差恰 4 项非 k/v/r | git show + comm diff | 命中 |
| rpcd 回滚常量 / uci savedir | Read | 命中 |

**修订轮（约 20 项，对应审读 openQuestions 与 P0/P1）**：

| 抽查项 | 方式 | 结果 |
|---|---|---|
| rnr 声明状态 | grep `config_add.*rnr`（wifi-scripts 包 + 23.05 hostapd.sh） | 双版本零匹配——§3.3 对、初稿 §3.5 笔误（P0-1 裁决） |
| mesh_apply 事务（快照/marker/定时器） | Read netconf.c:158-264 | :183-201/:203-210/:249-250 命中（P1-9 裁决：同 30s 事务） |
| recover 标记/快照清理 | Read netconf.c:499-538 | :522-537 删标记+快照+reload（P1-7 裁决） |
| 改写入口枚举 | grep 调用点（ubus_api.c:269/296/328/427、session_net.c:541-565、netconf.c:68、openmeshd.c:269） | 7 触发点 4 类（P0-5 更正） |
| RPC 角色门控 | Read openmeshd.c:255-275 | :259-268 仅 controller（P1-8 裁决依据） |
| OM_SES_CONFIG 载荷 | Read session_net.c:541-575 | 仅 mesh_id/band/key/mode——漫游无同步通道（P1-8 裁决） |
| seed-config 类型与内容 | file + grep | .config 种子片段；:47-48 规定 wpad-mesh-mbedtls——与 .config 背离（新发现） |
| config_generate 五项 / wifi-iface 生成 | grep | 五项零匹配；wireless 首启由 mac80211.uc 生成，:46 段名 "wireless.default_"+name（均有名） |
| bss_mgmt_enable WNM 位失败语义 | Read 构建树 src/ap/ubus.c:875-995 | 按位 no-op、恒返回 0，11k 三类照常（P1-13 裁决） |
| usteer 阈值语义（signal_diff/roam_scan/roam_trigger/min_connect/band_steering/load_balancing/noise-95） | Read policy.c:28-60/70-95/155-170/195-210/420-435 | 全部落定（P2-19，§2.4 表） |
| ipk control 元数据 | tar 完整解包 | Version: 2022-08-18-7d2b17c9-1 等四字段与 feed 一致（P1-12） |
| .config 六依赖键 / LuCI uci.uc:47-53 与 uci.js:975-1000 / rpcd session list（session.c:1351-1353） | grep/Read | 命中（P1-12/P2-23） |
| 写者全量枚举 | 逐目录 ugrep -r（base-files/network/openmeshd/system/feeds/scripts/target） | 新发现 rfkill 与 filogic 05-wifi-migrate；feeds 三包未选入 .config（P0-4）。**工具注记**：本机 ugrep 7.8.4 的全树 -r 与 find -exec 组合会漏报（同模式直查 netconf.c 命中而递归漏之），故枚举按单目录逐个执行 |

---

## 修订记录（R2 → 审读意见逐条对应）

| 审读条目 | 处置 |
|---|---|
| P0-1 rnr 矛盾 | 源码裁决（rnr 未声明），§3.3 维持、§3.5 更正并显式注明笔误 |
| P0-2 band steering 口径 | §2.2 band steering 段加「当前 basic 构建不可用，见 §2.8」限定，两处互引 |
| P0-3 「唯一解释」/现象未具体化 | §1.3 新增「原始现象记录」（两行= bss_transition/ieee80211r，三项= ieee80211k/mobility_domain/ft_psk_generate_local，材料口径+记录缺失声明）；形态解释改为附前提限定，与 §4.1-1 对齐 |
| P0-4 写者清单方法论 | §1.2 新增检索方法/范围/工具注记；修订轮新发现 rfkill 与平台迁移脚本补入清单 |
| P0-5 「五条路径」计数 | §1.3 更正为 7 触发点 4 类并逐调用点核验 |
| P1-6 向导按钮后果链 | §1.7 显式拼装推理链（源码推论标注）；A5/C1 增 hostapd 拒绝与 BSS 状态观察项 |
| P1-7 标记生命周期 | §1.3-5/7 补 rollback/recover 清理；A2 判据改写 |
| P1-8 MDID 同步 | §2.5/§4.1-12 确认为缺口（RPC 仅 controller、远程指令仅 mesh）；C2 改写为带显式 domain + agent 手工 uci |
| P1-9 mesh 事务 | §1.3 补 mesh_apply 同 30s 事务 |
| P1-10 区间不一致 | §3.1 定义权威区间，全文统一 |
| P1-11 计数口径 | §3.4 改为差异总账表；摘要第 3 条对齐 |
| P1-12 缺出处六项 | 逐条补：uci.uc:47-53/uci.js:975-1000、ipk URL+control 实解、.config 六键验证法、上游修复 URL、policy.c:157-168、seed-config 实查（并因此发现变体背离） |
| P1-13 bss_mgmt_enable 失败语义 | §2.8 源码落定：按位 no-op，11k 可用 |
| P1-14 第三二进制来源 | §1.7 写明三个均为构建树产物、非设备取样 |
| P2-15/16/17/18/19/20/21/22/23 | §2.2 时序括注与 get_clients 对象名、四处「全树」加范围、变体引用改 file:line 一一对应、文首材料可追溯性、§2.4 阈值语义表、现象具体化（P0-3 同）、roam_emit 写值语义+摘要「真正」软化、get_clients 双义标注、A1 日志串锚定+sid 获取 |
| P3-24/25/26/27 | 三处乱码修复（§1.2 表/§2.4/§3.3 表）、.config 键名为主、「全部承重结论」改「抽样复核」、usteer 来源两处加限定 |
| openQuestions 1-9 | 7 项源码裁决入正文（见 §4.1 开头）、1 项确认为缺口（MDID 同步）、1 项如实申报（原始现象记录缺失） |

*本报告为纯只读研读产物（未修改任何源码/配置文件）；矩阵全文可直接汇编入 docs/research/。*
