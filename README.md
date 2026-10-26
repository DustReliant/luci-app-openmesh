# OpenMesh Manager

> OpenWrt Mesh 编排与管理系统 —— 让多台 OpenWrt 路由器像商业 Mesh 产品一样简单易用。
>
> **当前状态：V0.2 RC1（tag `v0.2.0-rc1`），双机 802.11s + BATMAN-ADV 真机运行中。**

## 功能总览

### 网络管理闭环
- ✅ 节点自动发现（UDP 签名广播，Ed25519 防伪造/防重放）
- ✅ 节点授权领养（加密会话通道：X25519 ECDH + ChaCha20-Poly1305）
- ✅ 已领养节点持久化（UCI）与重启恢复
- ✅ 一键创建/加入 802.11s Mesh（原生 UCI 配置，SAE 加密）
- ✅ hybrid 模式（802.11s + batman-adv）双机自动建立
- ✅ 配置事务安全网：30 秒不确认自动回滚（含掉电恢复）
- ✅ 已提交配置受回滚保护（快照感知清理）

### 交互式拓扑管理
- ✅ SVG 交互画布：节点拖拽 / 画布平移 / 滚轮+双指缩放
- ✅ 商业级节点卡片（180×85，状态圆点，白底圆角+阴影）
- ✅ 双链路曲线分离（802.11s 虚线 / BATMAN 点线，贝塞尔曲线+标签底板）
- ✅ BFS 智能布局（星形/链式/树形/环形/孤立节点自适应）
- ✅ 节点/链路点击详情面板（分组展示）
- ✅ 布局持久化（localStorage，按控制器 node_id 网络隔离）
- ✅ 5 秒实时刷新（新节点自动加入，已有节点位置不变）
- ✅ 采集健康指示（ok/stale/failed，与节点在线状态解耦）
- ✅ 移动端适配（Pointer Events 统一鼠标+触摸，响应式布局）

### 监控与数据
- ✅ 客户端列表（AP 关联设备 MAC/SSID/RSSI/速率）
- ✅ Mesh peer 监控（iwinfo 解析 RSSI）
- ✅ BATMAN originator 监控（TQ、接口、多跳过滤）
- ✅ 漫游增强 802.11k/v/r（向导一键推送）
- ✅ 72h 稳定性采样（CSV 输出）

## 系统架构

```
LuCI Web 界面（交互式拓扑 + 节点管理 + 向导）
        ↓
ubus RPC（openmesh 对象：13 个方法）
        ↓
openmeshd 守护进程（C，libubox/libubus/libuci/libsodium）
  ├── identity    身份与密钥（Ed25519）
  ├── discovery   UDP 发现（签名 announce）
  ├── session_net 会话通道（ECDH + 加密帧）
  ├── netconf     配置事务（快照/确认/回滚）
  ├── monitor     数据采集（mesh peer + batman + AP 客户端）
  ├── batman_collect  batctl 执行层（可配超时）
  ├── batman_parse    batctl 解析层（纯函数，零依赖）
  ├── topology    统一 Node/Link 模型
  └── store       UCI 持久化
        ↓
UCI / netifd / hostapd / iw / batctl
        ↓
802.11s / BATMAN-ADV / Ethernet
```

## 快速开始

### 安装

```sh
# 从源码构建（需要 OpenWrt 24.10 SDK）
./scripts/setup-dev.sh /path/to/openwrt
./scripts/build.sh /path/to/openwrt

# 或直接安装预编译包
opkg install openmeshd_*.ipk luci-app-openmesh_*.ipk
```

### 使用

1. **控制器**：安装 openmeshd + luci-app-openmesh，打开 LuCI → 网络 → OpenMesh
2. **代理节点**：安装 openmeshd，配置 `uci set openmesh.main.role='agent'` 和 `controller_addr`
3. **建网**：控制器 LuCI → Mesh 向导 → 创建 Mesh（默认 2.4GHz + SAE）
4. **拓扑**：控制器 LuCI → 拓扑 → 查看实时网络拓扑

