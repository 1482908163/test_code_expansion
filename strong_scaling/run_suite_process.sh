#!/usr/bin/env bash
set -Euo pipefail

SCRIPT_DIR="$(cd "${STRONG_SCALING_DIR:-$(dirname "${BASH_SOURCE[0]}")}" && pwd)"
SUITE_ROOT="${SUITE_ROOT:?SUITE_ROOT 未设置}"
SUITE_NAME="${SUITE_NAME:?SUITE_NAME 未设置}"
SUITE_MODES="${SUITE_MODES:-core_timing core_cache full_io}"
SUITE_MODES="${SUITE_MODES//,/ }"
PROCESS_COUNTS="${PROCESS_COUNTS:?PROCESS_COUNTS 未设置}"

mkdir -p "${SUITE_ROOT}/mode_status"
overall_status=0
read -r -a suite_modes_array <<< "${SUITE_MODES}"

for mode in "${suite_modes_array[@]}"; do
    case "${mode}" in
        core_cache)
            mode_core_only=1
            mode_cache_counters=1
            mode_comm_graph=0
            ;;
        core_timing)
            mode_core_only=1
            mode_cache_counters=0
            mode_comm_graph=0
            ;;
        full_io)
            mode_core_only=0
            mode_cache_counters=0
            mode_comm_graph=0
            ;;
        comm_graph)
            mode_core_only=1
            mode_cache_counters=0
            mode_comm_graph=1
            ;;
        *)
            echo "[ERROR] 不支持的统一实验模式: ${mode}" >&2
            overall_status=2
            continue
            ;;
    esac

    echo "============================================================"
    echo "Suite (统一实验): ${SUITE_NAME}"
    echo "Mode (模式): ${mode}"
    echo "Processes (进程数): ${PROCESS_COUNTS}"
    echo "============================================================"

    set +e
    OUTPUT_ROOT="${SUITE_ROOT}/modes" \
    EXPERIMENT="${mode}" \
    RUN_NAME="${mode}" \
    CORE_ONLY="${mode_core_only}" \
    CACHE_COUNTERS="${mode_cache_counters}" \
    COMM_GRAPH="${mode_comm_graph}" \
    ANALYZE_AFTER_RUN=0 \
    ANALYSIS_ONLY=0 \
    bash "${SCRIPT_DIR}/run_experiments.sh"
    mode_status=$?
    set -e

    process_tag=$(printf 'p%05d' "${PROCESS_COUNTS}")
    status_file="${SUITE_ROOT}/mode_status/${process_tag}_${mode}.tsv"
    printf 'processes\tmode\tstate\texit_code\n' > "${status_file}"
    if (( mode_status == 0 )); then
        printf '%s\t%s\tCOMPLETED\t0\n' "${PROCESS_COUNTS}" "${mode}" >> "${status_file}"
    else
        printf '%s\t%s\tFAILED\t%s\n' "${PROCESS_COUNTS}" "${mode}" "${mode_status}" >> "${status_file}"
        echo "[ERROR] mode=${mode}, processes=${PROCESS_COUNTS}, exit=${mode_status}" >&2
        overall_status="${mode_status}"
    fi
done

exit "${overall_status}"
