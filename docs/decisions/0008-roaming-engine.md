# ADR-0008 智能漫游引擎复用 usteer（基础 k/v/r 自研编排）

- 状态：已接受（2026-10-24）
- 影响版本：V0.3

## 决策

分两层：

1. **基础层自研编排（本模块）**：802.11k（邻居报告）/ 802.11v（BSS 过渡）/ 802.11r（FT 快速漫游）均为 wifi-iface 原生 UCI 选项（23.05 hostapd.sh 实证 r/v 原生、k 直通；24.10 hostapd.uc 透传），由 netconf 事务引擎统一下发（快照/30s 确认/回滚，与 mesh_create 同一套安全网）。
2. **智能层复用 usteer**（feeds/packages 实证可用）：负载感知的引导/踢除决策不自研。选 usteer 而非 dawn 的理由：
   - usteer 基于 libubox/libubus，与本项目同栈、内存占用小（~100KB 级），API 即 ubus（编排成本低）；
   - dawn 自带完整度量/JSON 体系与更多依赖，面向更复杂部署；
   - 两者都比"自研漫游决策"成熟得多（红线：不重复造轮子）。
   V0.3 后期将 usteer 作为可选 DEPENDS 接入向导（"智能漫游"开关）。

## 后果

- roaming_apply 只负责把正确的 UCI 选项落到所有 AP 面 wifi-iface（含用户已有 AP），不碰 mesh 回程面。
- 多 SSID/访客网络的差异化漫游策略留 V1.0。