### ubus API

```sh
ubus call openmesh status          # 节点状态
ubus call openmesh list_adopted    # 已领养节点
ubus call openmesh get_links       # 回程链路（含采集健康）
ubus call openmesh get_clients     # AP 客户端
ubus call openmesh get_mesh_info   # Mesh 配置信息
ubus call openmesh topology        # 统一拓扑快照
ubus call openmesh mesh_create     # 创建 Mesh（事务）
ubus call openmesh apply_confirm   # 确认配置
ubus call openmesh adopt           # 领养节点
```

## 项目结构

```
luci-app-openmesh/       # LuCI Web 管理界面
  htdocs/.../topology.js #   交互式拓扑画布
  htdocs/.../overview.js #   节点/客户端列表
  htdocs/.../wizard.js   #   建网向导
openmeshd/               # 核心后台 daemon（C）
  src/identity.c         #   Ed25519 身份
  src/discovery.c        #   UDP 发现
  src/session_net.c      #   加密会话
  src/netconf.c          #   配置事务
  src/monitor.c          #   数据采集
  src/batman_parse.c     #   batctl 纯解析
  src/batman_collect.c   #   batctl 执行层
  src/topology.c         #   Node/Link 统一模型
docs/                    # 设计文档（ADR + 模块设计）
scripts/                 # 构建辅助 + OpenWrt 补丁
tests/                   # 自动化测试
```

## 测试

```sh
# 宿主机全套测试（无 OpenWrt SDK 依赖）
./tests/host-build.sh
# identity 28 + session 35 + batman 18 + topology 22 + discovery 213 断言

# 前端测试（Node.js）
node tests/test_topology_frontend.js   # 布局持久化+安全 23 项
node tests/test_phase62.js             # 智能布局+双链路 22 项

# 命令层测试（假 batctl）
./tests/test_batman_cmdlayer.sh

# 真机部署后冒烟
./tests/smoke.sh /path/to/sdk
```

## 兼容性

| 维度 | 目标 |
|---|---|
| OpenWrt 版本 | 23.05 / 24.10 / 25.x（CI 三版本矩阵） |
| 硬件 | x86_64、MT7981（AX3000T）、MT7986 |
| 浏览器 | Chrome/Edge/Firefox 桌面 + Android Chrome（Pointer Events） |

## 已验证真机环境

- 控制器：小米 AX3000T（MT7981，24.10-SNAPSHOT）
- 代理：x86 迷你主机（RTL8188EE 2.4G，24.10 种子镜像）
- 链路：802.11s SAE 加密（RSSI -26dBm）+ BATMAN-ADV（TQ 230）
- 72 小时稳定性测试进行中

## 命名与商标声明

本项目与以下组织/标准**无关**：

- `open-mesh.org`（batman-adv 官方社区）
- OpenMesh Inc.（Ruckus 旗下商业公司）
- Wi-Fi Alliance `EasyMesh®`（TR-369 标准，注册商标）

本项目不宣称 EasyMesh 认证兼容；802.11k/v/r 支持不等于 EasyMesh。

## License

GPL-3.0-or-later（依赖 libsodium 为 ISC、libubox/libubus 为 LGPL-2.1）

## 文档索引

| 文档 | 内容 |
|---|---|
| [workflow.md](docs/workflow.md) | 开发工作流规范 |
| [architecture.md](docs/architecture.md) | 总体架构 |
| [roadmap.md](docs/roadmap.md) | 版本路线图 |
| [v0.2-plan.md](docs/design/v0.2-plan.md) | V0.2 里程碑 + 真机验证记录 |
| [m18-topology-model.md](docs/design/m18-topology-model.md) | 统一拓扑模型设计 |
| [scripts/openwrt-patches/](scripts/openwrt-patches/) | rtl8188ee 驱动补丁 + 自编译指南 |
| [ADR 列表](docs/decisions/) | 架构决策记录（8 篇） |
