# 两个瓶颈的算法实验

新分支：`agent/dependency-aware-mesh-balance`。原采集分支保持不变。

第一轮完成：稀疏通信显著降低通信开销，旧均衡模型尚无稳定净收益。见 [第一轮归档](../docs/experiments/20260906_first_run.md)。
第二轮方法、公式、校准隔离与判断标准见 [研究记录](../docs/research_log.md)；第二轮尚未在集群实测。

## 四组消融

| ALGORITHMS（算法列表）取值 | 分区修正 | 面交换 |
|---|---|---|
| `baseline`（原算法对照） | 关闭 | 全收集 |
| `balance`（仅均衡） | 开启 | 全收集 |
| `sparse`（仅稀疏通信） | 关闭 | 面匹配与顶点依赖闭包 |
| `combined`（组合） | 开启 | 面匹配与顶点依赖闭包 |

主程序默认 `baseline`。四组使用相同的种子分区/广播和简化采集；请以本分支对照衡量算法增量。历史采集结果及原分支未修改。

## 构建与小规模正确性检查

在新分支当前目录运行原名称的构建脚本。脚本使用当前目录，不会跳回硬编码的另一个检出目录。已有默认安装目录名称保持不变。

```bash
git fetch origin
git switch agent/dependency-aware-mesh-balance
bash build_project.sh

# 先在已分配的计算资源中运行算法测试
MPI_LAUNCHER=yhrun MPI_EXTRA_ARGS='--mpi=pmix' \
TEST_PROCESS_COUNTS='1 2 3 4 8' bash tests/run_tests.sh

# 小规模网格验证，不开启性能采集
mkdir -p result/check_sparse
yhrun --mpi=pmix -n 16 ./build/mesh_occ_mpi/mesh_occ_mpi \
  -i inputData/wholewall3solid.STEP -l 1 -r 1 -adj -v \
  --algorithm sparse --verify-faces -o result/check_sparse/
```

`--verify-faces` 会实际运行原全收集作为参考，逐项比较面记录及共享顶点/边/面的进程集合；因此仅用于小规模验证，不能和性能采集同时使用。该检查通过后仍需比较完整输出。均衡方案另检查几何边界、单元规模和网格质量。

最终全局点号/单元号、通信及写出已采用64位，不再受21.47亿的全局编号限制；无需额外开关。每个进程内部仍使用 Netgen（网格库）的32位局部索引，包含邻接副本的局部实体数量必须小于21.47亿。下游结果读取器也须支持64位全局编号。合成百亿编号及单进程 MPI 测试通过，不代表百亿网格端到端运行已验证。请重新构建，并使用新的实验结果目录。

## 统一提交入口

所有常用参数集中在 `run_experiments.sh`（实验运行脚本）开头的“统一实验配置区”。在登录节点只执行：

```bash
bash strong_scaling/run_experiments.sh
```

脚本会自动为每个进程规模申请对应节点并行提交；进入计算作业后，同一个脚本自动执行实验，不需要手动调用 `submit_experiments.sh`（提交脚本）。原 `submit_experiments.sh` 和 `submit_suite.sh`（套件提交脚本）仅保留为兼容入口，最终都会进入相同配置。

提交时会把源码中的 `strong_scaling`（强扩展脚本目录）保存到 `STRONG_SCALING_DIR`，因此即使 `yhbatch`（作业提交器）把运行脚本复制为 `/tmp/slurmd/job*/slurm_script`，计算节点仍从仓库原目录加载 `cluster_env.sh`（集群环境脚本）和分析器，不会到临时目录查找配套文件。

当前保留用户正式规模默认值 `EXPERIMENT_PRESET=production`：1024、2048、4096、8192进程，对应64、128、256、512节点，L=R=3。
若需要先预检，在配置区改为 `pilot`：16、32、64进程，对应1、2、4节点，L=R=1，并执行稀疏面正确性核对。

第二轮先保持配置区：

```bash
EXPERIMENT_STAGE="${EXPERIMENT_STAGE:-calibration}"
```

完成校准后，在同一个配置区改两处（目录填本次新校准结果，第一轮旧汇总缺少新特征）：

```bash
EXPERIMENT_STAGE="${EXPERIMENT_STAGE:-evaluation}"
CALIBRATION_ROOT="${CALIBRATION_ROOT:-/完整路径/本次校准结果目录}"
```

两步都只执行 `bash strong_scaling/run_experiments.sh`，无需在命令行拼接环境参数；各用一个新结果目录。校准默认 baseline/sparse，评价默认四组消融。两步均默认 natural/split、预热1次、正式5次、每节点16进程、`MAXH=1000`、`MINH=0`，成功后清理过程数据。

评价前自动训练每个目标规模的冻结模型，完全排除该目标规模的校准样本；例如8192仅使用1024/2048/4096。先在 `models/MODEL_REPORT.md` 查看内部验证，实际性能结论仍由目标规模对照给出。模型绑定输入、二进制、L/R、线程与网格参数，不允许混用。`legacy` 模式保留第一轮代理修正；要精确复现第一轮源码应使用归档的提交。

`submit_suite.sh`（套件提交入口）仍可作为同一脚本的兼容入口。

