#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${TEST_BUILD_DIR:-${ROOT}/build/research_tests}"
CXX="${CXX:-g++}"
MPICXX="${MPICXX:-mpicxx}"
MPI_LAUNCHER="${MPI_LAUNCHER:-mpiexec}"
TEST_PROCESS_COUNTS="${TEST_PROCESS_COUNTS:-1 2 3 4 8}"
mkdir -p "${BUILD}"
for name in partition_cost face_dependency mesh_ids; do
    "${CXX}" -std=c++17 -Wall -Wextra -Werror -I "${ROOT}/mesh_occ_mpi" \
        "${ROOT}/tests/test_${name}.cpp" -o "${BUILD}/test_${name}"
    "${BUILD}/test_${name}"
done
"${MPICXX}" -std=c++17 -Wall -Wextra -Werror -I "${ROOT}/mesh_occ_mpi" \
    "${ROOT}/tests/test_sparse_faces.cpp" "${ROOT}/mesh_occ_mpi/scaling_profiler.cpp" \
    -o "${BUILD}/test_sparse_faces"
"${MPICXX}" -std=c++17 -Wall -Wextra -Werror -I "${ROOT}/mesh_occ_mpi" \
    "${ROOT}/tests/test_mesh_ids_mpi.cpp" -o "${BUILD}/test_mesh_ids_mpi"
read -r -a extra <<< "${MPI_EXTRA_ARGS:-}"
for p in ${TEST_PROCESS_COUNTS}; do
    timeout "${TEST_TIMEOUT_SECONDS:-60}" "${MPI_LAUNCHER}" "${extra[@]}" -n "${p}" "${BUILD}/test_mesh_ids_mpi"
    for mode in natural split; do
        args=("${BUILD}/test_sparse_faces" "${BUILD}/p${p}_${mode}")
        [[ "${mode}" == split ]] && args+=(split)
        timeout "${TEST_TIMEOUT_SECONDS:-60}" "${MPI_LAUNCHER}" "${extra[@]}" -n "${p}" "${args[@]}"
    done
done
python3 "${ROOT}/tests/test_runner.py"
