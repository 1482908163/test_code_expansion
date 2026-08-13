#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# One Slurm allocation is submitted per process count.  Allocations are
# independent and may overlap, but their earliest start times are staggered so
# they do not all begin reading the shared input at once.  All requested modes
# and repeats for the same process count stay in one allocation.
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPOSITORY_ROOT}/strong_scaling_results}"
EXPERIMENT="${EXPERIMENT:-strong_scaling_suite_pp16}"
BATCH_ID="${BATCH_ID:-$(date +%Y%m%d-%H%M%S)}"
RUN_NAME="${RUN_NAME:-${EXPERIMENT}_${BATCH_ID}}"
RUN_ROOT="${OUTPUT_ROOT}/${RUN_NAME}"
PROCESS_COUNTS="${PROCESS_COUNTS:-1024 2048 4096 8192}"
REPEATS="${REPEATS:-6}"
RANKS_PER_NODE="${RANKS_PER_NODE:-16}"
PARTITION="${PARTITION:-mt_module}"
SBATCH_COMMAND="${SBATCH_COMMAND:-yhbatch}"
SBATCH_JOB_PREFIX="${SBATCH_JOB_PREFIX:-mesh_scale}"
SBATCH_EXTRA_ARGS="${SBATCH_EXTRA_ARGS:-}"
DRY_RUN="${DRY_RUN:-0}"
SERIALIZE_JOBS="${SERIALIZE_JOBS:-0}"
START_INTERVAL_SECONDS="${START_INTERVAL_SECONDS:-120}"
SUITE_MODE="${SUITE_MODE:-1}"
SUITE_MODES="${SUITE_MODES:-core_timing core_cache full_io}"
SUITE_MODES="${SUITE_MODES//,/ }"
PAGE_CACHE_POLICY="${PAGE_CACHE_POLICY:-evict-first}"
PAGE_CACHE_STRICT="${PAGE_CACHE_STRICT:-0}"

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
if [[ ! "${SERIALIZE_JOBS}" =~ ^[01]$ || ! "${SUITE_MODE}" =~ ^[01]$ || ! "${PAGE_CACHE_STRICT}" =~ ^[01]$ ]]; then
    echo "[ERROR] SERIALIZE_JOBS、SUITE_MODE 和 PAGE_CACHE_STRICT 只能是 0 或 1。" >&2
    exit 2
fi
if [[ ! "${START_INTERVAL_SECONDS}" =~ ^[0-9]+$ ]]; then
    echo "[ERROR] START_INTERVAL_SECONDS 必须是大于或等于 0 的整数。" >&2
    exit 2
fi
if [[ ! "${PAGE_CACHE_POLICY}" =~ ^(observe|evict-first)$ ]]; then
    echo "[ERROR] PAGE_CACHE_POLICY 只能是 observe 或 evict-first。" >&2
    exit 2