- `natural`（自然运行）：用于性能对比；集合调用内可能含到达等待。
- `split`（等待/执行拆分）：四组一致添加前置屏障，用于归因；不能与自然运行数据混合。
- `REPEATS=5`（正式重复5次）、`WARMUPS=1`（先预热1次）；预热编号为0，不计入汇总。
- 不需要设置原来的 `CORE_ONLY`、`CACHE_COUNTERS` 或 `EXPERIMENT`（核心开关、缓存计数、实验名称）。新运行器始终采集核心阶段。
- 每个进程规模独立提交，统一最早启动时刻；没有作业串行依赖。相同规模内按轮次轮换算法顺序。
- 失败运行记录退出码，并继续后面的算法/重复；作业结束仍返回失败状态。再次使用相同 `RUN_ROOT`（结果目录）会跳过成功运行、重试失败运行。
- 更改二进制、输入、算法列表、重复次数或模型参数时，选择新结果目录；脚本不把旧配置结果混入新实验。

配置区还可以直接修改：`ALGORITHMS`（算法列表）、`TIMING_MODES`（计时模式）、`REPEATS`（正式次数）、`WARMUPS`（预热次数）、`BALANCE_SWEEPS`（修正轮数）、`CUT_GROWTH`（切分面增长上限）、`COST_WEIGHTS`（四阶段代理权重）、`START_DELAY_SECONDS`（所有作业统一的最早启动延时）、`TIMEOUT_SECONDS`（单次超时）和 `DRY_RUN`（仅打印提交命令）。

## 结果自动清理

默认 `CLEANUP_RESULTS=1`（开启清理），原提交命令无需增加参数。每次运行退出成功、逐进程指标通过完整性检查后立即清理；全部分析完成后再检查一次。清理与压缩均在核心计时之外。

- 删除成功性能运行 `repeat_*/mesh/` 下程序生成的普通网格文件：粗网格、细化面网格、细化体网格、邻接体网格及分区点/单元文件；只匹配已知目录和文件名，不按扩展名扫描整个结果根目录。
- `rank_profiles.jsonl`（原始逐进程指标）和 `run.log`（运行日志）在压缩后更小时保存为 `.gz`（无损压缩文件）；分析和断点续跑同时支持压缩前后格式，重新分析不需要手动解压。
- 保留原始指标、日志内容、汇总表、运行参数、作业状态、预热记录、质量统计及未知文件。失败现场和 `verify_*`（正确性验证）目录不自动清理，便于排错和结果比对。
- 运行器和分析器共用目录锁；活动运行不会被清理。压缩失败保留原始文件，并输出清理告警；清理失败不会把有效性能测量改成失败。

如需保留全部过程文件，在统一配置区设置 `CLEANUP_RESULTS=0`（关闭清理）。手动分析时用 `--keep-artifacts`（保留过程文件）关闭本次清理：

```bash
python3 strong_scaling/analyze_results.py /完整实验结果目录 --keep-artifacts
```

## 实验结束后看哪些文件

每个进程规模目录（如 `p1024/`）优先只看以下文件：

1. `RESULT_SUMMARY.txt`：第一入口；显示实验是否完整、有效重复次数和主要指标。
2. `analysis/summary.csv`：论文作图与算法比较使用的重复实验中位数。
3. `analysis/issues.txt`：仅当总览中的异常项不为 0 时查看。
4. `repeat_*/run.log.gz`：仅在排查某一次失败时查看，不参与日常结果阅读。

逐次运行目录和逐进程画像是可追溯的底层数据，不需要逐个阅读。正常作业日志只输出一条
`RESULT`（结果）汇总；逐次画像验证错误收集在 `p*/failures.log`，避免同一错误刷屏。

运行器还会在正式实验前检查可执行文件是否包含本分支的性能采集能力，并检查相关源码是否
比可执行文件更新。若提示 `stale/incompatible mesh executable`（陈旧或不兼容的网格程序），先运行：

```bash
cmake --build build -j
```

然后再从统一入口 `strong_scaling/run_experiments.sh` 提交。

默认重新分析会清理已有的新格式成功结果。旧版目录中无法由当前分析器验证的实验，不会自动删除。本功能不在提交代码时清理历史档案，也不新增测试目录或清理脚本。

大规模进程列表由用户显式指定；先确认全局编号容量和小规模正确性，再扩大资源。`cluster_env.sh`（集群环境脚本）保留原已验证的 MPI-X 与 PMIx（集群进程管理接口）设置。

## 汇总

每个作业结束自动分析本进程规模。全部作业结束后：

```bash
python3 strong_scaling/analyze_results.py /完整实验结果目录
```

输出在 `analysis`（分析目录）：

- `runs.csv`（各次正式运行）：直接测量的核心时间、面处理时间、等待、不均衡、通信量、实际单元规模、模型相关性。
- `stages.csv`（各阶段）：逐次最大/平均时间，便于定位不均衡来自哪一步。
- `summary.csv`（汇总）：重复实验中位数、变异系数、相同进程数与计时模式下相对对照的加速比。
- `issues.txt`（异常）：失败、缺失或不完整记录，不静默当成成功结果。
- `model_samples.csv.gz`（第二轮紧凑样本）：阶段特征、耗时和可复现身份；供训练/验证使用，不必逐行阅读。上传结果时保留此文件；评价批次也保留根目录的 `models/`。不需要上传所有逐次原始画像。

检查时先看自然运行核心时间，再看拆分等待、三轮稀疏通信总成本及分区修正开销。预测代价下降并不保证真实时间下降。跨进程数/划分模式若实际网格数量变化，不直接套用固定工作量强扩展效率。

已移除：硬件缓存计数、页缓存干预、逐边通信图导出及其专用套件分析；保留与两项算法相关的阶段计时、必要通信指标、失败恢复及针对性正确性测试。历史结果目录内容保留。
