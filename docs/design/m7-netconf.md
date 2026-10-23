# M7 配置事务与 Mesh 创建/加入

状态：已交付（r9，真机 DRILL-OK）

## 开工前 10 问（摘要）

1. **解决**：一键创建 802.11s Mesh（目标 4/5/6）+ 配置安全网（目标 18 的地基）。
2. **原生能力**：wireless UCI 的 `mode 'mesh'` + SAE 是 OpenWrt 原生 802.11s 配置（ADR-0006），netifd/hostapd(wpad) 生效；我们只做编排与事务。
3. **复用**：libuci 全程；reload 走 `ubus call network reload`。
4. **自研部分**：netconf 事务引擎（快照/标记/确认窗/自动回滚/掉电恢复）——OpenWrt 无此通用机制（LuCI 的无线安全模式是前端实现）。
5. **交互**：ubus mesh_create/mesh_join/apply_confirm/apply_rollback → netconf；控制器经会话通道 CONFIG 加密帧推送 mesh 参数给在线 agent；agent 应用后以"持续收到控制器加密流量"为自动确认条件，30s 无流量即回滚。
6. **存储**：mesh 名/密钥/频段 → `/etc/config/openmesh` 的 `backhaul` 段；事务快照与 pending 标记在状态目录（flash，掉电后启动恢复）。
7. **ubus**：见 5；事件 `openmesh.config.applied/rolledback`。
8. **恢复**：30s 确认窗 + 自动回滚 + 掉电启动恢复（未确认标记 → 还原快照）；回滚失败时明确报错留人工处置（绝不静默）。
9. **测试**：tests/mesh-drill.sh 真机四段演练（应用→自动回滚→确认保留→恢复原状）。
10. **兼容**：libuci/netifd 接口三版本稳定；802.11s 需 wpad-mesh（seed 已改）。

## 实现后回填

- 真机 DRILL-OK：mesh 接口真实出现（SAE 加密）→ 30s 不确认自动回滚（接口消失、配置还原）→ 确认保留 → 清理恢复原状。
- 修复 gen_key 弱密钥 bug（初版每字符复用 3 个随机源导致 "yvkyvkyvk" 重复模式——演练输出直接暴露，已改逐字节随机取模）。
- 已知边界（记录）：双机 11s 互通验证待第二台设备（单机已验证接口/回滚/确认全链路）；agent 侧 CONFIG 应用代码路径完整但同样待双机场景实测。
