# 任务 6：迁移 HA 与 HA x Master 来源统计

## 状态

已实现，待提交。

## 实现

- `TmRingHomeAgent` 只保留 `TmRingHaPmuPort`；删除本地统计成员、getter、`record_read()` 与清零接口。
- `accept_read()` 的每个返回路径恰好记录一个 `read_admission` outcome；merge 继续累计原有 `rd_requests`、有效字节与各 merge bucket，bypass 不计 HA 读流量。
- 写预留失败、后端读提交、功能读提交、私有 L2 满、completion buffer 采样与事务擦除前均改为对应 PMU 事件。
- Fabric 每 target 注册一个 HA Port，并把同一可复制 Port 交给 MemPort 和（启用时）HomeAgent；HA 关闭时 MemPort 仍通过该 Port 记录来源请求。
- MemPort 删除来源统计 owner/getter/clear；仅在本地请求 FIFO push 成功并 pop NodeInterface 后记录 `source_request_received()`。
- Perf、Demo 与多 Ring 测试从同一个 PMU snapshot 读取 HA total/source；`PERF_HA_SOURCE` 输出格式与 keys 未改。
- 直接构造 HA 的测试接入 PMU，并对 merge、functional completion、waiter bucket 与 bypass 增加 snapshot 断言。

## 验证

- `git diff --check`：通过。
- 静态 branch mapping：8 个 `accept_read()` return 全部紧邻且仅有一个 outcome 调用；四类 stall、三类 merge、hit/miss、bypass 齐全。
- 静态旧 owner/API 扫描：HomeAgent/MemPort 内无 `stats_`、`record_read`、HA getter、HA source owner 或对应 clear。
- 静态 Fabric 核对：每个 target 一次 `register_home_agent(i, cfg_->num_masters)`，Port 无条件传入 MemPort。

## 未运行

- `../build/exe/test_prj --gtest_filter='TmRingPmuTest.*:TmRingHomeAgentTest.*:TmRingMultiRingFabricTest.*:TmRingPerfReportTest.EmitsHomeAgentRequestSourcesByMasterAndCommand'`：未运行；当前 worktree、`code/build` 与 `aicore/build` 均无 `exe/test_prj`，因此未编译、gtest 未执行。

## 范围

- 未迁移 L2 Buffer 统计；保留其既有 `clear_stats()` 行为。

## 审查

- 独立静态审查无 P0。审查提出的 `reserve_write()` 失败后预留残留为基线既有功能行为；本任务仅迁移该失败事件计数，未改变功能语义。
- 审查提出的 L2 非 PMU snapshot 不纳入本任务：计划明确“不做 L2 迁移”，本任务要求的 HA total 与 HA x Master sources 均从同一个 `ring_pmu.ha` snapshot 读取。
- 工作树已有两个 `tools/__pycache__/*.pyc` 修改，未纳入本任务提交。
