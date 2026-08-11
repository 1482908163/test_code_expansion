#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "${STRONG_SCALING_DIR:-$(dirname "${BASH_SOURCE[0]}")}" && pwd)"
SUITE_ROOT="${SUITE_ROOT:?SUITE_ROOT 未设置}"
SUITE_MODES="${SUITE_MODES:-core_cache full_io}"

analyzed_modes=()
for mode in ${SUITE_MODES}; do
    mode_root="${SUITE_ROOT}/modes/${mode}"
    if find "${mode_root}" -name run_summary.csv -type f -print -quit 2>/dev/null | grep -q .; then
        echo "[ANALYZE] ${mode_root}"
        python3 "${SCRIPT_DIR}/analyze_results.py" "${mode_root}"
        analyzed_modes+=("${mode}")
    else
        echo "[WARN] ${mode} 没有有效 run_summary.csv，统一报告中将标记为缺失。" >&2
    fi
done

if (( ${#analyzed_modes[@]} == 0 )); then
    echo "[ERROR] 没有可汇总的模式结果。" >&2
    exit 1
fi

python3 "${SCRIPT_DIR}/analyze_suite.py" "${SUITE_ROOT}" --modes "${analyzed_modes[@]}"
echo "[OK] 统一报告: ${SUITE_ROOT}/analysis/suite_report.txt"
