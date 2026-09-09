#pragma once
#include "research_mesh.h"
// 生成并合并多个封闭子域，随后复用原体细化、编号和邻接交换。
double GenerateScheduledTasks(void *coarse,void *merged,int tasks,int levels,int maxbarycoord,
    std::map<int,xdMeshFaceInfo> &facemap,std::map<int,int> &g2l,
    std::map<Barycentric,int,CompBarycentric> &bary,std::list<xdFace> &faces);
