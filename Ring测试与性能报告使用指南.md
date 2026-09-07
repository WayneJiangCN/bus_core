# Ring 测试与性能报告使用指南

本文说明如何运行 Ring Google Test、保存测试日志、生成 HTML 性能报告，以及生成 L2 命中率、Outstanding 和 Burst 扫描图。命令以 Linux 工程目录为例：

```text
~/wayne/model_training/
├── build/exe/test_prj
├── src/aicore/run_ring_test.sh
├── src/aicore/tools/
└── sim/
```

如果实际目录不同，只需替换命令中的相对路径。

## 1. 快速运行

### 1.1 运行性能测试

`run_ring_test.sh` 内部固定执行当前目录下的 `./test_prj`，因此应先进入可执行文件所在目录：

```bash
cd ~/wayne/model_training/build/exe

bash ../../src/aicore/run_ring_test.sh \
  --gtest_filter='RingPerfBenchmark.*:RingL2HitRateSweep.*:RingOutstandingSweep.*:RingBurstSweep.*'
```

测试结束后生成两份日志：

```text
logs/full/<时间戳>.log
logs/filtered/<时间戳>.log
```

### 1.2 生成 HTML 报告

```bash
latest=$(ls -t logs/full/*.log | head -n 1)

python3 ../../src/aicore/tools/generate_ring_perf_report.py \
  "$latest" ring_perf_report.html
```

生成文件：

```text
~/wayne/model_training/build/exe/ring_perf_report.html
```

可在有图形界面的 Linux 环境直接打开：

```bash
xdg-open ring_perf_report.html
```

也可以启动临时只读访问入口：

```bash
python3 -m http.server 8000 --bind 127.0.0.1
```

浏览器访问：

```text
http://127.0.0.1:8000/ring_perf_report.html
```

远程服务器需要配合 SSH 端口转发，或者将 HTML 下载到本机后打开。

## 2. 测试程序指令

### 2.1 查看所有测试

```bash
cd ~/wayne/model_training/build/exe
./test_prj --gtest_list_tests
```

作用：只列出测试套件和测试名称，不运行仿真。

### 2.2 运行全部测试

```bash
./test_prj
```

作用：运行编译进 `test_prj` 的全部启用测试。该方式适合最终回归，但耗时通常最长。

### 2.3 运行一个测试套件

```bash
./test_prj --gtest_filter='RingPerfBenchmark.*'
```

作用：运行 `RingPerfBenchmark` 套件中的全部测试。

### 2.4 运行一个测试用例

```bash
./test_prj \
  --gtest_filter='RingPerfBenchmark.MultiVringPrivateRead128B'
```

作用：只运行指定用例，适合定位单个场景。

### 2.5 同时运行多个测试套件

```bash
./test_prj \
  --gtest_filter='RingPerfBenchmark.*:RingOutstandingSweep.*'
```

作用：冒号 `:` 表示多个匹配条件的并集。

### 2.6 排除部分测试

```bash
./test_prj \
  --gtest_filter='RingPerfBenchmark.*-*Write*'
```

作用：运行 `RingPerfBenchmark`，但排除名称中包含 `Write` 的测试。减号前是包含条件，减号后是排除条件。

### 2.7 常用 Google Test 参数

| 参数 | 作用 |
|---|---|
| `--gtest_list_tests` | 列出测试，不执行 |
| `--gtest_filter='Suite.Test'` | 运行一个测试 |
| `--gtest_filter='Suite.*'` | 运行一个测试套件 |
| `:` | 在 filter 中连接多个包含条件 |
| `-` | 在 filter 中分隔包含条件和排除条件 |
| `*` | 匹配任意长度的测试名称 |
| `?` | 匹配测试名称中的单个字符 |
| `--gtest_repeat=3` | 将所选测试连续运行 3 次 |
| `--gtest_shuffle` | 打乱测试执行顺序，用于发现顺序依赖 |
| `--gtest_break_on_failure` | 首次断言失败时立即中断，便于调试 |

filter 建议始终使用单引号包围，防止 Shell 将 `*` 当成文件通配符展开。

## 3. `run_ring_test.sh` 的作用

推荐使用脚本而不是直接运行 `test_prj`，因为脚本会同时保存完整日志和适合终端阅读的精简日志。

```bash
bash ../../src/aicore/run_ring_test.sh \
  --gtest_filter='RingPerfBenchmark.*'
```

脚本按以下顺序工作：

1. 检查脚本是否由 `bash` 正常执行，禁止使用 `source`；
2. 创建 `logs/full` 和 `logs/filtered`；
3. 用当前时间生成唯一日志名；
4. 执行 `./test_prj "$@"`，将命令行参数原样传给 Google Test；
5. 使用 `tee` 将全部输出保存到完整日志；
6. 使用 `awk` 从终端输出中隐藏大量逐 Buffer、逐 Edge 和 HA 来源记录；
7. 将过滤后的输出保存到精简日志；
8. 使用 `PIPESTATUS[0]` 返回 `test_prj` 的真实退出码。

### 3.1 两种日志的区别