fi
if [[ "${SUITE_MODE}" == "1" ]]; then
    if (( REPEATS < 2 )); then
        echo "[ERROR] 统一实验至少需要 REPEATS=2：第 1 次 cold（冷缓存），后续为 warm（热缓存）。" >&2
        exit 2
    fi
    read -r -a suite_modes_array <<< "${SUITE_MODES}"
    if (( ${#suite_modes_array[@]} == 0 )); then
        echo "[ERROR] SUITE_MODES 至少需要一种模式。" >&2
        exit 2
    fi
    seen_modes=" "
    for mode in "${suite_modes_array[@]}"; do
        case "${mode}" in
            core_cache|core_timing|full_io|comm_graph) ;;
            *)
                echo "[ERROR] 不支持的 SUITE_MODES 项: ${mode}" >&2
                exit 2
                ;;
        esac
        if [[ "${seen_modes}" == *" ${mode} "* ]]; then
            echo "[ERROR] SUITE_MODES 中存在重复模式: ${mode}" >&2
            exit 2
        fi
        seen_modes+="${mode} "
    done
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
export LEVELS="${LEVELS:-3}"
export REFINES="${REFINES:-3}"
export MAXH="${MAXH:-1000.0}"
export MINH="${MINH:-0.0}"
export CORE_ONLY="${CORE_ONLY:-0}"
export CACHE_COUNTERS="${CACHE_COUNTERS:-0}"
export COMM_GRAPH="${COMM_GRAPH:-0}"
export LAUNCHER_STYLE="${LAUNCHER_STYLE:-yhrun}"
export LAUNCHER="${LAUNCHER:-${LAUNCHER_STYLE}}"
export LAUNCHER_EXTRA_ARGS="${LAUNCHER_EXTRA_ARGS:---mpi=pmix}"
export EXTRA_APP_ARGS="${EXTRA_APP_ARGS:-}"
export CPU_BIND="${CPU_BIND:-cores}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export LOAD_CLUSTER_ENV="${LOAD_CLUSTER_ENV:-1}"
export SERIALIZE_JOBS START_INTERVAL_SECONDS SUITE_MODE SUITE_MODES PAGE_CACHE_POLICY PAGE_CACHE_STRICT COMM_GRAPH

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
    echo "repeat_roles=repeat_01:cold repeat_02..${REPEATS}:warm"
    echo "ranks_per_node=${RANKS_PER_NODE}"
    echo "partition=${PARTITION}"
    echo "mesh_executable=${MESH_EXECUTABLE}"
    echo "input_mesh=${INPUT_MESH}"
    echo "levels=${LEVELS}"
    echo "refines=${REFINES}"
    echo "maxh=${MAXH}"
    echo "minh=${MINH}"
    if [[ "${SUITE_MODE}" == "1" ]]; then
        echo "core_only=per-mode"
        echo "cache_counters=per-mode"
        echo "communication_graph=per-mode"
    else
        echo "core_only=${CORE_ONLY}"
        echo "cache_counters=${CACHE_COUNTERS}"
        echo "communication_graph=${COMM_GRAPH}"
    fi
    echo "suite_mode=${SUITE_MODE}"
    echo "suite_modes=${SUITE_MODES}"
    echo "serialize_jobs=${SERIALIZE_JOBS}"
    echo "submission_unit=process-count"
    echo "start_interval_seconds=${START_INTERVAL_SECONDS}"
    echo "start_policy=independent-staggered"
    echo "analysis_dependency_policy=afterany-all"
    echo "page_cache_policy=${PAGE_CACHE_POLICY}"
    echo "page_cache_strict=${PAGE_CACHE_STRICT}"
    echo "launcher=${LAUNCHER}"
    echo "launcher_extra_args=${LAUNCHER_EXTRA_ARGS}"
    echo "cpu_bind=${CPU_BIND}"
    echo "sbatch_extra_args=${SBATCH_EXTRA_ARGS}"
    echo
    echo "processes nodes start_delay_s"
    job_index=0
    for processes in ${PROCESS_COUNTS}; do
        nodes=$(( (processes + RANKS_PER_NODE - 1) / RANKS_PER_NODE ))
        start_delay=$(( job_index * START_INTERVAL_SECONDS ))
        printf '%9d %5d %13d\n' "${processes}" "${nodes}" "${start_delay}"
        job_index=$(( job_index + 1 ))
    done
} > "${RUN_ROOT}/run_plan.txt"

job_ids=()
previous_job_id=""
previous_process=""
job_index=0
{
    echo
    echo "submission_sequence (错峰提交顺序)"
    echo "processes nodes start_delay_s job_id"
} >> "${RUN_ROOT}/run_plan.txt"

