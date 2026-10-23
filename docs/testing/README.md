# 06 测试策略

## 测试层级

| 层级 | 环境 | 覆盖 | 时机 |
|---|---|---|---|
| L1 单元 | 宿主机 x86_64 直接编译 daemon 模块（无 OpenWrt 依赖的部分抽离） | crypto/协议解析/状态机 | 每 commit |
| L2 loopback | 单机双进程（lo 上跑两个 openmeshd 实例，不同端口/目录） | 发现、会话、领养、replay 拒绝 | 每 commit |
| L3 集成 | QEMU x86_64 多 VM 挂同一 Linux bridge | 多节点拓扑、配置下发、回滚、心跳/离线 | 每 PR |
| L4 真机 | 2×Xiaomi AX3000T（MT7981）+ 1×MT7986 设备 | 无线实配、RSSI、漫游、72h 稳定性 | 里程碑出口 |

## CI（GitHub Actions）

- 矩阵：`{openwrt-23.05.x, openwrt-24.10.x, openwrt master snapshot}` × `{x86_64, mediatek/filogic}` SDK
- 每 PR：三端编译（`-Wall -Wextra` 零警告）+ 冒烟断言：
  - openmeshd 二进制 ≤ 150KB；luci-app-openmesh ipk ≤ 60KB
  - `ubus call openmesh status` 返回 JSON schema 校验
- master 定期夜跑 L3

## 关键场景用例（V0.1 出口前必须全绿）

1. 发现：A 启动 2s 内列出 B（unadopted）
2. 领养：PIN 流程完成，伪造 pubkey 被拒
3. 建网/入网：两节点 mesh0 出现、L2 互通、SSID 下发正确
4. 回滚：apply 后不 confirm，30s 自动恢复原配置
5. 崩溃：`kill -9` openmeshd，procd 重生，状态无损
6. 重启：整 机 reboot 后 adopted 列表与 mesh 配置完好
7. 恶意：重放旧 announce/会话帧被滑窗丢弃；未领养节点发配置消息被拒

## 真机注意事项

- 本地 24.10 seed 配置当前为 `wpad-basic-mbedtls`（**无 mesh 能力**），M0 起 seed 改为 `wpad-mesh-mbedtls`
- 真机测试脚本放 `scripts/`，输出存 `tests/artifacts/`（不提交大二进制）
