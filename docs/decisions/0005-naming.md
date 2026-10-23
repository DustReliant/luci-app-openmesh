# ADR-0005 命名与商标声明

- 状态：已接受（2026-10-23）
- 影响版本：全程

## 问题

1. **open-mesh.org** 是 batman-adv 官方组织（本项目依赖的 batman-adv 包即从 downloads.open-mesh.org 下载，本地 feeds/routing 已实证）——「OpenMesh」与之直接撞名
2. **OpenMesh Inc.**（Ruckus 旗下）为商业公司名
3. **EasyMesh®** 是 Wi-Fi Alliance 注册商标（TR-369）；本项目 802.11k/v/r 目标 ≠ EasyMesh 认证
4. 仓库名 `luci-app-openmesh` 暗示「仅一个 LuCI 应用」，与「完整系统（daemon + LuCI + 协议）」定位不符

## 决策

- 展示名 **OpenMesh Manager**；包名 `openmeshd` / `luci-app-openmesh`
- README 首屏放置免责声明：与 open-mesh.org、OpenMesh Inc.、Wi-Fi Alliance EasyMesh® 均无关；不宣称 EasyMesh 认证
- 全文档/代码禁用「EasyMesh」指代本项目组网能力，一律说 802.11s/batman-adv
- **待 Owner 决定**：仓库是否更名 `openmesh-manager`（更名成本现在为零——仓库为空）

## 后果

- 若后续进入 OpenWrt 官方 feeds，撞名可能引发 package 命名审查，提前声明可减少摩擦
