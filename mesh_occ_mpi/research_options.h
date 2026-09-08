#pragma once
#include <string>
#include "partition_cost.h"
#include "partition_sampling.h"
#include "resource_mapping.h"
namespace mesh_research {
struct ResearchOptions {
    std::string algorithm = "baseline";
    std::string model_path;
    CostConfig cost;
    std::string resource_path, capacity_path;
    ResourceModel resource;
    int partition_seed = -1; // -1 retains the library's original default.
    std::string partition_variant = "metis_seed";
    std::string reference_path, preflight_dir;
    std::vector<int> reference_labels, preflight_seeds;
    int rank_shift = 0, preflight_parts = 0;
    bool verify_faces = false;
    bool sparse() const { return algorithm == "sparse" || algorithm == "combined"; }
    bool balance() const { return algorithm == "balance" || algorithm == "combined"; }
};
ResearchOptions &options();
}