| 日志 | 内容 | 主要用途 |
|---|---|---|
| `logs/full/*.log` | 保留全部 Google Test 和 `PERF_*` 记录 | 生成 HTML、生成扫参图、详细分析 |
| `logs/filtered/*.log` | 隐藏 `PERF_RING_BUFFER`、`PERF_RING_EDGE`、`PERF_HA_SOURCE` 明细 | 查看测试通过情况和主要性能结果 |

生成完整 HTML 报告时必须使用 `logs/full`。使用过滤日志会缺少逐节点队列、逐链路和 HA 请求来源数据。

### 3.2 为什么不能使用 `source`

错误方式：

```bash
source ../../src/aicore/run_ring_test.sh
```

`source` 会让脚本在当前 Shell 内执行，可能改变调用者的变量和退出行为。脚本对此进行了主动检查，应使用：

```bash
bash ../../src/aicore/run_ring_test.sh
```

## 4. 性能测试套件

| 测试套件 | 作用 | 主要输出 |
|---|---|---|
| `RingPerfBenchmark.*` | 运行固定拓扑下的主要读写与聚合场景 | 带宽、延迟、队列、链路、HA/L2 和 RBRG 指标 |
| `RingL2HitRateSweep.*` | 扫描 L2 命中率 | 有效带宽、HBM 带宽和 P99 延迟随命中率变化 |
| `RingOutstandingSweep.*` | 扫描每个 Master 的 Outstanding | 并发度、带宽、延迟、后端压力和阻塞关系 |
| `RingBurstSweep.*` | 扫描请求大小或 Burst 长度 | 请求粒度对包数量、DAT/HBM 流量和性能的影响 |

推荐先运行一个固定场景确认功能，再执行完整扫描：

```bash
bash ../../src/aicore/run_ring_test.sh \
  --gtest_filter='RingPerfBenchmark.MultiVringPrivateRead128B'
```

## 5. HTML 性能报告

### 5.1 基本命令

```bash
python3 ../../src/aicore/tools/generate_ring_perf_report.py \
  logs/full/20260907_120000.log \
  ring_perf_report.html
```

三个位置参数分别表示：

| 参数 | 作用 |
|---|---|
| `python3` | 使用 Python 3 解释器运行脚本 |
| `generate_ring_perf_report.py` | 解析 `PERF_*` 协议并生成离线 HTML |
| `logs/full/20260907_120000.log` | 输入的完整测试日志 |
| `ring_perf_report.html` | 输出文件；省略时默认与输入日志同名、扩展名改为 `.html` |

例如省略输出路径：

```bash
python3 ../../src/aicore/tools/generate_ring_perf_report.py \
  logs/full/20260907_120000.log
```

将生成：

```text
logs/full/20260907_120000.html
```

### 5.2 HTML 中的主要内容

- 各场景端到端带宽和稳定响应带宽；
- P50、P95、P99 和最大延迟；
- H-Ring、V-Ring、REQ、RSP、DAT 方向利用率；
- CW/CCW 不均衡程度和最热链路；
- 各节点 Inject/Eject Queue 的包数、峰值、满占比和拒绝次数；
- RBRG 四条路径的包数、字节数、忙周期和阻塞；
- HA 请求来源、合并率、L2 hit/miss 和后端访问量；
- 拓扑图及节点、Ring、subnet 下钻视图。

## 6. 扫参图生成

扫参脚本读取包含对应 `PERF_SWEEP` 记录的完整日志，并输出 CSV 和 PNG。绘图依赖 Python 的 `matplotlib`。

### 6.1 L2 命中率扫描

先运行：

```bash
bash ../../src/aicore/run_ring_test.sh \
  --gtest_filter='RingL2HitRateSweep.*'
```

再生成图表：

```bash
python3 ../../src/aicore/tools/plot_l2_sweep.py \
  logs/full/<时间戳>.log \
  -o l2_sweep_charts
```

主要输出：

```text
l2_sweep_charts/l2_sweep_summary.csv
l2_sweep_charts/l2_hit_rate_vs_e2e.png
l2_sweep_charts/l2_hit_rate_vs_hbm.png
l2_sweep_charts/l2_hit_rate_vs_p99.png
```

### 6.2 Outstanding 扫描

```bash
bash ../../src/aicore/run_ring_test.sh \
  --gtest_filter='RingOutstandingSweep.*'

python3 ../../src/aicore/tools/plot_outstanding_sweep.py \
  logs/full/<时间戳>.log \
  -o outstanding_charts
```

主要输出：

```text
outstanding_charts/outstanding_sweep_summary.csv
outstanding_charts/outstanding_vs_bandwidth.png
outstanding_charts/outstanding_vs_p99.png
outstanding_charts/outstanding_vs_resource_pressure.png
```

### 6.3 Burst 扫描

```bash
bash ../../src/aicore/run_ring_test.sh \
  --gtest_filter='RingBurstSweep.*'

python3 ../../src/aicore/tools/plot_burst_sweep.py \
  logs/full/<时间戳>.log \
  -o burst_charts
```

主要输出：

```text
burst_charts/burst_sweep_summary.csv
burst_charts/request_bytes_vs_bandwidth.png
burst_charts/request_bytes_vs_hbm.png
burst_charts/request_bytes_vs_p99.png
burst_charts/request_bytes_vs_packet_counts.png
```

