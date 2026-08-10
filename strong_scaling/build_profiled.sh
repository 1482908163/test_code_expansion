#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LOAD_CLUSTER_ENV="${LOAD_CLUSTER_ENV:-1}"
if [[ "${LOAD_CLUSTER_ENV}" == "1" ]]; then
    # shellcheck disable=SC1091
    source "${SCRIPT_DIR}/cluster_env.sh"
fi

BUILD_DIR="${BUILD_DIR:-${REPOSITORY_ROOT}/build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-${REPOSITORY_ROOT}/install-test}"
BUILD_JOBS="${BUILD_JOBS:-${SLURM_CPUS_ON_NODE:-$(nproc)}}"
INSTALL_AFTER_BUILD="${INSTALL_AFTER_BUILD:-1}"
BUILD_LOG_ROOT="${BUILD_LOG_ROOT:-${REPOSITORY_ROOT}/strong_scaling_results/build_logs}"
BUILD_ID="${BUILD_ID:-$(date +%Y%m%d-%H%M%S)}"
BUILD_LOG="${BUILD_LOG_ROOT}/build_${BUILD_ID}.log"

mkdir -p "${BUILD_DIR}" "${BUILD_LOG_ROOT}"

for command_name in cmake mpicc mpicxx; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "[ERROR] 找不到 ${command_name}，请检查模块和 cluster_env.sh 配置。" >&2
        exit 2
    fi
done

configure_command=(
    cmake
    -S "${REPOSITORY_ROOT}"
    -B "${BUILD_DIR}"
    "-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}"
    "-DCMAKE_C_COMPILER=$(command -v mpicc)"
    "-DCMAKE_CXX_COMPILER=$(command -v mpicxx)"
)
build_command=(cmake --build "${BUILD_DIR}" -- -j"${BUILD_JOBS}")

{
    echo "Strong-scaling profiled build (强扩展插桩版编译)"
    echo "================================================"
    echo "time=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "repository_root=${REPOSITORY_ROOT}"
    echo "build_dir=${BUILD_DIR}"
    echo "install_prefix=${INSTALL_PREFIX}"
    echo "build_jobs=${BUILD_JOBS}"
    echo "mpicc=$(command -v mpicc)"
    echo "mpicxx=$(command -v mpicxx)"
    echo "MPICH_CC=${MPICH_CC-}"
    echo "MPICH_CXX=${MPICH_CXX-}"
    echo
    printf '[CONFIGURE]'
    printf ' %q' "${configure_command[@]}"
    printf '\n'
    "${configure_command[@]}"
    printf '[BUILD]'
    printf ' %q' "${build_command[@]}"
    printf '\n'
    "${build_command[@]}"
    if [[ "${INSTALL_AFTER_BUILD}" == "1" ]]; then
        echo "[INSTALL] cmake --build ${BUILD_DIR} --target install"
        cmake --build "${BUILD_DIR}" --target install -- -j"${BUILD_JOBS}"
    fi
    echo
    echo "[OK] executable=${BUILD_DIR}/mesh_occ_mpi/mesh_occ_mpi"
} 2>&1 | tee "${BUILD_LOG}"

echo "[OK] 编译日志: ${BUILD_LOG}"
