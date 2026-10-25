# x86 节点 RTL8188EE 无线驱动：补丁与自编译指南

> 场景：x86 mesh 节点（AMI Aptio CRB）唯一的无线网卡是 Realtek RTL8188EE
> （PCI，2.4G 单频 802.11n，rtlwifi 驱动家族，已实测支持 mesh point 模式）。
> OpenWrt 在 23.05 时代之后把 rtlwifi 家族从 mac80211 的 PKG_DRIVERS 剔除
> （官方包不再提供 kmod-rtl8188ee），但 backports 源码一直完整包含 rtlwifi
> （6.12.96 里 301 个文件，含 Kconfig/Makefile 集成点）。本补丁把它加回。
>
> 配通记录见 `docs/design/v0.2-plan.md`；历史背景：7 月曾在内核 5.15 树上
> 编通过（lede 旧状态），树更新后支持丢失，r19 在 24.10/内核 6.6 上补回。

## 什么时候需要本文档

- x86 重刷了不含驱动的镜像（官方/种子镜像默认都没有它）；
- 构建树更新（`git pull`）或内核升级后需要重打重编；
- 驱动 ipk 丢失需要重新产出。

## 前提

- OpenWrt 构建树：`~/Build_OpenWrt/openwrt`（当前 24.10-SNAPSHOT，内核 6.6.x）；
- 树已配 x86/64 目标且工具链编过（首次切目标全编约 1 小时，之后增量）；
- **节点内核必须与构建树完全一致**（kmod 有 vermagic 强校验）：节点
  `uname -r` vs 树 `make -s kernel_version`。不一致就得连内核重刷，没有捷径。

## 四步流程

### 1. 应用补丁

```sh
cd ~/Build_OpenWrt/openwrt
git apply ~/Build_OpenWrt/luci-app-openmesh/scripts/openwrt-patches/0001-mac80211-restore-rtl8188ee.patch
```

补丁只改 `package/kernel/mac80211/Makefile`，三处：
1. `PKG_DRIVERS` 清单加 `rtl8188ee`；
2. 驱动内核开关一行：`config-$(call config_package,rtl8188ee) += RTLWIFI RTLWIFI_PCI RTL8188EE`；
3. `KernelPackage/rtl8188ee` 定义块（rtlwifi 核心 + rtl_pci 总线 + 8188ee
   芯片**三模块合一**打包，依赖 kmod-mac80211 + rtl8188e-firmware）。

树更新后 apply 冲突时：按上面三件事手动重做即可（补丁很小，行号漂移正常）。

### 2. 选包并编译

```sh
cd ~/Build_OpenWrt/openwrt
printf 'CONFIG_PACKAGE_kmod-rtl8188ee=y\n' >> .config
make defconfig
grep -E '^CONFIG_PACKAGE_kmod-(rtl8188ee|mac80211)' .config   # 必须都是 =y！
rm -rf build_dir/target-x86_64_musl/linux-x86_64/mac80211-regular
make -j$(nproc) package/kernel/mac80211/compile
```

**关键坑（r19 实测踩过）**：第 2 步 grep 若发现符号被 defconfig 丢掉（`=y`
消失），说明包元数据解析坏了——症状是构建时刷屏
`WARNING: ... has a dependency on 'kmod-mac80211', which does not exist`。
原因：改 Makefile 前后 pkginfo 元数据缓存陈旧。处置：确认补丁三处齐全后
删掉 `build_dir/.../mac80211-regular` 再 `make defconfig` 重新生成元数据。
产物在 `bin/targets/x86/64/packages/`（kernel 类 ipk 在这里，不在
bin/packages！）：`kmod-cfg80211`、`kmod-mac80211`、`kmod-rtl8188ee`；
固件包 `rtl8188e-firmware` 在 `bin/packages/x86_64/base/`（与内核无关可复用）。

backports 全量编译约 15~25 分钟。

### 3. SSH 部署（openmesh 升级同套路）

```sh
cd ~/Build_OpenWrt/openwrt/bin
scp targets/x86/64/packages/kmod-{cfg80211,mac80211,rtl8188ee}_*.ipk \
    packages/x86_64/base/rtl8188e-firmware_*.ipk root@192.168.40.232:/tmp/
ssh root@192.168.40.232 'opkg install /tmp/kmod-*.ipk /tmp/rtl8188e-firmware*.ipk && reboot'
```

装 kmod 后建议重启（模块自动加载最稳）。纯 userspace 包（openmeshd、
luci-app-openmesh）升级不必重启：`opkg install` 后
`/etc/init.d/openmeshd restart` 即可。

### 4. 验证

```sh
ssh root@192.168.40.232 '
  ls /sys/class/ieee80211/                            # 应出现 phy0
  iw phy0 info | grep -A9 "Supported interface modes" # 应含 mesh point
  wifi config && uci show wireless | head -4          # 应出现 radio0 (band 2g)
'
```

三条全过，就可以在控制器 `mesh_create`（band=2g）推送建网了。

## 已知边界（如实）

- **内核 6.6 + backports 6.12.96 的 rtlwifi 是本项目首次验证组合**；此前
  只在内核 5.15 验证过。异常时排查顺序：`dmesg | grep -E "rtl|firmware"`、
  降级测试（去掉加密的 mesh）。
- rtl8188 是 1x1 2.4G 老卡：HT20/40 上限，做协议验证足够，别指望吞吐。
- 官方源永远不会有这个包（上游已删）；本补丁是唯一来源，随仓库保存。
- 节点 LuCI/软件升级后若 `/etc/dropbear` 权限变成组可写（775），dropbear
  会拒绝公钥登录——`chmod 700 /etc/dropbear` 恢复（r18 踩过）。

## 附：为什么不用 lede 树（内核 5.15，历史验证过）

lede 树 2026-10 现状：已更新到近期 master、当前 .config 是 ramips/4A、
无 x86 工具链、mac80211 同样没有 rtl8188（同样要打本补丁）、且需重刷节点。
除非想让 x86 回 23.05，否则 openwrt 树路线严格更快。
