#pragma once
#include "parallelMeshData.h"
#include "metis.h"
#include <filesystem>


bool NewSubmesh(void* mesh, void* submesh);

idx_t* PartitionMesh(void *mesh, int parts);
int FindSurfElem(void *mesh, int *elem, surfMap_t &surfMap);
int ExtractFaceOutward(int *tverts, int *fverts);
void GetSurfPoints(void *mesh, surfMap_t &surfMap);
void ExtractSurfaceMesh(void *mesh, int fid, int procid, int *tverts, fid_xdMeshFaceInfo *sub_face_map, int sub_face_map_index, int domainidx);
void ExtractPartitionSurfaceMesh(void *mesh, idx_t *edest, std::map< int, xdMeshFaceInfo > &facemap, double *stage_time = nullptr);
void Allgather_Face_Map(std::map<int, xdMeshFaceInfo> &facemap, fid_xdMeshFaceInfo *sub_face_map, int ne, int sub_ne, int *every_sub_ne, int *every_offset, double *stage_time = nullptr);
void PartFaceCreate(void *mesh, int belongNumberPartition, std::map< int, xdMeshFaceInfo > &facemap, int maxbarycoord, void *submesh, std::map< int, int > &g2lvrtxmap, std::map< Barycentric, int, CompBarycentric > &baryc2locvrtxmap, std::list<xdFace> &newfaces);
void Refine(void *submesh, int numlevels, int belongNumberPartition, std::list<xdFace> &newfaces, std::map<Barycentric, int, CompBarycentric> &baryc2locvrtxmap, std::map<IntPair, int, IntPairCompare> &edgemap);
void BarycMidPoint(Barycentric p1, Barycentric p2, Barycentric &res);
Barycentric InitBarycv(int v, int maxbarycoord);
void Refineforvol(void *submesh, int belongNumberPartition, std::list<xdFace> &newfaces, std::map< Barycentric, int, CompBarycentric > &baryc2locvrtxmap, std::map < IntPair, int, IntPairCompare> &edgemap);
void computeadj(int mypid, std::map< int, xdMeshFaceInfo > &facemap, std::map<int, int > &g2lvrtxmap, std::map< Barycvrtx, std::list<int>, CompBarycvrtx > &barycvrtx2adjprocsmap);
GlobalId* com_barycoords(void *submesh,MPI_Comm comm,std::map< Barycvrtx, std::list<int>, CompBarycvrtx > &barycvrtx2adjprocsmap,std::map< Barycentric, int, CompBarycentric > &baryc2locvrtxmap,std::map< int, std::list<int>> &adjbarycs,	int numprocs, GlobalId *newgVEid,int mypid);
GlobalId* com_baryVolumeElements(void *submesh, MPI_Comm comm, std::map< Barycvrtx, std::list<int>, CompBarycvrtx > &barycvrtx2adjprocsmap, std::map< Barycentric, int, CompBarycentric > &baryc2locvrtxmap, std::map< int, std::list<int>> &adjbarycs, GlobalId *oldgid, GlobalId *VEgid, std::list<VEindex> &VEindexs, int numprocs, int mypid);
// GlobalId* com_baryVolumeElements(void *submesh, MPI_Comm comm, std::map< Barycvrtx, std::list<int>, CompBarycvrtx > &barycvrtx2adjprocsmap, std::map< Barycentric, int, CompBarycentric > &baryc2locvrtxmap, std::map< int, std::list<int>> &adjbarycs, GlobalId *oldgid, GlobalId *VEgid,  int numprocs, int mypid);
void Record_LWR_count(double LWR, GlobalCount * count);
bool meshQualityEvaluation(void *mesh, int id, std::string OUTPUT_PATH);
double triangle_jacobian_ratio(const double points[][3]);
double tetrahedrons_jacobian_ratio(const double points[][3]);
double getLenght(const double a1[], const double a2[]);
double length_width_ratio(const double points[][3]);
double tetrahedrons_length_width_ratio(const double points[][3]);
double min_internal_angle(const double points[][3]);
double max_internal_angle(const double points[][3]);
double func(const double a1[], const double a2[], const double a3[]);
double triangle_skew(const double points[][3]);
double tetrahedrons_skew(double points[][3]);