## 7. Shell 指令说明

| 指令或符号 | 作用 |
|---|---|
| `cd <目录>` | 切换当前工作目录 |
| `./test_prj` | 执行当前目录中的测试程序 |
| `bash <脚本>` | 使用 Bash 启动脚本，不要求脚本具有可执行权限 |
| `../../` | 从当前目录向上返回两级 |
| `\` | Shell 行尾续行符，将下一行视为同一条命令 |
| `'...'` | 禁止 Shell 展开其中的 `*`、`?` 等字符 |
| `"$latest"` | 读取变量并保留路径中的空格 |
| `2>&1` | 将错误输出合并到标准输出，保证日志完整 |
| `tee <文件>` | 一边继续输出，一边将内容写入文件 |
| `awk` | 按规则筛选文本；此处只影响显示，不影响完整日志 |
| `latest=...` | 创建名为 `latest` 的 Shell 变量 |
| `ls -t` | 按修改时间从新到旧列出文件 |
| `head -n 1` | 只取第一行，即最新日志 |
| `$(...)` | 执行括号内命令，并将结果赋给外层命令或变量 |
| `python3 <脚本>` | 使用 Python 3 执行脚本 |
| `-o <目录>` | 指定扫参图和 CSV 的输出目录 |

## 8. 不使用脚本时的直接运行方式

如果当前位于 `sim` 目录，可以直接运行二进制并保存完整日志：

```bash
cd ~/wayne/model_training/sim

../build/exe/test_prj \
  --gtest_filter='RingPerfBenchmark.*' \
  2>&1 | tee ring_perf_result.txt
```

再生成报告：

```bash
python3 ../src/aicore/tools/generate_ring_perf_report.py \
  ring_perf_result.txt ring_perf_report.html
```

这种方式不会生成 `logs/full` 和 `logs/filtered` 两套日志，但 `tee` 保存的 `ring_perf_result.txt` 包含完整输出，可以用于报告生成。

## 9. 如何判断执行成功

Google Test 结束时应看到：

```text
[  PASSED  ] N tests.
```

并检查 Shell 退出码：

```bash
echo $?
```

含义：

| 退出码 | 含义 |
|---|---|
| `0` | 所选测试全部通过 |
| 非 `0` | 至少一个测试失败，或程序异常退出 |

性能测试还应检查：

```text
drained=1
protocol_errors=0
measurement_valid=1
```

其中：

- `drained=1`：测试结束时请求、队列和在途事务已排空；
- `protocol_errors=0`：未发现协议错误；
- `measurement_valid=1`：该场景的性能测量区间有效。

## 10. 常见问题

### 10.1 `./test_prj: No such file or directory`

原因：执行脚本时，当前目录中没有 `test_prj`。

处理：

```bash
cd ~/wayne/model_training/build/exe
ls -l test_prj
```

确认文件存在后再运行脚本。

### 10.2 `Permission denied`

测试程序没有执行权限：

```bash
chmod u+x test_prj
```

脚本本身可直接使用 `bash <脚本路径>`，不要求执行权限。

### 10.3 显示 `0 tests`

原因通常是 filter 名称与实际测试名不一致。

处理：

```bash
./test_prj --gtest_list_tests
```

复制真实套件名和用例名，并用单引号包住 filter。

### 10.4 HTML 报告提示缺少 `PERF_*` 字段

常见原因：

- 使用了 `logs/filtered`，详细记录已被过滤；
- 只运行了不输出性能记录的功能测试；
- 测试二进制和 Python 报告脚本来自不同代码版本；
- 测试异常中止，场景没有输出完整结果。

优先改用同一次运行的 `logs/full`，并保证测试程序与报告脚本来自同一版本。

### 10.5 `ModuleNotFoundError: No module named 'matplotlib'`

只有三个扫参绘图脚本依赖 `matplotlib`。HTML 报告生成器不依赖它。

先确认环境：

```bash
python3 -c 'import matplotlib; print(matplotlib.__version__)'
```

如未安装，应在项目使用的 Python 虚拟环境中安装，避免修改系统全局 Python。

### 10.6 报告生成成功但拓扑或队列数据为空

检查输入日志是否包含：

```bash
grep '^PERF_RING_EDGE' logs/full/<时间戳>.log | head
grep '^PERF_RING_BUFFER' logs/full/<时间戳>.log | head
grep '^PERF_HA_SOURCE' logs/full/<时间戳>.log | head
```

如果这些记录不存在，说明测试场景未输出对应 PMU 数据，或者使用了精简日志。

## 11. 推荐执行流程

```text
列出测试
  -> 单用例功能确认
  -> 运行目标性能套件
  -> 检查退出码、drained 和 protocol_errors
  -> 使用完整日志生成 HTML
  -> 根据需要生成 L2/Outstanding/Burst 扫参图
  -> 保存配置、完整日志、HTML、CSV 和 PNG
```

需要复现实验时，应将测试配置、完整日志和报告一起保存。只保留 HTML 无法确认输入参数和原始计数。
