# OpenMesh Manager

> OpenWrt Mesh 编排与管理系统 —— 让多台 OpenWrt 路由器像商业 Mesh 产品一样简单易用。
>
> **当前状态：阶段 00（架构与工作流设计），尚无可安装产物。**

## 项目定位

在 OpenWrt **原生**网络组件之上，构建一层面向普通用户的「Mesh 编排与管理层」：

- 自动发现、授权认领、一键建网/入网
- 集中状态监控、拓扑展示、故障检测与恢复
- 有线/无线回程统一编排（数据面切换交给 802.11s / batman-adv 原生能力）

**明确不做的事**（技术红线，见 `docs/workflow.md`）：

- 不重新实现 802.11s / batman-adv / hostapd / netifd / ubus / UCI
- 不自研链路探测与回程切换逻辑（batman-adv metric 原生完成）
- 不修改 OpenWrt 核心代码

## 命名与商标声明

本项目与以下组织/标准**无关**：

- `open-mesh.org`（batman-adv 官方社区）
- OpenMesh Inc.（Ruckus 旗下商业公司）
- Wi-Fi Alliance `EasyMesh®`（TR-369 标准，注册商标）

本项目不宣称 EasyMesh 认证兼容；802.11k/v/r 支持不等于 EasyMesh。

## 兼容性与硬件目标

| 维度 | 目标 |
|---|---|
| OpenWrt 版本 | 23.05 / 24.10 / 25.x（每 PR 经 CI 三版本矩阵编译验证） |
| 硬件 | x86_64、MT7981、MT7986（第一阶段） |

## 仓库结构（规划）

```
luci-app-openmesh/   # LuCI Web 管理界面（仅控制器节点安装）
openmeshd/           # 核心后台 daemon（C，libubox/libubus/libuci）
docs/                # 设计文档（本目录为现阶段唯一产物）
scripts/             # 测试与部署辅助脚本
tests/               # 自动化测试
```

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/workflow.md](docs/workflow.md) | 开发工作流规范（阶段、循环、10 问、报告模板、Git 约定） |
| [docs/architecture.md](docs/architecture.md) | 总体架构 v1 草案（14 项：架构图/模块/daemon/LuCI/协议/数据结构/ubus/UCI/发现/认证/恢复/兼容/任务拆解） |
| [docs/roadmap.md](docs/roadmap.md) | 版本路线图（V0.1–V1.0）与 V0.1 任务拆解（M0–M8 + DoD） |
| [docs/decisions/](docs/decisions/) | 架构决策记录（ADR） |
| [docs/research/README.md](docs/research/README.md) | 调研计划（mesh11sd / easymesh 系 / OMM / 版本兼容性） |
| [docs/design/README.md](docs/design/README.md) | 模块设计文档规范与模板 |
| [docs/testing/README.md](docs/testing/README.md) | 测试策略（CI 矩阵 / 单测 / QEMU 多节点 / 真机） |

## License

待定（M0 决定，倾向 GPL-3.0 或 MPL-2.0；依赖 libsodium 为 ISC、libubox/libubus 为 LGPL-2.1，需一并审视）。
