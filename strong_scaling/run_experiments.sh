#!/usr/bin/env bash
set -euo pipefail

# Required environment variables:
#   MESH_EXECUTABLE  Built mesh_occ_mpi executable
#   INPUT_MESH       Fixed STEP input used by every process count
#
# Common optional variables are documented in README.md.

: "${MESH_EXECUTABLE:?Set MESH_EXECUTABLE to the mesh_occ_mpi executable}"
: "${INPUT_MESH:?Set INPUT_MESH to the fixed STEP input file}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

OUTPUT_ROOT="${OUTPUT_ROOT:-${REPOSITORY_ROOT}/strong_scaling_results}"
PROFILE_ROOT="${PROFILE_ROOT:-${OUTPUT_ROOT}/profiles}"
EXPERIMENT="${EXPERIMENT:-wholewall_strong_scaling}"
BATCH_ID="${BATCH_ID:-$(date +%Y%m%d-%H%M%S)}"
PROCESS_COUNTS="${PROCESS_COUNTS:-1 2 4 8 16 32 64}"
REPEATS="${REPEATS:-3}"
RANKS_PER_NODE="${RANKS_PER_NODE:-16}"
LAUNCHER_STYLE="${LAUNCHER_STYLE:-yhrun}"
LAUNCHER="${LAUNCHER:-${LAUNCHER_STYLE}}"
PARTITION="${PARTITION:-}"
LEVELS="${LEVELS:-0}"
REFINES="${REFINES:-0}"
MAXH="${MAXH:-1000.0}"
MINH="${MINH:-10.0}"
CORE_ONLY="${CORE_ONLY:-1}"
CACHE_COUNTERS="${CACHE_COUNTERS:-1}"
LAUNCHER_EXTRA_ARGS="${LAUNCHER_EXTRA_ARGS:-}"
EXTRA_APP_ARGS="${EXTRA_APP_ARGS:-}"

# Keep per-rank CPU resources fixed unless the caller explicitly requests threads.
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"

mkdir -p "${PROFILE_ROOT}/${EXPERIMENT}/launcher_logs"
mkdir -p "${OUTPUT_ROOT}/application_output/${EXPERIMENT}"

read -r -a launcher_extra <<< "${LAUNCHER_EXTRA_ARGS}"
read -r -a app_extra <<< "${EXTRA_APP_ARGS}"

for processes in ${PROCESS_COUNTS}; do
    nodes=$(( (processes + RANKS_PER_NODE - 1) / RANKS_PER_NODE ))
    for repeat in $(seq 1 "${REPEATS}"); do
        process_tag=$(printf 'p%05d' "${processes}")
        repeat_tag=$(printf 'repeat_%02d' "${repeat}")
        app_output="${OUTPUT_ROOT}/application_output/${EXPERIMENT}/${process_tag}/${repeat_tag}_${BATCH_ID}/"
        log_file="${PROFILE_ROOT}/${EXPERIMENT}/launcher_logs/${process_tag}_${repeat_tag}_${BATCH_ID}.log"
        mkdir -p "${app_output}"

        launcher_command=("${LAUNCHER}")
        case "${LAUNCHER_STYLE}" in
            yhrun|srun)
                if [[ -n "${PARTITION}" ]]; then
                    launcher_command+=( -p "${PARTITION}" )
                fi
                launcher_command+=( -N "${nodes}" -n "${processes}" )
                ;;
            mpirun|mpiexec)
                launcher_command+=( -np "${processes}" )
                ;;
            *)
                echo "Unsupported LAUNCHER_STYLE=${LAUNCHER_STYLE}" >&2
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
            -adj
            --profile
            --profile-dir "${PROFILE_ROOT}"
            --profile-experiment "${EXPERIMENT}"
            --profile-repeat "${repeat}"
        )
        if [[ "${CORE_ONLY}" == "1" ]]; then
            application_command+=( --profile-core-only )
        fi
        if [[ "${CACHE_COUNTERS}" == "1" ]]; then
            application_command+=( --profile-cache )
        fi
        application_command+=( "${app_extra[@]}" )

        echo "[RUN] processes=${processes} nodes=${nodes} repeat=${repeat}"
        "${launcher_command[@]}" "${application_command[@]}" 2>&1 | tee "${log_file}"
    done
done

python3 "${SCRIPT_DIR}/analyze_results.py" "${PROFILE_ROOT}/${EXPERIMENT}"
