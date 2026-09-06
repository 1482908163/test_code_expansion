#include "mesh_ids.h"
#include "createElmerOutput.h"
#include <cassert>
#include <iostream>
#include <limits>

int main()
{
    constexpr GlobalCount total = 10000000000LL;
    for (int p : {1, 2, 3, 16, 1024, 8192}) {
        std::vector<GlobalCount> counts(p, total / p);
        for (int r = 0; r < total % p; ++r) ++counts[r];
        const auto offsets = make_id_offsets(counts);
        assert(offsets.front() == 0 && offsets.back() == total);
        for (int r = 0; r < p; ++r) {
            assert(offsets[r + 1] - offsets[r] == counts[r]);
            if (r + 1 < p) assert(offsets[r] + counts[r] < offsets[r + 1] + 1);
        }
    }
    assert((make_id_offsets({0, 2147483647LL, 1, 0, 2147483648LL}) ==
            std::vector<GlobalId>{0, 0, 2147483647LL, 2147483648LL, 2147483648LL, 4294967296LL}));
    assert(make_id_offsets({std::numeric_limits<GlobalId>::max()}).back() ==
           std::numeric_limits<GlobalId>::max());
    for (const auto &bad : std::vector<std::vector<GlobalCount>>{{-1}, {std::numeric_limits<GlobalId>::max(), 1}}) {
        bool failed = false;
        try { make_id_offsets(bad); } catch (const std::overflow_error &) { failed = true; }
        assert(failed);
    }
    assert(fits_local_mesh(2000000) && !fits_local_mesh(total));

    // These keys collide if a 64-bit global point is narrowed to 32 bits.
    Index3 low(1, 3, 2), high(4294967297LL, 3, 2);
    low.Sort(); high.Sort();
    std::map<Index3, GlobalId, decltype(&fncomp)> parents(&fncomp);
    parents[low] = 2147483648LL; parents[high] = total;
    assert(parents.size() == 2 && parents.at(high) == total);
    assert(high.x[2] == 4294967297LL);

    const GlobalId points[] = {0, 2147483647LL, 2147483648LL, 4294967296LL, total};
    const int tet[] = {1, 2, 3, 4}, face[] = {2, 3, 4};
    const double xyz[] = {1.0, 2.0, 3.0};
    std::ostringstream elements, nodes, shared, boundary;
    write_partition_element(elements, total + 1, tet, points);
    write_partition_node(nodes, total, xyz);
    write_partition_shared(shared, total, "2 1 2");
    write_partition_boundary(boundary, 7, 2, total + 1, face, points);
    assert(elements.str() == "10000000001 1 504 2147483647 2147483648 4294967296 10000000000\n");
    assert(nodes.str() == "10000000000 -1 1 2 3\n");
    assert(shared.str() == "10000000000 2 1 2\n");
    assert(boundary.str() == "7 2 10000000001 0 303 2147483648 4294967296 10000000000\n");
    std::istringstream readback(elements.str());
    GlobalId id; int domain, type;
    readback >> id >> domain >> type;
    assert(id == total + 1 && domain == 1 && type == 504);
    for (int i = 1; i <= 4; ++i) { readback >> id; assert(id == points[i]); }
    std::cout << "PASS: synthetic 10-billion numbering, empty ranks, 64-bit overflow, boundary keys and production text writers\n";
}
