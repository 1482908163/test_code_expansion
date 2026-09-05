#!/usr/bin/env bash

# Shared build/run environment for the mt_module partition.
#
# This file is sourced by run_experiments.sh.  The
# defaults mirror build_project.sh and cjz_nodsp_copy.sh, while PROJECT_ROOT is
# resolved from the current checkout so that a renamed/moved clone still works.

SCALING_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCALING_SCRIPT_DIR}/.." && pwd)"

export PROJECT_ROOT="${PROJECT_ROOT:-${REPOSITORY_ROOT}}"
export PROJ_DIR="${PROJ_DIR:-${PROJECT_ROOT}}"
export GCCHOME="${GCCHOME:-/vol8/home/hnu_lhz/cjz/gcc-12}"
export LOCAL_LIB="${LOCAL_LIB:-/vol8/home/hnu_lhz/cjz/lib/usr/lib/aarch64-linux-gnu}"
export SYSTEM_AARCH64_LIB="${SYSTEM_AARCH64_LIB:-/usr/lib/aarch64-linux-gnu}"
export NETGEN_INSTALL_LIB="${NETGEN_INSTALL_LIB:-/vol8/home/hnu_lhz/cjz/NETGEN/install/lib}"
export MPI_X_LIB="${MPI_X_LIB:-/vol8/appsoftware/mpi-x/lib}"
export MPI_MODULE="${MPI_MODULE:-mpich/mpi-x}"
export EXPECTED_PMIX_LIB_PREFIX="${EXPECTED_PMIX_LIB_PREFIX:-/usr/lib}"

LOAD_MODULES="${LOAD_MODULES:-1}"
SANITIZE_MPI_ENV="${SANITIZE_MPI_ENV:-1}"
CLUSTER_ENV_STRICT="${CLUSTER_ENV_STRICT:-1}"

scaling_prepend_path()
{
    local variable="$1"
    local directory="$2"
    local current="${!variable-}"
    if [[ -n "${directory}" ]]; then
        export "${variable}=${directory}${current:+:${current}}"
    fi
}

scaling_load_modules()
{
    if ! type module >/dev/null 2>&1; then
        local init_script
        for init_script in \
            "${MODULE_INIT_SCRIPT:-}" \
            /etc/profile.d/modules.sh \
            /etc/profile.d/lmod.sh; do
            if [[ -n "${init_script}" && -r "${init_script}" ]]; then
                # shellcheck disable=SC1090
                source "${init_script}"
                break
            fi
        done
    fi

    if ! type module >/dev/null 2>&1; then
        echo "[ERROR] module 命令不可用；请在计算集群登录/作业环境中运行，或设置 LOAD_MODULES=0。" >&2
        return 1
    fi

    module purge
    module load "${MPI_MODULE}"
}

scaling_check_directory()
{
    local label="$1"
    local directory="$2"
    if [[ ! -d "${directory}" ]]; then
        echo "[ERROR] ${label} 目录不存在: ${directory}" >&2
        return 1
    fi
}

if [[ "${LOAD_MODULES}" == "1" ]]; then
    scaling_load_modules
fi

if [[ "${CLUSTER_ENV_STRICT}" == "1" ]]; then
    scaling_check_directory GCCHOME "${GCCHOME}"
    scaling_check_directory LOCAL_LIB "${LOCAL_LIB}"
    scaling_check_directory NETGEN_INSTALL_LIB "${NETGEN_INSTALL_LIB}"
fi

# Remove variables copied from unrelated OpenMPI/PMIx sessions.  The stable
# 256-process script does the same before starting MPICH through yhrun.
if [[ "${SANITIZE_MPI_ENV}" == "1" ]]; then
    unset LD_PRELOAD
    unset OMPI_MCA_pml
    unset OMPI_MCA_pml_ucx_tls
    unset PMIX_INSTALL_PREFIX
    unset PMIX_MCA_mca_base_component_path
fi

scaling_prepend_path PATH "${GCCHOME}/bin"

# Match the proven test_code_part03/cjz_nodsp_copy.sh runtime exactly.  Assign
# these paths instead of extending an inherited login-node environment: the
# latter previously selected /vol8/home/hnu_lhz/cjz/aarch64-linux-gnu/
# libpmix.so.2 together with MPI-X libmpi.so.12 and broke PMIx BFROPS loading.
export LIBRARY_PATH="${LOCAL_LIB}:${SYSTEM_AARCH64_LIB}:${MPI_X_LIB}"
export LD_LIBRARY_PATH="${GCCHOME}/lib64:${NETGEN_INSTALL_LIB}:${LOCAL_LIB}:${SYSTEM_AARCH64_LIB}:${MPI_X_LIB}"

# Force MPICH compiler wrappers to use the same GCC 12 toolchain as the
# runtime, avoiding libstdc++ ABI mismatches.
export MPICH_CC="${MPICH_CC:-${GCCHOME}/bin/gcc}"
export MPICH_CXX="${MPICH_CXX:-${GCCHOME}/bin/g++}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export NETGEN_MPI_TRACE="${NETGEN_MPI_TRACE:-0}"
