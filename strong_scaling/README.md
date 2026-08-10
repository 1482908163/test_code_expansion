# Strong scaling profiler（强扩展瓶颈分析）

这套工具在固定 STEP（三维模型交换格式）输入和固定网格参数下，随 MPI（消息传递接口）进程数增加，统一测量：

- 端到端时间、加速比和并行效率；
- 集合通信、点对点通信、同步等待、消息数和应用层通信字节数；
- 各进程局部网格规模及最大值/平均值负载不均衡；
- CPU cache（处理器缓存）访问、未命中、IPC（每周期指令数）；
- 页故障、逻辑/物理存储读写量、STEP 读取及结果写出的 I/O（输入/输出）时间；
- 每个进程的主机名、峰值 RSS（常驻内存集）、CPU/缓存拓扑、模块和动态库环境。

## 1. 与现有集群脚本的对应关系

`cluster_env.sh` 统一了根目录新增脚本中的有效配置：

| 原脚本约定 | 强扩展脚本中的默认值 |
|---|---|
| `module purge; module load mpich/mpi-x` | `MPI_MODULE=mpich/mpi-x` |
| GCC 12 和 MPICH wrapper | `GCCHOME`、`MPICH_CC`、`MPICH_CXX` |
| AArch64、Netgen、本地库路径 | 统一由 `cluster_env.sh` 设置 |
| `yhrun --mpi=pmix` | `LAUNCHER=yhrun`、`LAUNCHER_EXTRA_ARGS=--mpi=pmix` |
| `mt_module` 分区 | `PARTITION=mt_module` |
| 每节点/每任务 1 个进程和线程 | `RANKS_PER_NODE=1`、`OMP_NUM_THREADS=1` |
| `wholewall3solid.STEP` | `<当前仓库>/inputData/wholewall3solid.STEP` |
| `numlevels=4, numrefine=3` | `LEVELS=4`、`REFINES=3` |
| `maxh=1000, minh=0` | `MAXH=1000.0`、`MINH=0.0` |

仓库目录不再硬编码为 `test_code_time`：编译、可执行文件、输入和结果路径均从当前 checkout（检出的仓库）自动推导。第三方库路径仍保留集群上的现有默认值，需要时可通过同名环境变量覆盖。

## 2. 编译插桩版本

在登录节点执行：

```bash
bash strong_scaling/build_profiled.sh
```

该脚本不会删除已有 `build/`，会使用与 `build_project.sh` 相同的 MPI/GCC 12 环境进行 CMake 配置、增量编译和安装。日志统一写入：

```text
strong_scaling_results/build_logs/build_YYYYMMDD-HHMMSS.log
```

常用覆盖参数：

```bash
BUILD_JOBS=32 INSTALL_AFTER_BUILD=0 \
  bash strong_scaling/build_profiled.sh
```

如果集群路径变化，只需覆盖环境变量，例如：

```bash
GCCHOME=/new/gcc-12 LOCAL_LIB=/new/local/lib \
  bash strong_scaling/build_profiled.sh
```

## 3. 提交强扩展实验

### 3.1 先检查提交命令

`DRY_RUN=1` 只生成计划并打印 `sbatch` 命令，不提交作业：

```bash
DRY_RUN=1 bash strong_scaling/submit_experiments.sh
```

默认规模为 `16 32 64 128 256` 个进程，每个规模重复 3 次，每节点 1 个 MPI 进程。提交脚本会为每个进程数申请一个独立 Slurm 作业；同一规模的重复实验在同一 allocation（资源分配）中完成。所有计算作业成功后，会自动启动一个单节点汇总作业。

### 3.2 第一轮建议的两组实验

核心算法、通信、缓存和并行效率：

```bash
EXPERIMENT=wholewall_l4_r3_core_cache \
CORE_ONLY=1 CACHE_COUNTERS=1 \
PROCESS_COUNTS="16 32 64 128 256" REPEATS=3 \
  bash strong_scaling/submit_experiments.sh
```

完整 I/O 路径：

```bash
EXPERIMENT=wholewall_l4_r3_full_io \
CORE_ONLY=0 CACHE_COUNTERS=0 \
PROCESS_COUNTS="16 32 64 128 256" REPEATS=3 \
  bash strong_scaling/submit_experiments.sh
```

为了排除硬件性能计数器本身对短任务的扰动，正式的主强扩展曲线建议再补一组：

```bash
EXPERIMENT=wholewall_l4_r3_core_timing \
CORE_ONLY=1 CACHE_COUNTERS=0 \
  bash strong_scaling/submit_experiments.sh
```

三组实验必须使用相同输入、网格参数、分区、节点类型、每节点进程数和重复次数。不要把 `CORE_ONLY=1` 与 `CORE_ONLY=0` 放入同一个 `RUN_NAME`。

