# M14 漫游编排（802.11k/v/r）

状态：已交付（r15，k/domain/ft_psk 真机稳定；bss/11r 受 netifd 行为限制，见已知问题）

## 开工前 10 问（摘要）

1. **解决**：一键为所有 AP 面 wifi-iface 下发 802.11k/v/r（目标 15 的编排面；目标 16 漫游监控在 V0.3 后续）。
2. **原生**：k/v/r 均为 wifi-iface 原生 UCI 选项（23.05 hostapd.sh 实证 r 原生处理 FT key_mgmt、v 在 config_add_boolean；24.10 hostapd.uc 透传）。
3. **复用**：ADR-0008——基础层 netconf 事务编排；智能层 usteer（后续）。
4. **自研**：仅编排逻辑。
5. **交互**：ubus `roaming_apply{k,v,r,domain}`（写 ACL）→ netconf 事务（30s 确认/回滚）→ CONFIG 通道不推送（AP 面配置属各节点本地，V0.3 双机再评估统一域下发）。
6. **存储**：/etc/config/wireless + backhaul 无新增；FT mobility_domain 缺省随机 4 hex。
7. **ubus**：roaming_apply；向导页新增"应用漫游增强"按钮。
8. **恢复**：复用 netconf（快照/标记/30s/掉电恢复）。
9. **测试**：真机 drill（见回填）。
10. **兼容**：选项三版本同名；netifd 剥除行为见已知问题。

## 实现后回填：八轮排障的证据链（这是本模块最有价值的产出）

现象：五项选项写入后，`bss_transition`/`ieee80211r` 消失，`ieee80211k`/`mobility_domain`/`ft_psk_generate_local` 存活。依次排除并最终定位：

1. libuci `uci_set` 复用脏 `ptr.o` → 加 memset 无效（且 uci_set 全程返回成功）；
2. uci CLI 命令串 `cmd[160]` 截断（真实缺陷，已修：逐条执行）→ 无效；
3. libuci context 与 CLI 并存干扰 → 先释放后写 → 无效；
4. **0.3s 粒度文件轮询**揭示：uci show 看到的三项在**文件本体中也不存在**（delta 假象）；
5. 最终直接文件手术（五连 fprintf 原子替换）+ reload 后 dump：**五行写入、两行被后续 reload 剥除**——写入者全程无辜。

**结论**：netifd 无线 reload 管线在重写 /etc/config/wireless 时，于本机（psk2 单 AP）环境下剥除 `bss_transition`/`ieee80211r`（推测与加密组合/规范化规则相关；精确定义需读 hostapd.uc/netifd wireless 源码，排入 V0.3 双机阶段）。k/domain/ft_psk 稳定。

## 已知问题（如实）

- 本机 psk2 环境下 bss/11r 不持久（netifd 剥除）；802.11r 完整生效（FT 漫游实测）依赖双机 + 可能需要 SAE/FT 兼容加密调整——**V0.3 双机阶段解决**；
- roaming 不经 CONFIG 推送（各节点独立应用）；多 SSID 差异化留 V1.0；
- 教训沉淀：`uci show` ≠ 文件本体（delta 合并）；排障时**文件本体轮询**应作为第一手段。

## 研究修正（2026-10-24，工作流三路源码审计推翻本文档先前结论）

**"netifd wireless reload 剥除选项"的归因是错的。** 43 条结论/33 条独立复核的源码审计（docs/research/v03-research-report.md）证明：

1. netd/wifi-scripts/hostapd.uc 全链路对 /etc/config/wireless 是**纯读者**（uci_save/commit 零写点，逐行核实）。
2. 真机制有二：① **openmeshd 自身 30s 事务回滚**——把文件还原到事务前快照并自带第二次 reload，制造了"reload 剥除"的表象（"三项存活两项消失"正是快照时序的形态：存活项在旧快照里、消失项写入晚于快照）；② **uci delta 幻象**——未 commit 的 set 只进 /tmp/.uci delta，netifd 读配置会应用 delta（生效≠落盘）。
3. 次要机制：LuCI/rpcd 的 `uci apply{rollback:true}`（默认 60s/LuCI 实际 90s）也会整文件快照回滚，吞掉窗口期其他写入。

**由此落地的代码修复（r17/r18）**：
- netconf.c 错误注释已改正；
- **WNM 变体防护**：wpad-basic 无 WNM 时 `bss_transition=1` 会令 hostapd 以 unknown configuration item 拒绝整个 BSS 配置（复核员 strings 实证）——roaming_apply 现探针（整行精确匹配，防 STA 侧 `_tx` 符号子串误命中）检测后自动降级 v 并告警；值 '0' 时生成器不透传、无害（运行时已验证）。
- **匿名段漏管修复**：roam_rewrite_file 改按文件序号匹配 wifi-iface 段（cfgXXXX 匿名段不再漏管）。
- 双机验证清单（A/B/C 三组）见研究报告 §4.2。
