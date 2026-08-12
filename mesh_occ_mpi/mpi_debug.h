#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mpi.h"

inline bool netgen_mpi_trace_enabled()
{
    const char *value = std::getenv("NETGEN_MPI_TRACE");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

inline void netgen_mpi_checkpoint(
    MPI_Comm comm,
    const char *stage,
    long value1 = -1,
    long value2 = -1)
{
    if (!netgen_mpi_trace_enabled())
        return;

    int rank = -1;
    MPI_Comm_rank(comm, &rank);
    std::fprintf(
        stderr,
        "[MPI_CHECKPOINT] time=%.6f rank=%d stage=%s value1=%ld value2=%ld\n",
        MPI_Wtime(), rank, stage, value1, value2);
    std::fflush(stderr);
}

inline void netgen_mpi_check(MPI_Comm comm, int error_code, const char *operation)
{
    if (error_code == MPI_SUCCESS)
        return;

    int rank = -1;
    int error_length = 0;
    char error_string[MPI_MAX_ERROR_STRING] = {};
    MPI_Comm_rank(comm, &rank);
    MPI_Error_string(error_code, error_string, &error_length);
    std::fprintf(
        stderr,
        "[MPI_ERROR] rank=%d operation=%s code=%d message=%.*s\n",
        rank, operation, error_code, error_length, error_string);
    std::fflush(stderr);
    MPI_Abort(comm, error_code);
    std::abort();
}
