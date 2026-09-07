#pragma once
#include <string>
#include "partition_cost.h"
namespace mesh_research {
struct ResearchOptions {
    std::string algorithm = "baseline";
    std::string model_path;
    CostConfig cost;
    int partition_seed = -1; // -1 retains the library's original default.
    bool verify_faces = false;
    bool sparse() const { return algorithm == "sparse" || algorithm == "combined"; }
    bool balance() const { return algorithm == "balance" || algorithm == "combined"; }
};
ResearchOptions &options();
}
