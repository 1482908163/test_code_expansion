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
| 系统 AArch64、Netgen、本地库路径 | 统一由 `cluster_env.sh` 设置，不继承登录节点的动态库路径 |
| `yhrun --mpi=pmix` | `LAUNCHER=yhrun`、`LAUNCHER_EXTRA_ARGS=--mpi=pmix` |
| `mt_module` 分区 | `PARTITION=mt_module` |
| 每节点/每任务 1 个进程和线程 | `RANKS_PER_NODE=1`、`OMP_NUM_THREADS=1` |
| `wholewall3solid.STEP` | `<当前仓库>/inputData/wholewall3solid.STEP` |
| 当前强扩展参数 | `LEVELS=2`、`REFINES=2` |
| `maxh=1000, minh=0` | `MAXH=1000.0`、`MINH=0.0` |

仓库目录不再硬编码为 `test_code_time`：编译、可执行文件、输入和结果路径均从当前 checkout（检出的仓库）自动推导。第三方库路径仍保留集群上的现有默认值，需要时可通过同名环境变量覆盖。

运行环境以已验证的 `test_code_part03/cjz_nodsp_copy.sh` 为基准。`LD_LIBRARY_PATH` 会被设置为确定值，不会继承提交节点中的旧 MPI/PMIx 路径；尤其不得加入 `/vol8/home/hnu_lhz/cjz/aarch64-linux-gnu`，否则会混用该目录下的 `libpmix.so.2` 与 MPI-X 的 `libmpi.so.12`。计算作业会在启动 `yhrun` 前用 `ldd` 验证二者来源。

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

`DRY_RUN=1` 只生成计划并打印 `yhbatch` 命令，不提交作业：

```bash
DRY_RUN=1 bash strong_scaling/submit_suite.sh
```

推荐入口的默认规模为 `1 2 4 8 16 32 64 128 256` 个进程，每个规模重复 3 次，每节点 1 个 MPI（消息传递接口）进程。提交脚本仍为每个进程数申请一个独立 Slurm 作业，但默认使用 `afterany` 依赖按 P 从小到大串行启动；因此本批实验的不同 P 不会同时争用共享文件系统。同一 P 的所有模式和重复实验在同一个 allocation（资源分配）中顺序完成，最后自动启动汇总作业。

### 3.2 推荐：一次提交完整实验套件

```bash
bash strong_scaling/submit_suite.sh
```

这一个命令会在每个 P 的同一批节点上依次运行：

1. `core_cache`：核心路径计时、通信、并行效率和 CPU（中央处理器）硬件缓存计数；
2. `full_io`：完整结果写出和 I/O（输入/输出）计时。

核心计时和硬件缓存计数由同一次 `core_cache` 运行产生，因此不再默认提交第三组实验。若需要单独标定硬件计数器开销，可覆盖 `SUITE_MODES="core_timing core_cache full_io"`；统一报告会优先使用无硬件计数器的 `core_timing` 作为主强扩展曲线。

每种模式默认重复 3 次。第 1 次运行前，会在各计算节点通过 `POSIX_FADV_DONTNEED` 请求丢弃输入文件、可执行文件和动态库的 OS（操作系统）页缓存，标为 cold（冷缓存）；第 2、3 次不清理，作为 warm（热缓存），报告取其中位数。该接口只是无特权的内核提示，是否真正形成冷启动要结合报告中的 `PhysR`（物理读取量）判断。

“一次提交”不是只执行一次程序：冷/热对照至少需要两次运行，而核心路径和完整 I/O 也必须分开，否则结果写出会污染通信计时。

### 3.3 常用参数

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `PROCESS_COUNTS` | `1 2 4 8 16 32 64 128 256` | MPI 进程数序列 |
| `REPEATS` | `3` | 每个规模的重复次数 |
| `RANKS_PER_NODE` | `1` | 每节点 MPI 进程数 |
| `PARTITION` | `mt_module` | Slurm 分区 |
| `LAUNCHER_EXTRA_ARGS` | `--mpi=pmix` | `yhrun` 的附加参数 |
| `CPU_BIND` | `cores` | 将 rank（进程）绑定到 CPU 核；若该集群不接受此参数，设为 `none` |
| `CORE_ONLY` | `1` | `1` 跳过普通结果写出，`0` 测完整 I/O |
| `CACHE_COUNTERS` | `0` | 是否采集硬件缓存计数 |
| `SERIALIZE_JOBS` | `1` | `1` 使不同 P 作业按依赖串行，避免本批实验相互争用共享文件系统 |
| `SUITE_MODES` | `core_cache full_io` | 统一套件包含的模式；由 `submit_suite.sh` 使用 |
| `PAGE_CACHE_POLICY` | 套件为 `evict-first` | `evict-first` 在每种模式的第 1 次运行前请求清理文件页缓存；`observe` 只观察首次运行 |
| `PAGE_CACHE_STRICT` | `0` | 清缓存提示失败时是否立即终止；默认继续并把该次标为候选冷启动 |
| `LEVELS` / `REFINES` | `2` / `2` | 表面和体网格细化次数 |
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

统一套件每次提交生成独立的 `<EXPERIMENT>_<BATCH_ID>` 目录：

```text
strong_scaling_results/
└── strong_scaling_suite_l2_r2_YYYYMMDD-HHMMSS/
    ├── run_plan.txt                 # 参数、规模与 Slurm 作业号
    ├── scheduler_logs/              # sbatch 标准输出/错误
    ├── mode_status/                 # 每个 P、每种模式的完成状态
    ├── modes/
    │   ├── core_cache/              # 核心、通信、并行效率和硬件缓存
    │   │   ├── environment/         # CPU/cache、模块、动态库和 git 提交
    │   │   ├── commands/            # 每次运行的完整可复现命令
    │   │   ├── launcher_logs/       # 每次 yhrun 的程序输出
    │   │   ├── page_cache_logs/     # 第 1 次运行前的页缓存准备日志
    │   │   ├── status/              # 含 cold/warm 状态、退出码和墙钟时间
    │   │   ├── p00016/              # profiler（性能分析器）原始结果
    │   │   └── analysis/            # 该模式的详细阶段报告
    │   └── full_io/                 # 相同结构；另含普通网格输出
    └── analysis/
        ├── suite_report.txt         # 首先阅读：热缓存主曲线与冷/热对照
        └── suite_summary.csv        # 核心、cache、I/O 的统一绘图数据
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
- 主强扩展曲线只使用 warm（第 2 次及以后）的中位数；cold（第 1 次）单列，二者不再混合计算中位数或变异系数。
- 不同 P 默认串行可消除本批作业之间的并发竞争，但不能隔离集群上其他用户的共享文件系统负载；应同时保留作业时间、节点和调度日志。
- `all_runs.csv` 和 `CV(%)`（变异系数）用于判断重复实验抖动；抖动大时不要只看中位数，应检查 `scheduler_logs/`、节点列表和逐 rank 数据。
