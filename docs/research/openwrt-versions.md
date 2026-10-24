# OpenWrt 版本兼容性采样（进行中）

## hostapd 802.11k/v/r 配置接口（V0.3 前置，2026-10-24 首批采样）

**24.10（本地树实证）**：hostapd 配置生成已 ucode 化（`package/network/services/hostapd/files/hostapd.uc`），无线选项经 netifd→hostapd.uc 转换；`nasid` 等显式处理，其余大量 hostapd 原生选项以 **UCI 透传**形式生效（wifi-iface 上直接写 `option ieee80211k '1'`、`option bss_transition '1'`、`option ieee80211r '1'` 等）。
- 待验证（V0.3 实现时真机核对）：透传白名单的确切集合；`rrm_neighbor_report` 等子选项格式。
- 23.05：hostapd.sh（shell 版转换器）——采样待做（同方法 grep `ieee80211`）。
- master：hostapd.uc 演进版——采样待做。

**dawn / usteer**（feeds/packages 实证存在）：802.11k/v 智能漫游的成熟实现，V0.3 评估复用其一而非自研（符合"不重复造轮子"），本项目角色=编排其 UCI 配置。

## netifd proto 热装载（V0.2 实证）

netifd 仅在进程启动时扫描 `/lib/netifd/proto/`——运行期安装的 proto 包（如 batman-adv）不被识别，需一次 `network restart`；预装固件无此问题。**含义**：V0.2 模式 B 的 DEPENDS 已含 kmod-batman-adv，正式固件部署不受影响；opkg 后装场景在 README 部署章节必须写明。

## iwinfo JSON

M2 采样结论维持：monitor 用文本解析（不依赖 --json），三版本安全。

## make $(wildcard) 与符号链接包目录（构建系统行为，实证）

luci.mk 的 po/* 发现使用 make `$(wildcard)`，不穿透符号链接的包目录 → i18n 子包静默消失。已固化：luci 包一律复制安装（setup-dev.sh / ci-sdk-build.sh）。
