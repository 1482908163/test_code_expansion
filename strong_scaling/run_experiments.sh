#!/usr/bin/env bash
set -Eeuo pipefail
# Slurm/yhbatch executes a spool copy such as /tmp/slurmd/job*/slurm_script.
# The submitter exports the original directory so worker jobs can find the
# companion environment and analysis scripts beside the repository source.
invoked_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_DIR="${STRONG_SCALING_DIR:-${invoked_script_dir}}"
SCRIPT_DIR="$(cd "${SCRIPT_DIR}" && pwd)"
export STRONG_SCALING_DIR="${SCRIPT_DIR}"

# ============================================================================
# 统一实验配置区：通常只需修改 EXPERIMENT_PRESET，然后直接运行本脚本。
#   pilot      : 1/2/4 节点，小规模正确性与流程预检
#   production : 64/128/256/512 节点，复现正式大规模实验配置
# 环境变量仍可覆盖这些默认值，主要供作业脚本内部传递及断点续跑使用。
# ============================================================================
EXPERIMENT_PRESET="${EXPERIMENT_PRESET:-pilot}"
case "${EXPERIMENT_PRESET}" in
    pilot)
        default_process_counts="16 32 64"
        default_levels=1
        default_refines=1
        default_verify_faces=1
        ;;
    production)
        default_process_counts="1024 2048 4096 8192"
        default_levels=3
        default_refines=3
        default_verify_faces=0
        ;;
    *) echo "EXPERIMENT_PRESET must be pilot or production" >&2; exit 2 ;;
esac

PROCESS_COUNTS="${PROCESS_COUNTS:-${default_process_counts}}"
ALGORITHMS="${ALGORITHMS:-baseline balance sparse combined}"
TIMING_MODES="${TIMING_MODES:-natural split}"
RANKS_PER_NODE="${RANKS_PER_NODE:-16}"
REPEATS="${REPEATS:-5}"
WARMUPS="${WARMUPS:-1}"
LEVELS="${LEVELS:-${default_levels}}"
REFINES="${REFINES:-${default_refines}}"
MAXH="${MAXH:-1000}"
MINH="${MINH:-0}"
VERIFY_FACES="${VERIFY_FACES:-${default_verify_faces}}"
CLEANUP_RESULTS="${CLEANUP_RESULTS:-1}"
RESUME="${RESUME:-1}"
BALANCE_SWEEPS="${BALANCE_SWEEPS:-4}"
CUT_GROWTH="${CUT_GROWTH:-0.05}"
COST_WEIGHTS="${COST_WEIGHTS:-1,1,1,1}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-7200}"
START_DELAY_SECONDS="${START_DELAY_SECONDS:-120}"
PARTITION="${PARTITION:-mt_module}"
SBATCH_COMMAND="${SBATCH_COMMAND:-yhbatch}"
SBATCH_EXTRA_ARGS="${SBATCH_EXTRA_ARGS:-}"
MPI_LAUNCHER="${MPI_LAUNCHER:-yhrun}"
MPI_EXTRA_ARGS="${MPI_EXTRA_ARGS:---mpi=pmix}"
DRY_RUN="${DRY_RUN:-0}"
INPUT_PATH="${INPUT_PATH:-/vol8/home/hnu_lhz/cjz/NETGEN/test_code_expansion/inputData/wholewall3solid.STEP}"

export EXPERIMENT_PRESET PROCESS_COUNTS ALGORITHMS TIMING_MODES RANKS_PER_NODE
export REPEATS WARMUPS LEVELS REFINES MAXH MINH VERIFY_FACES CLEANUP_RESULTS RESUME
export BALANCE_SWEEPS CUT_GROWTH COST_WEIGHTS TIMEOUT_SECONDS START_DELAY_SECONDS
export PARTITION SBATCH_COMMAND SBATCH_EXTRA_ARGS MPI_LAUNCHER MPI_EXTRA_ARGS DRY_RUN INPUT_PATH

# 登录节点：转入提交驱动；计算作业：继续执行下方 worker（工作进程）逻辑。
if [[ "${MESH_EXPERIMENT_WORKER:-0}" != 1 ]]; then
    export MESH_EXPERIMENT_DRIVER_READY=1
    exec "${SCRIPT_DIR}/submit_experiments.sh" "$@"
fi

