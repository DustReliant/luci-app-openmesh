# M5 状态持久化与重启恢复

��态：实现中（回填见文末）

## 开工前 10 问

1. **解决什么问题**：控制器重启/掉电后已领养节点列表不丢；agent 侧 M4 已落盘（network.key/controller.pin），本模块补控制器侧 adopted 白名单持久化。
2. **OpenWrt 原生能力**：UCI 就是原生持久化（/etc/config 在 overlay，重启保留）；libuci 提供编程接口（M2 已核实 API）。
3. **可复用项目**：无；按 architecture.md §9 的 schema（config node section）。
4. **为什么自己开发**：schema 属本项目。
5. **模块交互**：store.c 是 libuci 薄封装（adopted 增/改名/枚举）；session_net 在领养成功与改名时调用写入，snet_init 时加载。
6. **配置存储**：`/etc/config/openmesh` 匿名 `config node` 段：id(hex64)/pubkey(base64)/name。密钥仍走文件（M4 已定）。
7. **ubus API**：无新增（list_adopted 数据源变为"UCI 加载 + 会话状态刷新"）。
8. **出错恢复**：UCI 写失败 → 日志告警、内存态仍生效（降级为 M4 行为）；加载时坏记录（hex/base64 解析失败）跳过并告警，不阻塞启动。
9. **测试**：真机 persist-test.sh：adopt → 重启控制器 → adopted 列表仍在（offline）→ agent 回归 → online（WELCOME 路径）。
10. **版本兼容**：libuci API 三版本稳定；base64 用 libsodium（sodium_bin2base64/base642bin）。

## 实现后回填

（待填）

## 实现后回填

- 真机 e2e-m45.sh 全绿：领养(带名)→UCI 落盘→控制器重启列表保留→agent 无 join_open 回归→WELCOME 上线。
- 实现期三个教训（全部有据可查）：
  1. **`sodium_base642bin` 第 8 参是 variant 不是 flags**，传 NULL(0) 触发 `sodium_misuse()`→SIGABRT（隔离测试+头文件确认；也暴露了构建时 grep 过滤 warning 的坏习惯，已纠正）。
  2. **ubus 对象只应由控制器角色注册**：否则 agent 测试实例在系统 daemon 空窗期抢注 `openmesh` 对象，角色语义错乱（已改为 controller-only 注册，agent headless 纯日志，与架构一致）。
  3. busybox ash 的 `set -e` 两连坑：`[ cond ] && break` 语句级假值自杀、`VAR=$(cmd)` 命令替换失败状态码自杀——测试脚本统一改 if/|| true 写法。
- adopt{name} 现随 pending 传递，首次领养即生效（此前被丢弃）。
