#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
export RUN_ROOT="${RUN_ROOT:-${REPOSITORY_ROOT}/strong_scaling_results/mesh_algorithms_$(date +%Y%m%d-%H%M%S)}"
export RANKS_PER_NODE="${RANKS_PER_NODE:-16}"
export START_EPOCH="${START_EPOCH:-$(( $(date +%s)+${START_DELAY_SECONDS:-120} ))}"
PROCESS_COUNTS="${PROCESS_COUNTS:-16 32 64}"
SBATCH_COMMAND="${SBATCH_COMMAND:-yhbatch}"
PARTITION="${PARTITION:-mt_module}"
DRY_RUN="${DRY_RUN:-0}"
# Start with a small scale; request the production process counts explicitly.
[[ "${RANKS_PER_NODE}" =~ ^[1-9][0-9]*$ && "${START_EPOCH}" =~ ^[0-9]+$ ]] || exit 2
read -r -a counts <<< "${PROCESS_COUNTS//,/ }"
read -r -a batch_extra <<< "${SBATCH_EXTRA_ARGS:-}"
((${#counts[@]}>0)) || exit 2
for p in "${counts[@]}"; do [[ "${p}" =~ ^[1-9][0-9]*$ ]] || exit 2; done
mkdir -p "${RUN_ROOT}"
printf '%s\n' "${counts[@]}" > "${RUN_ROOT}/requested_process_counts.txt"
failures=0
for p in "${counts[@]}"; do
    nodes=$(( (p+RANKS_PER_NODE-1)/RANKS_PER_NODE ))
    mkdir -p "${RUN_ROOT}/p${p}"
    export PROCESS_COUNT="${p}"
    command=("${SBATCH_COMMAND}" --parsable -p "${PARTITION}" -N "${nodes}" -n "${p}"
             --ntasks-per-node "${RANKS_PER_NODE}" --export=ALL --job-name "mesh_p${p}"
             --output "${RUN_ROOT}/p${p}/job-%j.log" "${batch_extra[@]}" "${SCRIPT_DIR}/run_experiments.sh")
    if [[ "${DRY_RUN}" == 1 ]]; then
        printf '%q ' "${command[@]}"; printf '\n'
    else
        if jobid=$("${command[@]}"); then
            printf '%s\t%s\n' "${p}" "${jobid}" >> "${RUN_ROOT}/submitted_jobs.tsv"
        else
            failures=$((failures+1));printf '%s\n' "${p}" >> "${RUN_ROOT}/failed_submissions.txt"
        fi
    fi
done
printf 'Results: %s\n' "${RUN_ROOT}"
((failures==0)) || exit 1