[[ -r "${SCRIPT_DIR}/cluster_env.sh" && -r "${SCRIPT_DIR}/analyze_results.py" ]] || {
    echo "Cannot locate strong_scaling source directory: ${SCRIPT_DIR}" >&2
    exit 2
}
source "${SCRIPT_DIR}/cluster_env.sh"
PROCESS_COUNT="${PROCESS_COUNT:?worker requires PROCESS_COUNT}"
RUN_ROOT="${RUN_ROOT:?worker requires RUN_ROOT}"
BINARY="${BINARY:-${REPOSITORY_ROOT}/build/mesh_occ_mpi/mesh_occ_mpi}"
cleanup_args=()
case "${CLEANUP_RESULTS}" in
    0) cleanup_args+=(--keep-artifacts) ;;
    1) ;;
    *) echo "CLEANUP_RESULTS must be 0 or 1" >&2; exit 2 ;;
esac
command -v flock >/dev/null || { echo "Missing flock (run directory locking)" >&2; exit 2; }
read -r -a algorithms <<< "${ALGORITHMS//,/ }"
read -r -a timings <<< "${TIMING_MODES//,/ }"
read -r -a mpi_extra <<< "${MPI_EXTRA_ARGS}"
for a in "${algorithms[@]}"; do
    case "$a" in baseline|balance|sparse|combined) ;; *) echo "Unknown algorithm: $a" >&2; exit 2;; esac
done
for mode in "${timings[@]}"; do
    case "$mode" in natural|split) ;; *) echo "Unknown timing mode: $mode" >&2; exit 2;; esac
