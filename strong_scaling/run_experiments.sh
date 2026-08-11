#!/usr/bin/env bash
set -Eeuo pipefail

# sbatch/yhbatch may copy this file to a per-job spool directory before
# execution.  In that case BASH_SOURCE[0] points at the copied slurm_script,
# so use the original directory explicitly exported by the submitter.
SCRIPT_DIR="$(cd "${STRONG_SCALING_DIR:-$(dirname "${BASH_SOURCE[0]}")}" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Use the same compiler/MPI/runtime libraries as build_project.sh and
# cjz_nodsp_copy.sh.  Set LOAD_CLUSTER_ENV=0 for a non-cluster dry run.
LOAD_CLUSTER_ENV="${LOAD_CLUSTER_ENV:-1}"
if [[ "${LOAD_CLUSTER_ENV}" == "1" ]]; then
    # shellcheck disable=SC1091
    source "${SCRIPT_DIR}/cluster_env.sh"
fi

MESH_EXECUTABLE="${MESH_EXECUTABLE:-${REPOSITORY_ROOT}/build/mesh_occ_mpi/mesh_occ_mpi}"
INPUT_MESH="${INPUT_MESH:-${REPOSITORY_ROOT}/inputData/wholewall3solid.STEP}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPOSITORY_ROOT}/strong_scaling_results}"
EXPERIMENT="${EXPERIMENT:-wholewall_l4_r3_core}"
BATCH_ID="${BATCH_ID:-$(date +%Y%m%d-%H%M%S)}"
RUN_NAME="${RUN_NAME:-${EXPERIMENT}_${BATCH_ID}}"
RUN_ROOT="${OUTPUT_ROOT}/${RUN_NAME}"

# Defaults follow cjz_nodsp_copy.sh and the 16--256 process experiment shown
# in the reference figure.  Every value can still be overridden at launch.
PROCESS_COUNTS="${PROCESS_COUNTS:-1 2 4 8 16 32 64 128 256}"
REPEATS="${REPEATS:-3}"
RANKS_PER_NODE="${RANKS_PER_NODE:-1}"
LAUNCHER_STYLE="${LAUNCHER_STYLE:-yhrun}"
LAUNCHER="${LAUNCHER:-${LAUNCHER_STYLE}}"
PARTITION="${PARTITION:-mt_module}"
CPU_BIND="${CPU_BIND:-cores}"
LEVELS="${LEVELS:-2}"
REFINES="${REFINES:-2}"
MAXH="${MAXH:-1000.0}"
MINH="${MINH:-0.0}"
CORE_ONLY="${CORE_ONLY:-1}"
CACHE_COUNTERS="${CACHE_COUNTERS:-1}"
PAGE_CACHE_POLICY="${PAGE_CACHE_POLICY:-observe}"
PAGE_CACHE_STRICT="${PAGE_CACHE_STRICT:-0}"
LAUNCHER_EXTRA_ARGS="${LAUNCHER_EXTRA_ARGS:---mpi=pmix}"
EXTRA_APP_ARGS="${EXTRA_APP_ARGS:-}"
ANALYZE_AFTER_RUN="${ANALYZE_AFTER_RUN:-1}"
ANALYSIS_ONLY="${ANALYSIS_ONLY:-0}"
DRY_RUN="${DRY_RUN:-0}"

# Keep resources per MPI (消息传递接口) rank fixed for strong scaling.
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"

