#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "${STRONG_SCALING_DIR:-$(dirname "${BASH_SOURCE[0]}")}" && pwd)"
SUITE_ROOT="${SUITE_ROOT:?SUITE_ROOT 未设置}"
SUITE_MODES="${SUITE_MODES:-core_timing core_cache full_io}"
SUITE_MODES="${SUITE_MODES//,/ }"

mkdir -p "${SUITE_ROOT}/analysis"
analysis_status_file="${SUITE_ROOT}/analysis/mode_analysis_status.tsv"
printf 'mode\tstate\texit_code\n' > "${analysis_status_file}"
overall_status=0
read -r -a suite_modes_array <<< "${SUITE_MODES}"
for mode in "${suite_modes_array[@]}"; do
    mode_root="${SUITE_ROOT}/modes/${mode}"
    if find "${mode_root}" -name run_summary.csv -type f -print -quit 2>/dev/null | grep -q .; then
        echo "[ANALYZE] ${mode_root}"
        set +e
        python3 "${SCRIPT_DIR}/analyze_results.py" "${mode_root}"
        mode_status=$?
        set -e
        if (( mode_status == 0 )); then
            printf '%s\tANALYZED\t0\n' "${mode}" >> "${analysis_status_file}"
        else
            printf '%s\tANALYSIS_FAILED\t%s\n' "${mode}" "${mode_status}" >> "${analysis_status_file}"
            echo "[ERROR] ${mode} 汇总失败；继续处理后续模式。" >&2
            overall_status="${mode_status}"
        fi
    else
        echo "[WARN] ${mode} 没有有效 run_summary.csv，统一报告中将标记为缺失。" >&2
        printf '%s\tMISSING_DATA\t1\n' "${mode}" >> "${analysis_status_file}"
        overall_status=1
    fi
done

set +e
python3 "${SCRIPT_DIR}/analyze_suite.py" "${SUITE_ROOT}" --modes "${suite_modes_array[@]}"
suite_status=$?
set -e
if (( suite_status == 0 )); then
    echo "[OK] 统一报告: ${SUITE_ROOT}/analysis/suite_report.txt"
else
    echo "[ERROR] 统一报告生成失败，exit=${suite_status}" >&2
    overall_status="${suite_status}"
fi

exit "${overall_status}"
