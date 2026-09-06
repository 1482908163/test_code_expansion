#include "mesh_mpi_types.h"
#include <array>
#include <iostream>

static void require(bool ok)
{
    if (!ok) { std::fprintf(stderr, "64-bit MPI numbering regression failed\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank); MPI_Comm_size(MPI_COMM_WORLD, &size);
    constexpr GlobalCount total = 10000000013LL;
    // Synthetic counts, not allocation of ten billion mesh entities.
    const auto offsets = gather_id_offsets(total / size + (rank < total % size), MPI_COMM_WORLD);
    require(offsets.back() == total);
    require(offsets[rank] == rank * (total / size) + std::min<GlobalCount>(rank, total % size));

    const std::array<GlobalId, 6> values{{2147483647LL, 2147483648LL, 4294967295LL,
                                       4294967296LL, 10000000000LL, -1}};
    std::array<Barycentric, 6> vertices{}, received_vertices{};
    std::array<xdVElement, 6> cells{}, received_cells{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        vertices[i].newgid = values[i];
        for (int k = 0; k < 3; ++k) { vertices[i].gvrtx[k] = rank + k + 1; vertices[i].coord[k] = 7 + k; }
        cells[i].gid = values[i]; cells[i].domidx = rank + 1;
        for (int k = 0; k < 4; ++k) {
            cells[i].Pindex[k] = values[(i + k) % values.size()];
            for (int d = 0; d < 3; ++d) cells[i].Vertexs[k].xyz[d] = rank + k + 0.25 * d;
        }
    }
    MPI_Datatype vertex_type = barycentric_mpi_type(MPI_COMM_WORLD);
    MPI_Datatype cell_type = volume_element_mpi_type(MPI_COMM_WORLD);
    MPI_Aint lb, extent;
    MPI_Type_get_extent(vertex_type, &lb, &extent); require(lb == 0 && extent == sizeof(Barycentric));
    MPI_Type_get_extent(cell_type, &lb, &extent); require(lb == 0 && extent == sizeof(xdVElement));
    int bytes;
    MPI_Type_size(vertex_type, &bytes); require(bytes == 3*sizeof(int)+3*sizeof(short)+sizeof(GlobalId));
    MPI_Type_size(cell_type, &bytes); require(bytes == 5*sizeof(GlobalId)+12*sizeof(double)+sizeof(int));
    const int next = (rank + 1) % size, prev = (rank + size - 1) % size;
    netgen_mpi_check(MPI_COMM_WORLD, MPI_Sendrecv(vertices.data(), 6, vertex_type, next, 21,
                     received_vertices.data(), 6, vertex_type, prev, 21, MPI_COMM_WORLD, MPI_STATUS_IGNORE), "test/vertices");
    netgen_mpi_check(MPI_COMM_WORLD, MPI_Sendrecv(cells.data(), 6, cell_type, next, 22,
                     received_cells.data(), 6, cell_type, prev, 22, MPI_COMM_WORLD, MPI_STATUS_IGNORE), "test/cells");
    for (std::size_t i = 0; i < values.size(); ++i) {
        require(received_vertices[i].newgid == values[i] && received_cells[i].gid == values[i]);
        for (int k = 0; k < 3; ++k)
            require(received_vertices[i].gvrtx[k] == prev+k+1 && received_vertices[i].coord[k] == 7+k);
        require(received_cells[i].domidx == prev + 1);
        for (int k = 0; k < 4; ++k) {
            require(received_cells[i].Pindex[k] == values[(i+k)%values.size()]);
            for (int d = 0; d < 3; ++d) require(received_cells[i].Vertexs[k].xyz[d] == prev+k+0.25*d);
        }
    }
    GlobalCount local_bins[6], global_bins[6]{};
    std::fill_n(local_bins, 6, total / size + (rank < total % size));
    MPI_Allreduce(local_bins, global_bins, 6, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
    for (auto bin : global_bins) require(bin == total);
    MPI_Type_free(&vertex_type); MPI_Type_free(&cell_type);
    if (rank == 0) std::cout << "PASS: 64-bit MPI prefixes, multi-record vertex/cell transfer and count reduction on " << size << " rank(s)\n";
    MPI_Finalize();
}
