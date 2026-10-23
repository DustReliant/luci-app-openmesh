# 01 项目调研计划

> 规则：每项调研的结论必须附证据（源码路径 / commit / 实测输出），禁止只写印象。
> 产出文件放在本目录，命名即条目名。调研是 M0–M2 的并行背景任务，不阻塞 M0。

## 已核实事实（2026-10-23，本地 openwrt-24.10 树，可直接引用）

| 事实 | 证据位置 |
|---|---|
| mesh11sd 在 feeds/routing | `feeds/routing/mesh11sd/` |
| batman-adv 2024.3 在 feeds/routing，上游 open-mesh.org | `feeds/routing/batman-adv/Makefile` |
| mbedtls 3.6.7 | `package/libs/mbedtls/Makefile` |
| hostapd 含 ubus 补丁 | `package/network/services/hostapd/patches/600-ubus_support.patch` |
| luci-base JS API 齐全（rpc/uci/form） | `feeds/luci/modules/luci-base/htdocs/luci-static/resources/` |
| wpad-basic **不含** mesh | `.config` 中 wpad 变体清��� |
| umdns、lldpd 存在于 base 树 | `package/network/services/{umdns,lldpd}` |
| 竞品 luci-app-easymesh 多个同名仓库（GitHub 搜索确认） | research/luci-app-easymesh.md（待补证据链接） |

## 调研条目与必答问题

### 1. mesh11sd（产出：`mesh11sd.md`）
- UCI 全量选项清单（`/etc/config/mesh11sd`）与默认值？
- 能力边界：能管什么、不能管什么（多 radio？漫游？加密？）
- 与原生 wireless UCI 静态 11s 的关系/冲突（我们 V0.1 用静态，V0.2 切换时如何共存）
- 上游活跃度、Issue 里的坑

### 2. luci-app-easymesh 系列（产出：`luci-app-easymesh.md`）
- 各同名仓库分别怎么实现（UCI 生成器？脚本？）
- 确认空白点：无发现/无领养/无集中状态 —— 我们的差异化实证
- 可借鉴之处（UCI 模板、参数集）

### 3. OMM / and-elf（产出：`omm.md`）
- 功能与架构（Go daemon + mDNS + mTLS + PWA 前端）
- 已知不足与用户反馈（Issue 区）
- 对我们的启示：哪些设计要吸收，哪些坑要绕开（如 Go 运行时占用）

### 4. OpenWrt 版本兼容采样（产出：`openwrt-versions.md`）
- hostapd ubus 方法名清单（从 600-ubus_support.patch 摘录 + 真机 `ubus list hostapd.*`）
- `iwinfo --json` 在 23.05/24.10 的输出格式（真机采样存档）
- `ubus call network.wireless status` / `network.device status` 返回结构采样
- 802.11k/v/r 的 hostapd UCI 选项可用集（V0.3 前完成即可）

### 5. 加密库验证（产出：结论回填 ADR-0004）
- 按 ADR-0004 §M1 定夺方法执行

## 结论模板

```markdown
# <对象>
## 结论（3–5 条）
## 对本项目的复用决策
## 证据（路径/链接/命令输出）
```