if [[ ! "${RUN_NAME}" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "[ERROR] RUN_NAME 只能包含字母、数字、下划线和连字符: ${RUN_NAME}" >&2
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
for numeric_value in "${REPEATS}" "${RANKS_PER_NODE}"; do
    if [[ ! "${numeric_value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "[ERROR] REPEATS 和 RANKS_PER_NODE 必须是正整数。" >&2
        exit 2
    fi
done
if [[ ! "${PAGE_CACHE_POLICY}" =~ ^(observe|evict-first)$ ]]; then
    echo "[ERROR] PAGE_CACHE_POLICY 只能是 observe 或 evict-first。" >&2
    exit 2
fi
if [[ ! "${PAGE_CACHE_STRICT}" =~ ^[01]$ ]]; then
    echo "[ERROR] PAGE_CACHE_STRICT 只能是 0 或 1。" >&2
    exit 2
fi

mkdir -p \
    "${RUN_ROOT}/application_output" \
    "${RUN_ROOT}/commands" \
    "${RUN_ROOT}/environment" \
    "${RUN_ROOT}/launcher_logs" \
    "${RUN_ROOT}/page_cache_logs" \
    "${RUN_ROOT}/status"

if [[ "${ANALYSIS_ONLY}" == "1" ]]; then
    echo "[ANALYZE] ${RUN_ROOT}"
    python3 "${SCRIPT_DIR}/analyze_results.py" "${RUN_ROOT}"
    echo "[OK] 汇总报告: ${RUN_ROOT}/analysis/scaling_report.txt"
    exit 0
fi

if [[ "${DRY_RUN}" != "1" ]]; then
    if [[ ! -x "${MESH_EXECUTABLE}" ]]; then
        echo "[ERROR] 可执行文件不存在或不可执行: ${MESH_EXECUTABLE}" >&2
        echo "        请先运行: bash strong_scaling/build_profiled.sh" >&2
        exit 2
    fi
    if [[ ! -f "${INPUT_MESH}" ]]; then
        echo "[ERROR] 固定输入文件不存在: ${INPUT_MESH}" >&2
        exit 2
    fi
    if ! command -v "${LAUNCHER}" >/dev/null 2>&1; then
        echo "[ERROR] 找不到启动器 ${LAUNCHER}。" >&2
        exit 2
    fi
fi

read -r -a launcher_extra <<< "${LAUNCHER_EXTRA_ARGS}"
read -r -a app_extra <<< "${EXTRA_APP_ARGS}"

validate_mpi_runtime()
{
    local dependency_output mpi_library pmix_library

    dependency_output="$(ldd "${MESH_EXECUTABLE}")"
    mpi_library="$(awk '$1 ~ /^libmpi[.]so/ && $2 == "=>" { print $3; exit }' <<< "${dependency_output}")"
    pmix_library="$(awk '$1 ~ /^libpmix[.]so/ && $2 == "=>" { print $3; exit }' <<< "${dependency_output}")"

    if [[ -z "${mpi_library}" || -z "${pmix_library}" ]]; then
        echo "[ERROR] 无法从 ldd 输出中解析 libmpi/libpmix；请检查可执行文件的 MPI 链接。" >&2
        return 1
    fi
    if [[ "${mpi_library}" != "${MPI_X_LIB}/"* ]]; then
        echo "[ERROR] MPI 运行库不属于 MPI-X: ${mpi_library}" >&2
        echo "        期望目录: ${MPI_X_LIB}" >&2
        return 1
    fi
    if [[ "${pmix_library}" != "${EXPECTED_PMIX_LIB_PREFIX}/"* ]]; then
        echo "[ERROR] PMIx 运行库来源异常: ${pmix_library}" >&2
        echo "        期望系统 PMIx 目录: ${EXPECTED_PMIX_LIB_PREFIX}" >&2
        echo "        请勿将 /vol8/home/hnu_lhz/cjz/aarch64-linux-gnu 加入 LD_LIBRARY_PATH。" >&2
        return 1
    fi

    echo "[ENV-OK] libmpi=${mpi_library}"
    echo "[ENV-OK] libpmix=${pmix_library}"
}

if [[ "${DRY_RUN}" != "1" && "${LOAD_CLUSTER_ENV}" == "1" ]]; then
    validate_mpi_runtime
fi

job_tag="job_${SLURM_JOB_ID:-local}_$(date +%Y%m%d-%H%M%S)_$$"
environment_file="${RUN_ROOT}/environment/${job_tag}.txt"
{
    echo "Strong-scaling environment (强扩展运行环境)"
    echo "================================================"
    echo "time=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "hostname=$(hostname)"
    echo "repository_root=${REPOSITORY_ROOT}"
    echo "git_commit=$(git -C "${REPOSITORY_ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "slurm_job_id=${SLURM_JOB_ID:-N/A}"
    echo "slurm_job_nodelist=${SLURM_JOB_NODELIST:-N/A}"
    echo "run_name=${RUN_NAME}"
    echo "process_counts=${PROCESS_COUNTS}"
    echo "repeats=${REPEATS}"
    echo "ranks_per_node=${RANKS_PER_NODE}"
    echo "launcher=${LAUNCHER}"
    echo "launcher_extra_args=${LAUNCHER_EXTRA_ARGS}"
    echo "cpu_bind=${CPU_BIND:-N/A}"
    echo "omp_num_threads=${OMP_NUM_THREADS}"
    echo "core_only=${CORE_ONLY}"
    echo "cache_counters=${CACHE_COUNTERS}"
    echo "page_cache_policy=${PAGE_CACHE_POLICY}"
    echo "page_cache_strict=${PAGE_CACHE_STRICT}"
    echo "mesh_executable=${MESH_EXECUTABLE}"
    echo "input_mesh=${INPUT_MESH}"
    echo "levels=${LEVELS}"
    echo "refines=${REFINES}"
    echo "maxh=${MAXH}"
    echo "minh=${MINH}"
    echo "GCCHOME=${GCCHOME-}"
    echo "MPICH_CC=${MPICH_CC-}"
    echo "MPICH_CXX=${MPICH_CXX-}"
    echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH-}"
    if [[ -r /proc/sys/kernel/perf_event_paranoid ]]; then
        echo "perf_event_paranoid=$(< /proc/sys/kernel/perf_event_paranoid)"
    fi
    echo
    echo "--- uname -a ---"
    uname -a || true
    if command -v lscpu >/dev/null 2>&1; then
        echo
        echo "--- lscpu ---"
        lscpu 2>&1 || true
        echo
        echo "--- lscpu --caches ---"
        lscpu --caches 2>&1 || true
    fi
    if type module >/dev/null 2>&1; then
        echo
        echo "--- module list ---"
        module list 2>&1 || true
    fi
    if [[ -x "${MESH_EXECUTABLE}" ]] && command -v ldd >/dev/null 2>&1; then
        echo
        echo "--- ldd mesh_occ_mpi ---"
        ldd "${MESH_EXECUTABLE}" || true
    fi
} > "${environment_file}"

if [[ ! -e "${RUN_ROOT}/run_plan.txt" ]]; then
    plan_tmp="${RUN_ROOT}/.run_plan_$$.tmp"
    {
        echo "Strong Scaling Run Plan (强扩展实验计划)"
        echo "========================================"
        echo "experiment=${EXPERIMENT}"
        echo "batch_id=${BATCH_ID}"
        echo "run_name=${RUN_NAME}"
        echo "result_directory=${RUN_ROOT}"
        echo "process_counts=${PROCESS_COUNTS}"
        echo "repeats=${REPEATS}"
        echo "ranks_per_node=${RANKS_PER_NODE}"
        echo "levels=${LEVELS}"
        echo "refines=${REFINES}"
        echo "maxh=${MAXH}"
        echo "minh=${MINH}"
        echo "core_only=${CORE_ONLY}"
        echo "cache_counters=${CACHE_COUNTERS}"
        echo "page_cache_policy=${PAGE_CACHE_POLICY}"
        echo "repeat_roles=repeat_01:cold repeat_02..${REPEATS}:warm"
    } > "${plan_tmp}"
    ln "${plan_tmp}" "${RUN_ROOT}/run_plan.txt" 2>/dev/null || true
    rm -f "${plan_tmp}"
fi

echo "============================================================"
echo "Strong scaling (强扩展) run: ${RUN_NAME}"
echo "Processes: ${PROCESS_COUNTS}; repeats: ${REPEATS}"
echo "Page cache (页缓存): ${PAGE_CACHE_POLICY}; repeat 1=cold, repeat 2+=warm"
echo "Result directory: ${RUN_ROOT}"
echo "Environment: ${environment_file}"
echo "============================================================"

overall_status=0
for processes in ${PROCESS_COUNTS}; do
    if (( processes <= 0 )); then
        echo "[ERROR] 非法进程数: ${processes}" >&2
        exit 2
    fi
    nodes=$(( (processes + RANKS_PER_NODE - 1) / RANKS_PER_NODE ))

    for repeat in $(seq 1 "${REPEATS}"); do
        process_tag=$(printf 'p%05d' "${processes}")
        repeat_tag=$(printf 'repeat_%02d' "${repeat}")
        run_tag="${process_tag}_${repeat_tag}"
        app_output="${RUN_ROOT}/application_output/${process_tag}/${repeat_tag}/"
        log_file="${RUN_ROOT}/launcher_logs/${run_tag}.log"
        command_file="${RUN_ROOT}/commands/${run_tag}.command"
        status_file="${RUN_ROOT}/status/${run_tag}.tsv"
        page_cache_log="${RUN_ROOT}/page_cache_logs/${run_tag}.log"
        mkdir -p "${app_output}"

        cache_state="warm"
        if (( repeat == 1 )); then
            if [[ "${PAGE_CACHE_POLICY}" == "evict-first" ]]; then
                cache_state="cold"
            else
                cache_state="cold_candidate"
            fi
        fi

        launcher_command=("${LAUNCHER}")
        case "${LAUNCHER_STYLE}" in
            yhrun|srun)
                if [[ -n "${PARTITION}" ]]; then
                    launcher_command+=( -p "${PARTITION}" )
                fi
                launcher_command+=( -N "${nodes}" -n "${processes}" )
                if [[ -n "${CPU_BIND}" && "${CPU_BIND}" != "none" ]]; then
                    launcher_command+=( "--cpu-bind=${CPU_BIND}" )
                fi
                ;;
            mpirun|mpiexec)
                launcher_command+=( -np "${processes}" )
                ;;
            *)
                echo "[ERROR] 不支持 LAUNCHER_STYLE=${LAUNCHER_STYLE}" >&2
                exit 2
                ;;
        esac
        launcher_command+=( "${launcher_extra[@]}" )

        application_command=(
            "${MESH_EXECUTABLE}"
            -i "${INPUT_MESH}"
            -o "${app_output}"
            -l "${LEVELS}"
            -r "${REFINES}"
            --maxh "${MAXH}"
            --minh "${MINH}"
            -v
            -adj
            --profile
            --profile-dir "${OUTPUT_ROOT}"
            --profile-experiment "${RUN_NAME}"
            --profile-repeat "${repeat}"
        )
        if [[ "${CORE_ONLY}" == "1" ]]; then
            application_command+=( --profile-core-only )
        fi
        if [[ "${CACHE_COUNTERS}" == "1" ]]; then
            application_command+=( --profile-cache )
        fi
        application_command+=( "${app_extra[@]}" )

        full_command=("${launcher_command[@]}" "${application_command[@]}")
        {
            printf '# processes=%s nodes=%s repeat=%s cache_state=%s\n' \
                "${processes}" "${nodes}" "${repeat}" "${cache_state}"
            if (( repeat == 1 )) && [[ "${PAGE_CACHE_POLICY}" == "evict-first" ]]; then
                printf '# page-cache preparation: same launcher + prepare_page_cache.py\n'
            fi
            printf '%q ' "${full_command[@]}"
            printf '\n'
        } > "${command_file}"

        echo "[RUN] processes=${processes} nodes=${nodes} repeat=${repeat}/${REPEATS} cache=${cache_state}"

        if (( repeat == 1 )) && [[ "${PAGE_CACHE_POLICY}" == "evict-first" ]]; then
            page_cache_command=(
                "${launcher_command[@]}"
                python3 "${SCRIPT_DIR}/prepare_page_cache.py"
                --input "${INPUT_MESH}"
                --executable "${MESH_EXECUTABLE}"
            )
            {
                printf '# page_cache_command:'
                printf ' %q' "${page_cache_command[@]}"
                printf '\n'
            } >> "${command_file}"
            if [[ "${DRY_RUN}" == "1" ]]; then
                printf '[DRY-RUN CACHE]'
                printf ' %q' "${page_cache_command[@]}"
                printf '\n'
            else
                echo "[CACHE] 在每个 rank 所在节点请求丢弃输入、程序和动态库页缓存。"
                set +e
                "${page_cache_command[@]}" 2>&1 | tee "${page_cache_log}"
                page_cache_status=${PIPESTATUS[0]}
                set -e
                if (( page_cache_status != 0 )); then
                    echo "[WARN] 页缓存准备失败，退出码 ${page_cache_status}；冷启动将仅按首次运行标记。" >&2
                    cache_state="cold_candidate"
                    echo "# effective_cache_state=${cache_state}" >> "${command_file}"
                    if [[ "${PAGE_CACHE_STRICT}" == "1" ]]; then
                        exit "${page_cache_status}"
                    fi
                fi
            fi
        fi

        if [[ "${DRY_RUN}" == "1" ]]; then
            printf '[DRY-RUN]'
            printf ' %q' "${full_command[@]}"
            printf '\n'
            printf 'processes\tnodes\trepeat\tcache_state\tstate\texit_code\truntime_s\tlog\n' > "${status_file}"
            printf '%s\t%s\t%s\t%s\tDRY_RUN\t0\t0\t%s\n' \
                "${processes}" "${nodes}" "${repeat}" "${cache_state}" "${log_file}" >> "${status_file}"
            continue
        fi

        start_epoch=$(date +%s)
        set +e
        "${full_command[@]}" 2>&1 | tee "${log_file}"
        run_status=${PIPESTATUS[0]}
        set -e
        runtime=$(( $(date +%s) - start_epoch ))

        state="COMPLETED"
        if (( run_status != 0 )); then
            state="FAILED"
        fi
        printf 'processes\tnodes\trepeat\tcache_state\tstate\texit_code\truntime_s\tlog\n' > "${status_file}"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "${processes}" "${nodes}" "${repeat}" "${cache_state}" "${state}" "${run_status}" "${runtime}" "${log_file}" \
            >> "${status_file}"

        if (( run_status != 0 )); then
            echo "[ERROR] ${run_tag} 失败，退出码 ${run_status}；日志: ${log_file}" >&2
            if (( overall_status == 0 )); then
                overall_status="${run_status}"
            fi
            echo "[CONTINUE] 记录失败并继续后续重复实验。" >&2
        fi
    done
done

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[OK] dry-run 完成；命令清单: ${RUN_ROOT}/commands/"
elif [[ "${ANALYZE_AFTER_RUN}" == "1" ]]; then
    set +e
    python3 "${SCRIPT_DIR}/analyze_results.py" "${RUN_ROOT}"
    analysis_status=$?
    set -e
    if (( analysis_status == 0 )); then
        echo
        cat "${RUN_ROOT}/analysis/scaling_report.txt"
        echo
        echo "[OK] 全部原始数据和分析结果: ${RUN_ROOT}"
    else
        echo "[ERROR] 分析失败，exit=${analysis_status}" >&2
        if (( overall_status == 0 )); then
            overall_status="${analysis_status}"
        fi
    fi
else
    echo "[OK] 本作业运行完成；等待其他规模结束后统一分析: ${RUN_ROOT}"
fi

exit "${overall_status}"