### 3.3 常用参数

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `PROCESS_COUNTS` | `16 32 64 128 256` | MPI 进程数序列 |
| `REPEATS` | `3` | 每个规模的重复次数 |
| `RANKS_PER_NODE` | `1` | 每节点 MPI 进程数 |
| `PARTITION` | `mt_module` | Slurm 分区 |
| `LAUNCHER_EXTRA_ARGS` | `--mpi=pmix` | `yhrun` 的附加参数 |
| `CPU_BIND` | `cores` | 将 rank（进程）绑定到 CPU 核；若该集群不接受此参数，设为 `none` |
| `CORE_ONLY` | `1` | `1` 跳过普通结果写出，`0` 测完整 I/O |
| `CACHE_COUNTERS` | `1` | 是否采集硬件缓存计数 |
| `LEVELS` / `REFINES` | `4` / `3` | 表面和体网格细化次数 |
| `MAXH` / `MINH` | `1000.0` / `0.0` | 网格尺度参数 |
| `SBATCH_EXTRA_ARGS` | 空 | 账号、时限或集群允许的 `--exclusive` 等提交参数 |
| `OUTPUT_ROOT` | `<仓库>/strong_scaling_results` | 所有实验结果根目录 |
| `BATCH_ID` | 当前时间 | 区分不同批次，防止覆盖或混合数据 |

例如每节点运行 4 个进程并指定作业时限：

```bash
RANKS_PER_NODE=4 SBATCH_EXTRA_ARGS="--time=02:00:00" \
  bash strong_scaling/submit_experiments.sh
```

## 4. 已有 allocation 中直接运行

若已通过 `sbatch`/交互命令取得足够资源，可直接运行：

```bash
PROCESS_COUNTS="16 32 64" REPEATS=3 \
EXPERIMENT=wholewall_l4_r3_debug \
  bash strong_scaling/run_experiments.sh
```

只检查最终 `yhrun` 和应用参数、不执行程序：

```bash
LOAD_CLUSTER_ENV=0 DRY_RUN=1 \
MESH_EXECUTABLE=/path/to/mesh_occ_mpi INPUT_MESH=/path/to/model.STEP \
PROCESS_COUNTS="16 32" REPEATS=1 \
  bash strong_scaling/run_experiments.sh
```

## 5. 统一结果目录

每次提交生成独立的 `<EXPERIMENT>_<BATCH_ID>` 目录：

```text
strong_scaling_results/
└── wholewall_l4_r3_core_cache_YYYYMMDD-HHMMSS/
    ├── run_plan.txt                 # 参数、规模与 Slurm 作业号
    ├── scheduler_logs/              # sbatch 标准输出/错误
    ├── environment/                 # CPU/cache、模块、动态库和 git 提交
    ├── commands/                    # 每次运行的完整可复现命令
    ├── launcher_logs/               # 每次 yhrun 的程序输出
    ├── status/                      # 每次运行的状态、退出码和墙钟时间
    ├── application_output/          # CORE_ONLY=0 时的普通网格输出
    ├── p00016/
    │   └── repeat_01_YYYYMMDD-HHMMSS/
    │       ├── summary.txt          # 单次实验可读摘要
    │       ├── run_summary.csv      # 单次全局指标
    │       ├── stages.csv           # 阶段 min/avg/max 指标
    │       ├── rank_stages.csv      # 每进程阶段原始数据
    │       ├── rank_metrics.csv     # 每进程网格规模与内存
    │       └── metadata.txt
    └── analysis/
        ├── scaling_report.txt       # 首先阅读：加速比/效率/通信/I/O/cache
        ├── stage_bottlenecks.txt    # 各规模最耗时的 10 个阶段
        ├── scaling_summary.csv      # 适合绘图的强扩展汇总
        ├── stage_scaling.csv        # 逐阶段跨规模汇总
        └── all_runs.csv             # 所有重复实验原始总指标
```

这样每批数据、运行环境、命令、日志和分析都在同一个目录中；后续只需提供该目录，即可直接读取并比较瓶颈。

## 6. 单独重新汇总

如果汇总作业未运行，或后来补跑了某个规模：

```bash
python3 strong_scaling/analyze_results.py \
  strong_scaling_results/<RUN_NAME>
```

加速比和并行效率以最小已测进程数 \(P_0\) 为基准：

\[
S(P)=\frac{T(P_0)}{T(P)},\qquad
E(P)=\frac{S(P)}{P/P_0}\times100\%.
\]

## 7. 指标解释与注意事项

- `--profile-core-only` 保留网格生成、全局编号和邻接通信，跳过普通网格结果写出、质量评价和旧版计时文件；因此适合分析算法与通信扩展性。
- Linux `perf_event_open` 负责 cache references/misses、cycles、instructions 和 IPC。若 `perf_event_paranoid` 权限不足，程序仍会完成并将缓存指标标为 `N/A`。
- 通用 `cache-references`/`cache-misses` 的精确定义随 CPU 型号变化，只应在相同节点型号、相同绑核方式下横向比较。
- `Profile coverage`（计时覆盖率）之外的时间列为 `unprofiled`（未细分），可用于发现尚未插桩的路径。
- 通信字节数是应用层缓冲区规模，不等同于网络链路上的实际流量；集合通信接收量包含本进程贡献。
- `/proc/self/io` 用于区分逻辑和物理 I/O。逻辑读写量大、物理读写量小，通常表示页缓存或延迟写回生效。
- `all_runs.csv` 和 `CV(%)`（变异系数）用于判断重复实验抖动；抖动大时不要只看中位数，应检查 `scheduler_logs/`、节点列表和逐 rank 数据。
