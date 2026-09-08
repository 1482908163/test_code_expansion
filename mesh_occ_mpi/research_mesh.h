#pragma once
#include "3DNgmesher.h"
#include "research_options.h"
idx_t *PartitionResearchMesh(void *mesh, int parts);
void RunPartitionPreflight(void *mesh);
void SparseGatherFaceMap(std::map<int,xdMeshFaceInfo> &facemap,
                        fid_xdMeshFaceInfo *records,int count,int first_element);
void VerifyFaceClosure(const std::map<int,xdMeshFaceInfo> &sparse,
                      const std::map<int,xdMeshFaceInfo> &global,int rank);
