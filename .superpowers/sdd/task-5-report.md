# 任务 5：迁移 RBRG 四路径统计

## 状态

已完成并提交：`d721a2dfad2541487a594958c358b32a9cc66796`

## 实现

- `TmRingRbrgL1` 仅保留强类型 `TmRingRbrgPmuPort`；删除旧统计成员、getter、清零接口和 `PathState::queue_occupancy`。
- 成功入 transfer queue 后记录 `enqueued`；队满、destination inject 拒绝和成功交付分别记录对应 PMU 事件；成功交付严格先 `pop_front()` 后 `delivered()`。
- Fabric 按每个 V-Ring 注册一次 RBRG PMU Port，并移除 RBRG 旧聚合/getter。
- Perf、Demo 与多 Ring 测试从单次 PMU snapshot 读取；Demo 使用 `instance_ids` 输出真实 RBRG ID，多 Ring 测试用 snapshot 的 ID lookup。

## 验证

- `git diff --check`：通过。
- 静态事件顺序核对：通过。
- 静态旧 Fabric RBRG API 扫描：通过。

## 未运行

- `../build/exe/test_prj --gtest_filter='TmRingMultiRingFabricTest.*:TmRingPmuTest.Rbrg*'`：未运行；当前 worktree 不存在 `../build/exe/test_prj`，因此 C++ 未编译、gtest 未执行。

## 疑虑

- 无已知源码接口冲突。运行时行为与四路径统计口径仍需在具备 Linux ESL/gtest 构建产物后验证。

## 审查修复：保留 RBRG 实例 ID

### 修复

- `TmRingPerfResult` 新增与 `rbrg_stats` 同序的 `rbrg_instance_ids`；collector 从同一次 `ring_pmu` snapshot 同步复制该列表。
- report 使用对应的真实实例 ID 输出 `PERF_RBRG_CHANNEL id=`，不再将 vector 下标误作 ID；PERF section、key 和各项公式均未改变。
- `TmRingPerfReportTest.EmitsMeasuredChannelAndBufferRecords` 构造非连续 ID `1,7`，断言两者均出现在输出中，且绝不输出 `id=0`。

### 验证

- `git diff --check`：通过。
- C++ gtest：未运行；worktree 与相邻 `build` 路径均不存在 `exe/test_prj`，因此未编译。