for processes in ${PROCESS_COUNTS}; do
    if (( processes <= 0 )); then
        echo "[ERROR] 非法进程数: ${processes}" >&2
        exit 2
    fi
    nodes=$(( (processes + RANKS_PER_NODE - 1) / RANKS_PER_NODE ))
    start_delay=$(( job_index * START_INTERVAL_SECONDS ))
    process_tag=$(printf 'p%05d' "${processes}")
    compute_script="${SCRIPT_DIR}/run_experiments.sh"
    export_spec="ALL,STRONG_SCALING_DIR=${SCRIPT_DIR},PROCESS_COUNTS=${processes},ANALYZE_AFTER_RUN=0,ANALYSIS_ONLY=0,DRY_RUN=0"
    if [[ "${SUITE_MODE}" == "1" ]]; then
        compute_script="${SCRIPT_DIR}/run_suite_process.sh"
        export_spec+=",SUITE_ROOT=${RUN_ROOT},SUITE_NAME=${RUN_NAME}"
    fi

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
        --export="${export_spec}"
    )
    if (( start_delay > 0 )); then
        submit_command+=( --begin="now+${start_delay}" )
    fi
    if [[ "${SERIALIZE_JOBS}" == "1" && -n "${previous_job_id}" ]]; then
        submit_command+=( --dependency="afterany:${previous_job_id}" )
    fi
    submit_command+=( "${sbatch_extra[@]}" "${compute_script}" )

    printf '[SUBMIT] processes=%s nodes=%s start_delay=%ss:' "${processes}" "${nodes}" "${start_delay}"
    printf ' %q' "${submit_command[@]}"
    printf '\n'
    if [[ "${DRY_RUN}" == "1" ]]; then
        if [[ "${SERIALIZE_JOBS}" == "1" && -n "${previous_process}" ]]; then
            echo "[ORDER] P=${processes} 将在 P=${previous_process} 结束后启动。"
        elif (( start_delay > 0 )); then
            echo "[STAGGER] P=${processes} 最早在提交后 ${start_delay} 秒具备启动资格。"
        fi
        printf '%9d %5d %13d %s\n' "${processes}" "${nodes}" "${start_delay}" "DRY_RUN" >> "${RUN_ROOT}/run_plan.txt"
        previous_process="${processes}"
        job_index=$(( job_index + 1 ))
        continue
    fi

    job_result=$("${submit_command[@]}")
    job_id="${job_result%%;*}"
    if [[ ! "${job_id}" =~ ^[0-9]+$ ]]; then
        echo "[ERROR] 无法解析 sbatch 作业号: ${job_result}" >&2
        exit 2
    fi
    job_ids+=("${job_id}")
    previous_job_id="${job_id}"
    previous_process="${processes}"
    printf '%9d %5d %13d %s\n' "${processes}" "${nodes}" "${start_delay}" "${job_id}" >> "${RUN_ROOT}/run_plan.txt"
    echo "[QUEUED] ${process_tag} -> job ${job_id}, start_delay=${start_delay}s"
    job_index=$(( job_index + 1 ))
done

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[OK] 提交 dry-run 完成；计划文件: ${RUN_ROOT}/run_plan.txt"
    exit 0
fi

if [[ "${SERIALIZE_JOBS}" == "1" ]]; then
    dependency="afterany:${previous_job_id}"
else
    dependency="afterany"
    for job_id in "${job_ids[@]}"; do
        dependency+=":${job_id}"
    done
fi

analysis_script="${SCRIPT_DIR}/run_experiments.sh"
analysis_export="ALL,STRONG_SCALING_DIR=${SCRIPT_DIR},ANALYSIS_ONLY=1,ANALYZE_AFTER_RUN=1,LOAD_CLUSTER_ENV=0,DRY_RUN=0"
if [[ "${SUITE_MODE}" == "1" ]]; then
    analysis_script="${SCRIPT_DIR}/run_suite_analysis.sh"
    analysis_export="ALL,STRONG_SCALING_DIR=${SCRIPT_DIR},SUITE_ROOT=${RUN_ROOT},LOAD_CLUSTER_ENV=0"
fi

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
    --export="${analysis_export}"
)
analysis_command+=( "${sbatch_extra[@]}" "${analysis_script}" )
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
if [[ "${SUITE_MODE}" == "1" ]]; then
    echo "完成后先看: ${RUN_ROOT}/analysis/suite_report.txt"
else
    echo "完成后先看: ${RUN_ROOT}/analysis/scaling_report.txt"
fi
echo "============================================================"