done
[[ "${PROCESS_COUNT}" =~ ^[1-9][0-9]*$ && "${REPEATS}" =~ ^[1-9][0-9]*$ &&
   "${WARMUPS}" =~ ^[0-9]+$ && "${RANKS_PER_NODE}" =~ ^[1-9][0-9]*$ &&
   "${TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid run counts" >&2; exit 2; }
[[ -x "${BINARY}" && -f "${INPUT_PATH}" ]] || { echo "Missing executable or input: ${BINARY}, ${INPUT_PATH}" >&2; exit 2; }
(( ${#algorithms[@]} > 0 && ${#timings[@]} > 0 )) || exit 2

# A stale executable silently ignores the new research arguments in older
# revisions.  That produces ordinary mesh artifacts but no rank profile, and
# the failure would otherwise be discovered only after many expensive runs.
binary_error=""
for marker in "--profile-core-only" "--algorithm" "research_1" "global_id_bits"; do
    if ! LC_ALL=C grep -aFq -- "${marker}" "${BINARY}"; then
        binary_error="missing capability marker ${marker}"
        break
    fi
done
if [[ -z "${binary_error}" ]]; then
    newer_source="$(find "${REPOSITORY_ROOT}/mesh_occ_mpi" -type f \
        \( -name '*.cpp' -o -name '*.h' -o -name 'CMakeLists.txt' \) \
        -newer "${BINARY}" -print -quit)"
    [[ -z "${newer_source}" ]] || binary_error="source is newer than executable: ${newer_source}"
fi
if [[ -n "${binary_error}" ]]; then
    echo "ERROR: stale/incompatible mesh executable (${binary_error})." >&2
    echo "Rebuild first: cmake --build \"${REPOSITORY_ROOT}/build\" -j" >&2
    echo "No experiment was started and no result directory needs analysis." >&2
    exit 2
fi
# Every submitted job has the SAME earliest launch time; no serial dependencies.
while (( $(date +%s) < ${START_EPOCH:-0} )); do
    remaining=$(( START_EPOCH - $(date +%s) ))
    (( remaining > 30 )) && remaining=30
    (( remaining > 0 )) && sleep "${remaining}"
done
pdir="${RUN_ROOT}/p${PROCESS_COUNT}"
mkdir -p "${pdir}"
status_file="${pdir}/run_status.tsv"
[[ -f "${status_file}" ]] || printf 'algorithm\ttiming\trepeat\texit_code\tdirectory\n' > "${status_file}"
failure_file="${pdir}/failures.log"
launch=("${MPI_LAUNCHER}" "${mpi_extra[@]}" -n "${PROCESS_COUNT}")
case "$(basename "${MPI_LAUNCHER}")" in yhrun|srun)
    launch+=(-N "$(( (PROCESS_COUNT+RANKS_PER_NODE-1)/RANKS_PER_NODE ))") ;;
esac
common=(-i "${INPUT_PATH}" -l "${LEVELS}" -r "${REFINES}" --maxh "${MAXH}" --minh "${MINH}" -adj
        --balance-sweeps "${BALANCE_SWEEPS:-4}" --cut-growth "${CUT_GROWTH:-0.05}" --cost-weights "${COST_WEIGHTS:-1,1,1,1}")
# Correctness checks deliberately cannot be combined with timing collection.
if [[ "${VERIFY_FACES}" == 1 ]]; then
    for a in sparse combined; do
        checkdir="${pdir}/verify_${a}"
        mkdir -p "${checkdir}"
        timeout "${TIMEOUT_SECONDS}" "${launch[@]}" "${BINARY}" "${common[@]}" \
            --algorithm "${a}" --verify-faces -v -o "${checkdir}/" > "${checkdir}/run.log" 2>&1 || exit $?
    done
fi
config_text="$(printf '%s\n' "${PROCESS_COUNT}" "${ALGORITHMS}" "${TIMING_MODES}" "${REPEATS}" "${WARMUPS}" "${common[@]}" "OMP_NUM_THREADS=${OMP_NUM_THREADS}"; sha256sum "${BINARY}" "${INPUT_PATH}")"
if [[ -f "${pdir}/configuration.txt" && "$(cat "${pdir}/configuration.txt")" != "${config_text}" ]]; then
    echo "Existing results use another configuration; choose a new RUN_ROOT." >&2
    exit 2
fi
printf '%s\n' "${config_text}" > "${pdir}/configuration.txt"
python3 - "${pdir}/plan.json" "${PROCESS_COUNT}" "${REPEATS}" "${ALGORITHMS}" "${TIMING_MODES}" <<'PLAN'
import json,pathlib,sys
path,n,repeats,algorithms,timings=sys.argv[1:]
pathlib.Path(path).write_text(json.dumps(dict(ranks=int(n), repeats=int(repeats),
    algorithms=algorithms.replace(',', ' ').split(), timings=timings.replace(',', ' ').split())))
PLAN
failures=0
# Negative/zero repeats are warmups. Rotate mode order to avoid always giving
# one algorithm the first file-cache/allocator state. No cache eviction code.
for ((rep=1-WARMUPS;rep<=REPEATS;++rep)); do
    for mode in "${timings[@]}"; do
        for ((j=0;j<${#algorithms[@]};++j)); do
            a="${algorithms[$(( (j+rep+WARMUPS-1)%${#algorithms[@]} ))]}"
            out="${pdir}/${a}_${mode}/repeat_${rep}"
            mkdir -p "${out}"
            exec 9>>"${out}/.run.lock"
            if ! flock -n 9; then
                echo "Run directory is busy: ${out}" >&2
                failures=$((failures+1));exec 9>&-;continue
            fi
            if [[ "${RESUME}" == 1 && -f "${out}/SUCCESS" &&
                  ( -f "${out}/rank_profiles.jsonl" || -f "${out}/rank_profiles.jsonl.gz" ) ]]; then
                exec 9>&-;continue
            fi
            rm -f "${out}/SUCCESS" "${out}/rank_profiles.jsonl" "${out}/rank_profiles.jsonl.gz" "${out}/run.log.gz"
            touch "${out}/RUNNING"
            args=("${common[@]}" --algorithm "${a}" --profile-core-only --profile-dir "${out}"
                  --profile-experiment "${a}" --profile-repeat "${rep}" -o "${out}/mesh/")
            [[ "${mode}" == natural ]] && args+=(--profile-natural)
            rc=0
            timeout "${TIMEOUT_SECONDS}" "${launch[@]}" "${BINARY}" "${args[@]}" > "${out}/run.log" 2>&1 || rc=$?
            rm -f "${out}/RUNNING"
            exec 9>&-
            if ((rc==0)); then
                finalize_error=""
                if ! finalize_error="$(python3 "${SCRIPT_DIR}/analyze_results.py" "${out}" --finish-run "${cleanup_args[@]}" 2>&1)"; then
                    rc=1
                    printf '%s/%s repeat %s: %s\n' "${a}" "${mode}" "${rep}" "${finalize_error}" >> "${failure_file}"
                fi
            fi
            if ((rc!=0)); then
                failures=$((failures+1))
            fi
            printf '%s\t%s\t%s\t%s\t%s\n' "${a}" "${mode}" "${rep}" "${rc}" "${out}" >> "${status_file}"
        done
    done
done
python3 "${SCRIPT_DIR}/analyze_results.py" "${pdir}" "${cleanup_args[@]}" || exit $?
((failures==0)) || exit 1
