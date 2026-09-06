#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Final distributed mesh identifiers/counts. Netgen-local and coarse-mesh
// indices remain int and must never receive a final GlobalId by narrowing.
using GlobalId = std::int64_t;
using GlobalCount = std::int64_t;
static_assert(sizeof(GlobalId) == 8, "64-bit global mesh IDs required");

// Exclusive offsets; rank r owns (offset[r], offset[r+1]]. Empty ranks
// are valid. Checking before addition avoids signed overflow at INT64_MAX.
inline std::vector<GlobalId> make_id_offsets(const std::vector<GlobalCount> &counts)
{
    std::vector<GlobalId> offsets(counts.size() + 1, 0);
    for (std::size_t r = 0; r < counts.size(); ++r) {
        if (counts[r] < 0 || counts[r] > std::numeric_limits<GlobalId>::max() - offsets[r])
            throw std::overflow_error("global mesh count exceeds signed 64-bit capacity");
        offsets[r + 1] = offsets[r] + counts[r];
    }
    return offsets;
}

// Keep room for one-based indexing and the increment after the last element.
inline bool fits_local_mesh(GlobalCount count)
{
    return count >= 0 && count < std::numeric_limits<int>::max();
}
