# ADR-0001 管理面采用 Controller/Agent 星型模型

- 状态：已接受（2026-10-23）
- 影响版本：V0.1 起

## 背景

多节点管理需要一致的集群视图。候选：全对等 P2P（每节点等权）、Controller/Agent 主从、Wi-Fi Alliance EasyMesh（TR-369）多控制器模型。

## 决策

管理面采用单 Controller + 多 Agent 星型拓扑；Agent 主动向 Controller 发起出站 TCP 长连接；数据面（11s/batman-adv）仍为节点对等，与管理面解耦。

## 理由

- 商业产品（Deco/AiMesh/华为）均为用户可理解的主从心智模型，与「简单易用」定位一致
- 全对等需要分布式一致性（成员表/配置版本收敛），复杂度远超收益，且 32 节点规模无此必要
- Agent 出站连接规避入站方向/NAT 问题
- 管理面单点故障通过 fail-static 缓解（管理面不承载转发），控制器接管留到 V0.4

## 后果

- LuCI 仅装在控制器；Agent 可 headless（省 flash）
- 协议需预留 generation/epoch 字段以便 V0.4 平滑接管
- 单控制器是已知限制，写入 README

## 被否决的替代方案

- 全对等 P2P：一致性复杂度不可接受
- TR-369 EasyMesh 多控制器：实现重、需认证栈，与资源限制冲突
