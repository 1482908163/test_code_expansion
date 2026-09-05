#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/cluster_env.sh"
PROCESS_COUNT="${PROCESS_COUNT:-${SLURM_NTASKS:-16}}"
RUN_ROOT="${RUN_ROOT:?set RUN_ROOT through submit_experiments.sh}"
BINARY="${BINARY:-${REPOSITORY_ROOT}/build/mesh_occ_mpi/mesh_occ_mpi}"
INPUT_PATH="${INPUT_PATH:-/vol8/home/hnu_lhz/cjz/NETGEN/test_code_expansion/inputData/wholewall3solid.STEP}"
ALGORITHMS="${ALGORITHMS:-baseline balance sparse combined}"
TIMING_MODES="${TIMING_MODES:-natural split}"
REPEATS="${REPEATS:-5}"
WARMUPS="${WARMUPS:-1}"
LEVELS="${LEVELS:-1}"
REFINES="${REFINES:-1}"
MAXH="${MAXH:-1000}"
MINH="${MINH:-10}"
MPI_LAUNCHER="${MPI_LAUNCHER:-yhrun}"
MPI_EXTRA_ARGS="${MPI_EXTRA_ARGS:---mpi=pmix}"
RANKS_PER_NODE="${RANKS_PER_NODE:-16}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-7200}"
VERIFY_FACES="${VERIFY_FACES:-0}"
RESUME="${RESUME:-1}"
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
            if [[ "${RESUME}" == 1 && -f "${out}/SUCCESS" && -f "${out}/rank_profiles.jsonl" ]]; then continue; fi
            mkdir -p "${out}"
            rm -f "${out}/SUCCESS"
            args=("${common[@]}" --algorithm "${a}" --profile-core-only --profile-dir "${out}"
                  --profile-experiment "${a}" --profile-repeat "${rep}" -o "${out}/mesh/")
            [[ "${mode}" == natural ]] && args+=(--profile-natural)
            rc=0
            timeout "${TIMEOUT_SECONDS}" "${launch[@]}" "${BINARY}" "${args[@]}" > "${out}/run.log" 2>&1 || rc=$?
            if ((rc==0)) && [[ -s "${out}/rank_profiles.jsonl" ]]; then
                touch "${out}/SUCCESS"
            else
                ((rc==0)) && rc=1
                failures=$((failures+1))
            fi
            printf '%s\t%s\t%s\t%s\t%s\n' "${a}" "${mode}" "${rep}" "${rc}" "${out}" >> "${status_file}"
        done
    done
done
python3 "${SCRIPT_DIR}/analyze_results.py" "${pdir}" || exit $?
((failures==0)) || exit 1
