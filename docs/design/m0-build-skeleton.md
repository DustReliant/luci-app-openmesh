# M0 仓库与构建骨架

状态：已交付（分支 feat/m0-build-skeleton，合并回 main）

## 开工前 10 问

1. **解决什么问题**：让两个包（openmeshd / luci-app-openmesh）能在标准 OpenWrt 构建树中产出 ipk，并让每次 PR 都在 23.05/24.10/master 三版本 × x86_64/filogic 双架构上编译验证。没有这一步，后续所有模块没有可信的编译基线。
2. **OpenWrt 原生能力**：构建系统本身就是原生能力——`package.mk` 打包 C 程序、`feeds/luci/luci.mk` 打包 LuCI 应用。本模块完全复用，不引入任何自建构建系统。
3. **可复用项目**：包结构模式取自本树已验证的 `package/luci-app-wizard`（第三方现代 JS 应用，`include $(TOPDIR)/feeds/luci/luci.mk` + htdocs/root/po 布局）；C 包模式参照 `feeds/routing/mesh11sd` 的极简 Makefile。
4. **为什么自己开发**：骨架本身无现成可用模板可整仓复制（模板散在各包里），组装是必要工作，非重复造轮子。
5. **模块交互**：openmeshd 是被 luci-app-openmesh 依赖的底座（`LUCI_DEPENDS:=+openmeshd`）；M0 的 daemon 只是 uloop 心跳骨架，ubus 对象 M3 才注册。
6. **配置存储**：`/etc/config/openmesh` 仅 `main` section（enabled/role/mesh_name/端口/间隔），由包以 conffiles 交付；M0 不写更全的 schema，避免死配置。
7. **ubus API**：M0 无（`openmesh` 对象 M3 注册；ACL 文件先按最小读权限占位：status/health）。
8. **出错恢复**：init 脚本用 procd `respawn 3600 5 0`（阈值 1h 内无限重启）；daemon 自身 `-Wall -Wextra` 零警告���ulog→syslog；M0 尚无持久状态，崩溃即重启无恢复负担。
9. **测试**：DoD=(a) 24.10 filogic 工具链实际编译两 ipk；(b) `tests/smoke.sh` 断言 ipk 存在 + openmeshd 二进制 ≤150KiB；(c) CI 工作流三版本矩阵（首跑在 GitHub 上验证）。
10. **版本兼容**：C 代码仅依赖 libubox uloop/ulog（API 自 2015 年稳定，已从本树 libubox-2025.07.23 头文件核实签名）；luci.mk 布局 23.05/24.10/master 一致（wizard 同模式可证）；SDK 由 CI 按版本目录自动发现最新 patch（已验证 23.05.6/24.10.8/snapshot 均可解析）。

## 设计

```
仓库布局（M0 交付部分）
├── openmeshd/
│   ├── Makefile                 # package.mk，C 包
│   ├── src/openmeshd.c          # uloop + 30s 心跳骨架（M2 起逐模块填充）
│   └── files/etc/{config/openmesh, init.d/openmeshd}   # procd + 默认 UCI
├── luci-app-openmesh/
│   ├── Makefile                 # luci.mk，LUCI_DEPENDS:=+openmeshd
│   ├── htdocs/.../openmesh/overview.js   # 占位视图（M6 换真实现）
│   └── root/usr/share/{luci/menu.d, rpcd/acl.d}/...    # 菜单 + 最小 ACL
├── scripts/{setup-dev.sh, build.sh, ci-sdk-build.sh}
├── tests/smoke.sh               # ipk 存在 + 体积预算断言
└── .github/workflows/ci.yml     # 6 作业矩阵（3 版本 × 2 架构）
```

- 关键决策：luci-app 不用旧式 Lua（openclash 模式），用 wizard 的现代 JS 模式（luci.mk + menu.d），与 LuCI 主线一致。
- CI 用官方 SDK 而非完整源码树（省 30–60 分钟工具链构建）；SDK 文件名由目录索引动态发现，不硬编码 patch 号。
- License 决定：**GPL-3.0-or-later**（仓库根 LICENSE + 各文件 SPDX 头；与 LGPL-2.1 的 libubox 动态链接兼容）。

## 待验证项

| 项 | 方法 | 结果 |
|---|---|---|
| libubox ulog/uloop 签名 | 读 build_dir 头文件 | ✅ `ulog_open(channels,facility,ident)`、`ULOG_SYSLOG/STDIO` 为 enum |
| SDK URL 发现 | curl 目录索引 | ✅ 23.05.6 / 24.10.8 / snapshots(xz+zst) |
| luci.mk 第三方引用 | wizard 同模式在本树可编 | ✅ |
| CI 工作流本身 | 推送后 GitHub 首跑 | ⏳ 待首跑 |

## 实现后回填

24.10 filogic 树（AX3000T 工具链 gcc-13.3.0）实际编译验证通过，产出：

- `bin/packages/aarch64_cortex-a53/base/openmeshd_0.0.1-r1_aarch64_cortex-a53.ipk`（二进制 65499B ≤ 150KiB 预算）
- `bin/packages/aarch64_cortex-a53/base/luci-app-openmesh_0_all.ipk`（menu.d + rpcd ACL + 视图齐备）
- `tests/smoke.sh` 断言通过

实现期踩坑（后续模块全部适用，已固化进 build.sh 注释）：

| 坑 | 原因与修复 |
|---|---|
| 包未选中时 `make package/x/compile` 静默空跑（"Nothing to be done"） | 必须先 `CONFIG_PACKAGE_x=y` + `make defconfig`；未选中的包连 Build/Prepare 都不执行 |
| `uloop.h: field 'orig' has incomplete type` | libubox 的 uloop.h 不自带 `signal.h`；且 `-std=c11` 严格模式下 musl 隐藏 `struct sigaction`。修复：源文件顶部 `#define _POSIX_C_SOURCE 200809L` + `#include <signal.h>`（M1 起所有 C 文件沿用此文件头约定） |
| 改源码后重编译不生效 | `Build/Prepare` 的 $(CP) 受 stamp 保护，不追踪本地源码修改；改 src 后必须 `make package/x/clean` |
| ipk 用 `ar` 解析报 file format not recognized | OpenWrt ipk 实为外层 tar.gz（内含 data.tar.gz/control.tar.gz，见 `scripts/ipkg-build`），smoke.sh 已兼容 tar.gz 与 ar 两种格式 |
| 本地包 ipk 落在 `bin/packages/<arch>/base/` 而非 `packages/` | smoke.sh 改为全 bin/packages 递归查找 |

遗留：CI 工作流需推送后 GitHub 首跑验证（矩阵 URL 发现逻辑已本地 curl 验证过）。
