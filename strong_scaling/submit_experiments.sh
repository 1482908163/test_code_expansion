#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# One Slurm allocation is submitted per process count.  Repetitions run inside
# the same allocation, then a dependent one-node job performs unified analysis.
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPOSITORY_ROOT}/strong_scaling_results}"
EXPERIMENT="${EXPERIMENT:-wholewall_l4_r3_core}"
BATCH_ID="${BATCH_ID:-$(date +%Y%m%d-%H%M%S)}"
RUN_NAME="${RUN_NAME:-${EXPERIMENT}_${BATCH_ID}}"
RUN_ROOT="${OUTPUT_ROOT}/${RUN_NAME}"
PROCESS_COUNTS="${PROCESS_COUNTS:-16 32 64 128 256}"
REPEATS="${REPEATS:-3}"
RANKS_PER_NODE="${RANKS_PER_NODE:-1}"
PARTITION="${PARTITION:-mt_module}"
SBATCH_COMMAND="${SBATCH_COMMAND:-sbatch}"
SBATCH_JOB_PREFIX="${SBATCH_JOB_PREFIX:-mesh_scale}"
SBATCH_EXTRA_ARGS="${SBATCH_EXTRA_ARGS:-}"
DRY_RUN="${DRY_RUN:-0}"

if [[ ! "${RUN_NAME}" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "[ERROR] RUN_NAME 只能包含字母、数字、下划线和连字符。" >&2
    exit 2
fi
if [[ ! "${PROCESS_COUNTS}" =~ ^[[:space:]0-9]+$ ]]; then
    echo "[ERROR] PROCESS_COUNTS 必须是空格分隔的正整数。" >&2
    exit 2
fi
if [[ -z "${PROCESS_COUNTS//[[:space:]]/}" ]]; then
    echo "[ERROR] PROCESS_COUNTS 至少需要一个进程数。" >&2
    exit 2
fi
if [[ ! "${REPEATS}" =~ ^[1-9][0-9]*$ || ! "${RANKS_PER_NODE}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[ERROR] REPEATS 和 RANKS_PER_NODE 必须是正整数。" >&2
    exit 2
fi
if [[ "${DRY_RUN}" != "1" ]] && ! command -v "${SBATCH_COMMAND}" >/dev/null 2>&1; then
    echo "[ERROR] 找不到 ${SBATCH_COMMAND}；请在 Slurm 登录节点运行。" >&2
    exit 2
fi

mkdir -p "${RUN_ROOT}/scheduler_logs"
read -r -a sbatch_extra <<< "${SBATCH_EXTRA_ARGS}"

# Export every experiment setting once.  Each compute job only overrides its
# own PROCESS_COUNTS and disables premature analysis.
export OUTPUT_ROOT EXPERIMENT BATCH_ID RUN_NAME REPEATS RANKS_PER_NODE PARTITION
export MESH_EXECUTABLE="${MESH_EXECUTABLE:-${REPOSITORY_ROOT}/build/mesh_occ_mpi/mesh_occ_mpi}"
export INPUT_MESH="${INPUT_MESH:-${REPOSITORY_ROOT}/inputData/wholewall3solid.STEP}"
export LEVELS="${LEVELS:-4}"
export REFINES="${REFINES:-3}"
export MAXH="${MAXH:-1000.0}"
export MINH="${MINH:-0.0}"
export CORE_ONLY="${CORE_ONLY:-1}"
export CACHE_COUNTERS="${CACHE_COUNTERS:-1}"
export LAUNCHER_STYLE="${LAUNCHER_STYLE:-yhrun}"
export LAUNCHER="${LAUNCHER:-${LAUNCHER_STYLE}}"
export LAUNCHER_EXTRA_ARGS="${LAUNCHER_EXTRA_ARGS:---mpi=pmix}"
export EXTRA_APP_ARGS="${EXTRA_APP_ARGS:-}"
export CPU_BIND="${CPU_BIND:-cores}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export LOAD_CLUSTER_ENV="${LOAD_CLUSTER_ENV:-1}"

{
    echo "Strong Scaling Submission Plan (强扩展提交计划)"
    echo "================================================"
    echo "created_at=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "repository_root=${REPOSITORY_ROOT}"
    echo "experiment=${EXPERIMENT}"
    echo "batch_id=${BATCH_ID}"
    echo "run_name=${RUN_NAME}"
    echo "result_directory=${RUN_ROOT}"
    echo "process_counts=${PROCESS_COUNTS}"
    echo "repeats=${REPEATS}"
    echo "ranks_per_node=${RANKS_PER_NODE}"
    echo "partition=${PARTITION}"
    echo "mesh_executable=${MESH_EXECUTABLE}"
    echo "input_mesh=${INPUT_MESH}"
    echo "levels=${LEVELS}"
    echo "refines=${REFINES}"
    echo "maxh=${MAXH}"
    echo "minh=${MINH}"
    echo "core_only=${CORE_ONLY}"
    echo "cache_counters=${CACHE_COUNTERS}"
    echo "launcher=${LAUNCHER}"
    echo "launcher_extra_args=${LAUNCHER_EXTRA_ARGS}"
    echo "cpu_bind=${CPU_BIND}"
    echo "sbatch_extra_args=${SBATCH_EXTRA_ARGS}"
    echo
    echo "processes nodes"
    for processes in ${PROCESS_COUNTS}; do
        nodes=$(( (processes + RANKS_PER_NODE - 1) / RANKS_PER_NODE ))
        printf '%9d %5d\n' "${processes}" "${nodes}"
    done
} > "${RUN_ROOT}/run_plan.txt"

job_ids=()
for processes in ${PROCESS_COUNTS}; do
    if (( processes <= 0 )); then
        echo "[ERROR] 非法进程数: ${processes}" >&2
        exit 2
    fi
    nodes=$(( (processes + RANKS_PER_NODE - 1) / RANKS_PER_NODE ))
    process_tag=$(printf 'p%05d' "${processes}")
    submit_command=(
        "${SBATCH_COMMAND}"
        --parsable
        --job-name="${SBATCH_JOB_PREFIX}_${processes}"
        --partition="${PARTITION}"
        --nodes="${nodes}"
        --ntasks="${processes}"
        --ntasks-per-node="${RANKS_PER_NODE}"
        --cpus-per-task=1
        --output="${RUN_ROOT}/scheduler_logs/${process_tag}_%j.out"
        --error="${RUN_ROOT}/scheduler_logs/${process_tag}_%j.err"
        --export="ALL,PROCESS_COUNTS=${processes},ANALYZE_AFTER_RUN=0,ANALYSIS_ONLY=0,DRY_RUN=0"
    )
    submit_command+=( "${sbatch_extra[@]}" "${SCRIPT_DIR}/run_experiments.sh" )

    printf '[SUBMIT] processes=%s nodes=%s:' "${processes}" "${nodes}"
    printf ' %q' "${submit_command[@]}"
    printf '\n'
    if [[ "${DRY_RUN}" == "1" ]]; then
        continue
    fi

    job_result=$("${submit_command[@]}")
    job_id="${job_result%%;*}"
    if [[ ! "${job_id}" =~ ^[0-9]+$ ]]; then
        echo "[ERROR] 无法解析 sbatch 作业号: ${job_result}" >&2
        exit 2
    fi
    job_ids+=("${job_id}")
    echo "[QUEUED] ${process_tag} -> job ${job_id}"
done

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[OK] 提交 dry-run 完成；计划文件: ${RUN_ROOT}/run_plan.txt"
    exit 0
fi

dependency="afterok"
for job_id in "${job_ids[@]}"; do
    dependency+=":${job_id}"
done

analysis_command=(
    "${SBATCH_COMMAND}"
    --parsable
    --job-name="${SBATCH_JOB_PREFIX}_analysis"
    --partition="${PARTITION}"
    --nodes=1
    --ntasks=1
    --cpus-per-task=1
    --dependency="${dependency}"
    --output="${RUN_ROOT}/scheduler_logs/analysis_%j.out"
    --error="${RUN_ROOT}/scheduler_logs/analysis_%j.err"
    --export="ALL,ANALYSIS_ONLY=1,ANALYZE_AFTER_RUN=1,LOAD_CLUSTER_ENV=0,DRY_RUN=0"
)
analysis_command+=( "${sbatch_extra[@]}" "${SCRIPT_DIR}/run_experiments.sh" )
analysis_result=$("${analysis_command[@]}")
analysis_job_id="${analysis_result%%;*}"

{
    echo
    echo "compute_job_ids=${job_ids[*]}"
    echo "analysis_job_id=${analysis_job_id}"
    echo "dependency=${dependency}"
} >> "${RUN_ROOT}/run_plan.txt"

echo "============================================================"
echo "已提交计算作业: ${job_ids[*]}"
echo "自动汇总作业: ${analysis_job_id}"
echo "统一结果目录: ${RUN_ROOT}"
echo "完成后先看: ${RUN_ROOT}/analysis/scaling_report.txt"
echo "============================================================"
