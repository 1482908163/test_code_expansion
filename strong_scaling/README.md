# 两个瓶颈的算法实验

新分支：`agent/dependency-aware-mesh-balance`。原采集分支保持不变。

实现及文献说明：[算法设计](../docs/algorithm_design.md)。代码是待集群验证的研究实现，尚无新算法加速比结论。

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

当前编号格式仍是32位；全局点号或单元号超过约21.47亿会明确退出。更大规模正确性运行需要先完成全系统64位编号改造，不能只扩大计数变量。

## 一次提交四组实验

```bash
ALGORITHMS='baseline balance sparse combined' \
TIMING_MODES='natural split' \
PROCESS_COUNTS='16 32 64' \
RANKS_PER_NODE=16 REPEATS=5 WARMUPS=1 \
LEVELS=1 REFINES=1 \
INPUT_PATH=/实际路径/wholewall3solid.STEP \
bash strong_scaling/submit_experiments.sh
```

`submit_suite.sh`（套件提交入口）仍可作为同一脚本的兼容入口。

- `natural`（自然运行）：用于性能对比；集合调用内可能含到达等待。
- `split`（等待/执行拆分）：四组一致添加前置屏障，用于归因；不能与自然运行数据混合。
- `REPEATS=5`（正式重复5次）、`WARMUPS=1`（先预热1次）；预热编号为0，不计入汇总。
- 不需要设置原来的 `CORE_ONLY`、`CACHE_COUNTERS` 或 `EXPERIMENT`（核心开关、缓存计数、实验名称）。新运行器始终采集核心阶段。
- 每个进程规模独立提交，统一最早启动时刻；没有作业串行依赖。相同规模内按轮次轮换算法顺序。
- 失败运行记录退出码，并继续后面的算法/重复；作业结束仍返回失败状态。再次使用相同 `RUN_ROOT`（结果目录）会跳过成功运行、重试失败运行。
- 更改二进制、输入、算法列表、重复次数或模型参数时，选择新结果目录；脚本不把旧配置结果混入新实验。

可选变量：`BALANCE_SWEEPS=4`（修正轮数）、`CUT_GROWTH=0.05`（切分面增长上限）、`COST_WEIGHTS=1,1,1,1`（四阶段代理权重）、`START_DELAY_SECONDS=120`（所有作业相同的启动延时）、`TIMEOUT_SECONDS=7200`（单次运行超时）、`DRY_RUN=1`（仅打印提交命令）。

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

检查时先看自然运行核心时间，再看拆分等待、三轮稀疏通信总成本及分区修正开销。预测代价下降并不保证真实时间下降。跨进程数/划分模式若实际网格数量变化，不直接套用固定工作量强扩展效率。

已移除：硬件缓存计数、页缓存干预、逐边通信图导出及其专用套件分析；保留与两项算法相关的阶段计时、必要通信指标、失败恢复及针对性正确性测试。历史结果目录内容保留。
