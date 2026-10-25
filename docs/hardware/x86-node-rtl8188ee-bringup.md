# x86 节点 RTL8188EE WiFi 驱动编译实录（r19）

> 整理自 2026-10-25 的实际修改过程（构建树内未提交 diff + 产物时间线）。
> 背景：x86 第二测试节点（mesh-node-x86，192.168.40.232，已领养为 agent）
> 的 WiFi 网卡是 Realtek **RTL8188EE**（PCIe，2.4G 单频 802.11n 1T1R），
> 需要给它编译驱动以支撑 802.11s 无线回程测试。

## 1. 问题：OpenWrt 24.10 上游没有这个驱动的 kmod

`package/kernel/mac80211` 的 `PKG_DRIVERS` 列表（已核对 `git show HEAD`）**不含任何 rtlwifi 家族成员**——24.10 官方 mac80211 包没有 `kmod-rtl8188ee`（上游只收录了 mt76/ath/rtl8xxxu 等常见型号）。x86 节点的种子镜像因此无 WiFi。

内核主线本身是有驱动的（`drivers/net/wireless/realtek/rtlwifi/rtl8188ee/`，mac80211 子系统原生支持 **mesh point 模式**——已在节点上验证），所以路径是：**在 mac80211 包里自己定义 KernelPackage，让 backport 构建系统把主线驱动编出来**。

## 2. 修改点（共三处，均在构建树 `openwrt/` 内，未提交）

### 2.1 `package/kernel/mac80211/Makefile` —— 新增包定义

```diff
 PKG_DRIVERS = \
 	mac80211-hwsim \
 	mt7601u \
+	rtl8188ee \
 	rsi91x rsi91x-usb rsi91x-sdio\
```

```diff
+define KernelPackage/rtl8188ee
+  $(call KernelPackage/mac80211/Default)
+  TITLE:=Realtek RTL8188EE PCIe wireless adapter
+  DEPENDS+= +kmod-mac80211
+  FILES:= \
+	$(PKG_BUILD_DIR)/drivers/net/wireless/realtek/rtlwifi/rtlwifi.ko \
+	$(PKG_BUILD_DIR)/drivers/net/wireless/realtek/rtlwifi/rtl_pci.ko \
+	$(PKG_BUILD_DIR)/drivers/net/wireless/realtek/rtlwifi/rtl8188ee/rtl8188ee.ko
+  AUTOLOAD:=$(call AutoProbe,rtl8188ee)
+endef
```

```diff
 config-$(call config_package,wlcore) += WLCORE WLCORE_SDIO
+config-$(call config_package,rtl8188ee) += RTL_CARDS RTLWIFI RTLWIFI_PCI RTL8188EE
```

**原理说明**：
- rtlwifi 家族是**三层模块栈**：`rtlwifi.ko`（公共核心）→ `rtl_pci.ko`（PCIe 总线）→ `rtl8188ee.ko`（芯片型号），三个 .ko 必须打进同一个包（后两个不单独装）；
- `config_package` 那行是 backport 的内核 Kconfig 映射：`RTL_CARDS` 是全家总开关、`RTLWIFI`/`RTLWIFI_PCI`/`RTL8188EE` 逐级选择，缺一不可；
- `AutoProbe(rtl8188ee)` 让 udev/procd 热插入时自动 modprobe。

### 2.2 `target/linux/x86/config-6.6` —— 补内核依赖

```diff
+# wireless deps (mesh node bring-up, r19)
+CONFIG_CRYPTO_CMAC=m
```

**原理**：rtlwifi 的 Kconfig `select CRYPTO_CMAC`，而 x86 目标的默认内核配置没开它——首次编 kmod 时 modpost 报 missing symbol（`crypto_cmac_*` 未定义）。在 target config 里以 `=m`（模块）补上即可，代价约 8KB。

### 2.3 `.config` —— 选中包

```
CONFIG_PACKAGE_kmod-rtl8188ee=y
```

经 `make menuconfig` 勾选或直接追加 + `make defconfig`。

## 3. 编译与产物

```sh
make package/kernel/mac80211/clean
make package/kernel/mac80211/compile -j$(nproc)   # 须重编: 包定义变了
make target/linux/install -j$(nproc)               # CRYPTO_CMAC 进内核模块集
```

产物（2026-10-25 13:54，`bin/targets/x86/64/packages/`）：

| 文件 | 大小 | 说明 |
|---|---|---|
| `kmod-rtl8188ee_6.6.159.6.12.96-r4_x86_64.ipk` | 102KB | 三模块合一（vermagic 与节点内核精确匹配） |
| `kmod-mac80211_6.6.159.6.12.96-r4_x86_64.ipk` | 386KB | 同批重编 |

## 4. 装机与验证（x86 节点）

```sh
# 传输安装（vermagic 必须与节点内核完全一致，种子镜像同树编译故直接可用）
scp bin/targets/x86/64/packages/kmod-rtl8188ee*.ipk root@192.168.40.232:/tmp/
ssh root@192.168.40.232 'opkg install /tmp/kmod-rtl8188ee*.ipk && rm /tmp/*.ipk'

# 验证三步
dmesg | grep -iE "rtl8188|rtlwifi"        # ① 驱动识别 PCIe 卡并注册 phy
iw phy                                   # ② "Supported interface modes" 含 mesh point
iw dev phy0 set channel 6                # ③ 能操作 2.4G 信道
```

之后即可在控制器向导对 x86 节点下发 mesh（band 选 **2g**——本卡仅 2.4G），
或经 CONFIG 通道推送，由节点侧 netconf 事务落 wireless 配置。

## 5. 已知限制与维护注意

- **性能预期**：RTL8188EE 为 1T1R 802.11n（最高 72.2Mbps@HT20），做 2.4G 无线回程仅够功能验证与小流量演示；回程性能测试应依赖有线 + MT7981 的 5G mesh。
- **改动未提交**：三处 diff 目前只存在于构建树工作区。建议按以下顺序固化：
  1. `git add -p package/kernel/mac80211/Makefile target/linux/x86/config-6.6` 提交到本地分支（如 `x86-rtl8188ee`），并在提交信息里链接本文档；
  2. 上游（openwrt/openwrt master）已收录部分 rtlwifi 型号，后续升级时先核对是否已被官方收编，避免重复携带 patch。
- **内核升级**：kernel bump 后 vermagic 变化，kmod 必须随树重编（这正是自编固件的意义）；`CRYPTO_CMAC=m` 行要随 target config 一起保留。
- **23.05 注意**：本修改基于 24.10（内核 6.6 + backport 6.12.96）；如需 23.05（5.15）树，rtlwifi 路径与 Kconfig 宏相同，但需核对 backport 版本是否含该目录。

## 6. 与 OpenMesh 项目的关系

x86 节点是 V0.2 双机联调的第二节点（此前所有"待双机"边界的解锁钥匙）。
本驱动工作让该节点从"纯有线"升级为"有线+无线回程均可测"，
V0.2 出口条件中的 72h 稳测与混合回程验证自此具备完整硬件条件。
节点台账见 `Build_OpenWrt/DEVICE-CREDENTIALS.md`（仓库外，勿入库）。
