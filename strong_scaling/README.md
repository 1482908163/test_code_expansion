# Strong scaling profiler（强扩展分析工具）

这套工具在固定 STEP（三维模型交换格式）输入和固定网格参数下，随 MPI（消息传递接口）进程数增加，测量：

- 端到端时间、加速比和并行效率；
- 集合通信、点对点通信、同步等待、消息数和通信字节数；
- 各进程局部网格规模及最大值/平均值负载不均衡；
- CPU cache（处理器缓存）访问、未命中、IPC（每周期指令数）；
- 页故障、逻辑/实际存储读写量、STEP 读取及结果写出的 I/O（输入/输出）时间和峰值 RSS（常驻内存集）。

硬件缓存计数通过 Linux `perf_event_open` 获取。若系统的 `perf_event_paranoid` 权限不允许访问，程序仍会完成实验，并在 `summary.txt` 和 `metadata.txt` 中标记计数器不可用。
`cache-references`/`cache-misses` 是处理器提供的通用硬件事件，通常更接近末级缓存行为，精确定义随 CPU 型号变化；因此应在相同节点类型上比较不同进程数。

## 1. 单次运行

```bash
yhrun -p mt_test -N 1 -n 16 ./mesh_occ_mpi \
  -i /path/wholewall3solid.STEP \
  -o /path/experiment/application_output/ \
  -l 1 -r 0 --maxh 1000 --minh 10 -adj \
  --profile --profile-cache --profile-core-only \
  --profile-dir /path/experiment/profiles \
  --profile-experiment wholewall_l1_r0 \
  --profile-repeat 1
```

`--profile-core-only` 会跳过普通网格结果写出、质量评价和旧版计时文件，保留完整网格生成、全局编号和邻接通信，用于测量核心算法扩展性。去掉该参数即可测量完整 I/O 开销。

## 2. 批量运行

```bash
export MESH_EXECUTABLE=/path/build/mesh_occ_mpi/mesh_occ_mpi
export INPUT_MESH=/path/inputData/wholewall3solid.STEP
export OUTPUT_ROOT=/path/result/strong_scaling
export PARTITION=mt_test
export RANKS_PER_NODE=16
export PROCESS_COUNTS="1 2 4 8 16 32 64 128"
export REPEATS=3
export LEVELS=1
export REFINES=0
export EXPERIMENT=wholewall_l1_r0_core

bash strong_scaling/run_experiments.sh
```

常用环境变量：

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `LAUNCHER_STYLE` | `yhrun` | 支持 `yhrun`、`srun`、`mpirun`、`mpiexec` |
| `LAUNCHER_EXTRA_ARGS` | 空 | 例如 `--mpi=pmix` |
| `CORE_ONLY` | `1` | `1` 测核心算法，`0` 测完整 I/O |
| `CACHE_COUNTERS` | `1` | 是否请求硬件缓存计数器 |
| `PROCESS_COUNTS` | `1 2 4 8 16 32 64` | MPI 进程数序列 |
| `REPEATS` | `3` | 每个进程数重复次数 |
| `BATCH_ID` | 当前时间 | 区分多次批量提交，避免旧输出和日志被覆盖 |

核心算法测试和完整 I/O 测试应使用不同的 `EXPERIMENT` 名称，避免将两种口径混合计算并行效率。

脚本默认固定 `OMP_NUM_THREADS=1`。建议通过 `LAUNCHER_EXTRA_ARGS` 加入集群支持的绑核参数（例如 Slurm 的 `--cpu-bind=cores`），并在所有进程数下保持相同的每节点进程数、队列和节点类型。

建议至少运行两组：`CORE_ONLY=1 CACHE_COUNTERS=0` 用于低扰动的主强扩展曲线，`CORE_ONLY=0 CACHE_COUNTERS=1` 用于同时观察缓存和完整 I/O。若缓存计数器开销在短任务中不可忽略，可再运行 `CORE_ONLY=1 CACHE_COUNTERS=1`，并使用第三个实验名称。

## 3. 结果目录

所有分析结果统一位于：

```text
<profile-dir>/<experiment>/
├── p00016/
│   ├── repeat_01_YYYYMMDD-HHMMSS/
│   │   ├── summary.txt
│   │   ├── run_summary.csv
│   │   ├── stages.csv
│   │   ├── rank_stages.csv
│   │   ├── rank_metrics.csv
│   │   └── metadata.txt
│   └── repeat_02_YYYYMMDD-HHMMSS/
├── launcher_logs/
└── analysis/
    ├── scaling_report.txt
    ├── stage_bottlenecks.txt
    ├── scaling_summary.csv
    ├── stage_scaling.csv
    └── all_runs.csv
```

- `summary.txt`：单次实验的清晰摘要，阶段按最大时间排序。
- `stages.csv`：各阶段最小/平均/最大时间、缓存、通信和 I/O 指标。
- `rank_stages.csv`：每个进程的原始阶段数据，用于定位慢进程。
- `rank_metrics.csv`：每个进程的网格规模、共享点和内存数据。
- `scaling_report.txt`：跨进程数的加速比、并行效率和瓶颈占比汇总。
- `stage_bottlenecks.txt`：每个进程数下耗时最高的阶段，便于直接定位瓶颈。
- `stage_scaling.csv`：跨进程数、跨重复实验的逐阶段中位数指标。

摘要中的 `Profile coverage`（阶段计时覆盖率）表示已细分阶段占端到端时间的比例；剩余部分列为 `unprofiled`（未细分），可用于发现还需继续插桩的代码路径或分析插桩本身的开销。

通信字节数表示应用层逻辑缓冲区规模；集合通信的接收量包含本进程贡献，不能等同为网络硬件链路上的实际字节数。点对点通信的消息数和字节数则按实际发送/接收缓冲区统计。

I/O 同时记录逻辑读写量（进程向文件系统请求的字节）和物理读写量（Linux `/proc/self/io` 报告的实际存储访问）。逻辑量很大而物理量很小时，通常说明操作系统页缓存或延迟写回正在起作用。
如果计算节点不提供 `/proc/self/io`，字节数字段会标记为 `N/A`，I/O 阶段时间仍可正常用于扩展性分析。

## 4. 单独重新汇总

```bash
python3 strong_scaling/analyze_results.py \
  /path/experiment/profiles/wholewall_l1_r0_core
```

加速比和并行效率以最小已测进程数 \(P_0\) 为基准：

\[
S(P)=\frac{T(P_0)}{T(P)},\qquad
E(P)=\frac{S(P)}{P/P_0}\times100\%.
\]
