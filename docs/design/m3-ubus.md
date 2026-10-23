# M3 daemon 骨架完善 + ubus API

状态：已交付（r5，真机验证：status/list_nodes/health 全通、事件经 ubus listen 验证、二实例发现可见）

## 开工前 10 问

1. **解决什么问题**：把 daemon 内部状态（身份/已发现节点）暴露给系统总线，LuCI（M6）和用户 CLI 可查询；新节点事件可被订阅。这是 LuCI 交互的唯一通道。
2. **OpenWrt 原生能力**：ubus + libubus + blobmsg 就是原生机制，全部复用；自建 RPC 不做。
3. **可复用项目**：无整块复用；libubus 对象注册模式照抄 rpcd 插件通例（blobmsg 表/数组 API）。
4. **为什么自己开发**：对象 schema 属本项目领域。
5. **模块交互**：ubus_api 持 identity+disc 引用（只读）；disc 的 on_new 回调同时发 ubus 事件 `openmesh.node.new`；M4 起 session/adopt 方法挂到同一对象。
6. **配置存储**：无新增配置；role 仍读 UCI。
7. **ubus API**（本次注册）：
   - `status {}` → `{version, role, node_id, uptime, nodes_total}`
   - `list_nodes {}` → `{nodes:[{id(hex64), role, model, fw, addr, first_seen, last_seen}]}`
   - `health {}` → `{ok, identity:"ok", discovery:"ok", ubus:"ok"}`
   - 事件：`openmesh.node.new`（node 同 list_nodes 元素）
   - ACL：luci-app-openmesh 的 read.ubus 增补 `list_nodes`
8. **出错恢复**：ubusd 不在/对象名被占（多实例共存场景）→ 记 WARN 继续运行（daemon 不依赖 ubus 存活）；方法内构造应答失败仅本调用失败。
9. **测试**：真机 `ubus call openmesh status|list_nodes|health`；手动二实例场景：B 实例 announce 单向发往 7689，系统实例 list_nodes 应出现 B。
10. **版本兼容**：libubus 对象/事件 API 23.05~master 稳定；`ubus send` 事件名带点号是惯例。

## 设计

- `ubus_api.c`：`om_ubus_init(u, id, disc)`（失败仅返回 -1，main 记 WARN 不退出）；方法表 blobmsg 构造。
- main 的 `on_new` 回调同时打日志 + 发事件；事件发送失败不影响发现逻辑。
- uptime 用 monotonic（clock_gettime CLOCK_MONOTONIC），避免 NTP 跳变。

## 实现后回填

（见模块报告；真机验证：status/list_nodes/health 全通，二实例发现可见，事件经 `ubus listen` 验证。）
