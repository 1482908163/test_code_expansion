#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Compatibility wrapper.  submit_experiments.sh is the stable primary entry.
export SUITE_MODE=1
export EXPERIMENT="${EXPERIMENT:-strong_scaling_suite}"
export PROCESS_COUNTS="${PROCESS_COUNTS:-1 2 4 8 16 32 64 128 256}"
export REPEATS="${REPEATS:-6}"
export RANKS_PER_NODE="${RANKS_PER_NODE:-1}"
export SUITE_MODES="${SUITE_MODES:-core_timing core_cache full_io}"
export SERIALIZE_JOBS="${SERIALIZE_JOBS:-1}"
export PAGE_CACHE_POLICY="${PAGE_CACHE_POLICY:-evict-first}"

exec bash "${SCRIPT_DIR}/submit_experiments.sh"
