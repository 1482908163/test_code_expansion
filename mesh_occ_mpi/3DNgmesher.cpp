#include <map>
#include <algorithm>
#include <math.h>
#include <iostream>
#include "3DNgmesher.h"
#include <string>
#include <time.h>

// #define M_PI 3.1415926535897932

namespace nglib
{
#include <nglib.h>
}
//数组排序，整个数组元素将按升序排序
void SortInt(int *v)
{
	if (v[0] > v[1])
	{
		v[0] ^= v[1];
		v[1] ^= v[0];
		v[0] ^= v[1];
	}
	if (v[1] > v[2])
	{
		v[1] ^= v[2];
		v[2] ^= v[1];
		v[1] ^= v[2];
	}
	if (v[0] > v[1])
	{
		v[0] ^= v[1];
		v[1] ^= v[0];
		v[0] ^= v[1];
	}
}
void sort3int(
	int *v)
{
	if (v[0] > v[1])
	{
		v[0] ^= v[1];
		v[1] ^= v[0];
		v[0] ^= v[1];
	}
	if (v[1] > v[2])
	{
		v[1] ^= v[2];
		v[2] ^= v[1];
		v[1] ^= v[2];
	}
	if (v[0] > v[1])
	{
		v[0] ^= v[1];
		v[1] ^= v[0];
		v[0] ^= v[1];
	}
}
void sort2int(
	int *v)
{
	if (v[1] > v[0])
	{
		v[0] ^= v[1];
		v[1] ^= v[0];
		v[0] ^= v[1];
	}
}


//根据给定的主网格mesh，创建一个子网格submesh
bool NewSubmesh(void *mesh, void *submesh)
{
	//获取主网格mesh的面数
	int faceNum = nglib::Ng_GetNFD((nglib::Ng_Mesh *)mesh);
	for (int i = 2; i < faceNum + 1; i++)
	{
		int x[4];
		//获取面的描述符，并将其存储在数组x中
		nglib::My_Ng_GetFaceDescriptor((nglib::Ng_Mesh *)mesh, i, x);
		//将数组x中的描述符添加到子网格submesh中
		nglib::My_Ng_AddFaceDescriptor((nglib::Ng_Mesh *)submesh, x[0], x[1], x[2], x[3]);
	}
	return true;
}

//使用了METIS库进行网格划分的函数
idx_t *PartitionMetis(void *mesh, int numParts, idx_t *eptr, idx_t *eind, idx_t *edest, idx_t *ndest)
{
	idx_t ne; // 网格中的元素数目
	idx_t nn; // 网格中的节点数目
	idx_t ncommon = 3;	//两个相邻元素之间的共享节点数目
	idx_t objval;	//划分结果的优化目标值
	int vrts[4];	//存储元素的节点索引数组
	int rc; 		// METIS库的返回代码
	int i;			//
	int elemno;		//元素编号
	int domainidx;	//元素所属的域索引
	idx_t options[METIS_NOPTIONS];//METIS库的选项数组

	METIS_SetDefaultOptions(options);	//设置默认的 METIS 选项
	options[METIS_OPTION_CONTIG] = 0;
	nglib::Ng_Mesh *ngMesh = (nglib::Ng_Mesh *)mesh;
	nn = nglib::Ng_GetNP(ngMesh);
	ne = nglib::Ng_GetNE(ngMesh);
	// printf("MY: (nn,ne) (%d 9%d)\n", nn, ne);
	MYMALLOC(eptr, idx_t *, ((ne + 1) * sizeof(idx_t))); // memory allocation
	MYMALLOC(edest, idx_t *, (ne * sizeof(idx_t)));
	MYMALLOC(eind, idx_t *, (4 * ne * sizeof(idx_t)));
	MYMALLOC(ndest, idx_t *, (nn * sizeof(idx_t)));
	eptr[0] = 0;
	i = 0;

	//获取元素的节点索引，并将其存储在 eind 数组中。
	for (elemno = 1; elemno <= ne; elemno++)
	{
		nglib::Ng_GetVolumeElement(ngMesh, elemno, vrts, domainidx);
		eind[i++] = vrts[0] - 1;
		eind[i++] = vrts[1] - 1;
		eind[i++] = vrts[2] - 1;
		eind[i++] = vrts[3] - 1;
		eptr[elemno] = eptr[elemno - 1] + 4;
	}
	// partition function for the metis
	//  printf("NE = %d\n", ne);
	//  printf("NN = %d\n", nn);
	//  printf("numParts = %d\n", numParts);

	//调用 METIS_PartMeshDual 函数进行网格划分
	rc = METIS_PartMeshDual(&ne, &nn, eptr, eind, NULL, NULL, &ncommon, &numParts, NULL, options,
							&objval, edest, ndest);
	// printf("MY: metis objval = 9%d\n",objval);
	// printf("Partipartition succeeded!");
	//返回 edest 数组，其中包含了元素的目标划分信息
	return edest;
}

//对给定的网格 mesh 进行划分，并返回划分后的结果。
idx_t *PartitionMesh(void *mesh, int parts)
{
	// 用于存储划分过程中的中间结果
	idx_t *eptr = NULL;
	idx_t *eind = NULL;
	idx_t *edest = NULL;
	idx_t *ndest = NULL;

	//该函数会使用 METIS 库对网格进行划分，并返回划分结果到 edest 数组中
	edest = PartitionMetis((nglib::Ng_Mesh *)mesh, parts, eptr, eind, edest, ndest);
	// Extract surface mesh and refine
	// ExtractPartit ionSurfaceMesh ((nglib::Ng_ Mesh*)mesh, numlevels, edest, maxbarycoord, surfMap) ;

	//函数返回了 edest 数组，即划分后的结果
	return edest;
}

//从划分后的网格中提取表面网格，并将结果存储在 facemap 中
void ExtractPartitionSurfaceMesh(void *mesh, idx_t *edest, std::map<int, xdMeshFaceInfo> &facemap, double *stage_time)
{
	if (stage_time) {
		for (int i = 0; i < 6; i++) stage_time[i] = 0.0;
	}
	double func_start = MPI_Wtime();


	//传入的 mesh 转换为 nglib::Ng_Mesh 类型的指针 newMesh
	nglib::Ng_Mesh *newMesh = (nglib::Ng_Mesh *)mesh;
	int fids[4]; // four indexes of the tetrahedron face
	int vrts[4];
	// index of the four vertices of the tetrahedra
	int orient[4];
	int p, i, k, elemno;
	int domainidx;
	bool netgenmeshupdate = true;
	
	//获取当前进程的 MPI 通信相关信息，包括进程编号 id 和进程总数 size。
	int id, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &id);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	//获取网格中的表面元素数量和体元素数量
	double setup_start = MPI_Wtime();
	int nse = nglib::Ng_GetNSE(newMesh);
	int ne = nglib::Ng_GetNE(newMesh);
	surfMap_t surfMap;
	GetSurfPoints(newMesh, surfMap);
	time_t start_time,time1,time2,time3,time4;
	if(id == 0) {
		start_time = time(NULL);
	}
	//每个进程计算了划分后负责处理的元素数量。
	int every_sub_ne[size];
	int every_offset[size];
	int sub_ne = ne / size;
	for(int i = 0; i < size; i++) {
		every_sub_ne[i] = sub_ne*4;
		every_offset[i] = sub_ne*4 * i;
	}
	every_sub_ne[size-1] = (ne - (sub_ne * (size-1)))*4;
	int start_ne = sub_ne * id + 1;
	int end_ne = start_ne + sub_ne-1;
	if(id == size - 1) {
		end_ne = ne;
	}

	// std::vector<fid_xdMeshFaceInfo> sub_face_map;
	sub_ne = end_ne-start_ne+1;
	fid_xdMeshFaceInfo sub_face_map[sub_ne*4];
	int sub_face_map_index = 0;

	double setup_end = MPI_Wtime();
	if (stage_time) stage_time[0] = setup_end - setup_start;

	//函数根据每个进程负责的元素范围，遍历划分后的网格的元素
	double local_extract_start = MPI_Wtime();
	for (elemno = start_ne; elemno <= end_ne; elemno++)
	{
		p = edest[elemno - 1];
		// the edest storage area decomposes after each body element belongs to the partition
		// if (netgenmeshupdate) printf("MY: starting update\n")

		//获取其面的索引
		nglib::My_Ng_GetElement_Faces(newMesh, elemno, fids, orient, netgenmeshupdate);
		// get the four surface indexes for each body elemetn

		//获取顶点的索引
		netgenmeshupdate = false;
		nglib::Ng_GetVolumeElement(newMesh, elemno, vrts, domainidx);


		// get the four vertex indexes for each body element
		for (k = 0; k < 4; k++)
		{
			//调用ExtractSurfaceMesh函数提取表面网格，将面信息存储在sub_face_map中
			ExtractSurfaceMesh(newMesh, fids[k], p, vrts, sub_face_map, sub_face_map_index, domainidx);
			sub_face_map_index++;
		}

	}
	double local_extract_end = MPI_Wtime();
	if (stage_time) stage_time[1] = local_extract_end - local_extract_start;

	//使用Allgather_Face_Map函数收集所有进程的结果，得到最终的表面网格
	double allgather_stage_time[3] = {0.0, 0.0, 0.0};
	Allgather_Face_Map(facemap,sub_face_map,ne,sub_ne,every_sub_ne,every_offset, allgather_stage_time);
	if (stage_time) {
		stage_time[2] = allgather_stage_time[0];
		stage_time[3] = allgather_stage_time[1];
		stage_time[4] = allgather_stage_time[2];
	}
	//函数在进程编号为 0 的进程输出总运行时间
	double func_end = MPI_Wtime();
	if (stage_time) stage_time[5] = func_end - func_start;

	if(id == 0) {
		time_t end_time = time(NULL);
		std::cout << "id0 sum runtime is :" << end_time - start_time << "s" << std::endl;
	}
}

void GetSurfPoints(void *mesh, surfMap_t &surfMap)
{
	nglib::Ng_Mesh *newMesh = (nglib::Ng_Mesh *)mesh;
	int surfidx;
	//获取网格中的表面元素的数量
	int nse = nglib::Ng_GetNSE(newMesh);
	//存储每个表面元素的节点索引
	int *surfpoints = new int[3];
	for (int i = 0; i < nse; i++)
	{
		//获取每个表面元素的节点索引和表面索引
		nglib::Ng_GetSurfaceElement(newMesh, i + 1, surfpoints, surfidx);
		FacePoints fp;
		//将节点索引排序后，将排序后的节点索引作为键，表面索引作为值，存储在 surfMap 中
		std::sort(surfpoints, surfpoints + 3);
		fp.p[0] = surfpoints[0];
		fp.p[1] = surfpoints[1];
		fp.p[2] = surfpoints[2];
		// surfMap[fp] = nglib::GetBoundaryID(newMesh, i + 1);
		surfMap[fp] = surfidx;
	}
}

//提取网格中的表面网格信息，并将结果存储在 sub_face_map 中。
void ExtractSurfaceMesh(void *mesh, int fid, int procid, int *tverts, fid_xdMeshFaceInfo *sub_face_map, int sub_face_map_index, int domainidx)
{
	nglib::Ng_Mesh *newMesh = (nglib::Ng_Mesh *)mesh;
	xdMeshFaceInfo finfo;
	std::map<int, xdMeshFaceInfo>::iterator it;
	int fverts[3];
	int outw;
	//通过传入的表面元素的索引 fid，获取该表面元素的顶点索引 fverts
	nglib::My_Ng_GetFace_Vertices(newMesh, fid, fverts); // get an index of the surface points according to the id of the surface
	// it = facemap.find(fid);

	//根据传入的顶点索引 tverts 和表面顶点索引 fverts，计算表面的外向法向量
	outw = ExtractFaceOutward(tverts, fverts);

	//将表面的外向法向量、进程编号、顶点索引和域索引存储在 finfo 中
	finfo.outw = outw;
	finfo.procids[0] = procid;
	finfo.procids[1] = -1;
	finfo.svrtx[0][0] = fverts[0];
	if (!outw)
	{
		finfo.svrtx[0][1] = fverts[1];
		finfo.svrtx[0][2] = fverts[2];
	}
	else
	{
		finfo.svrtx[0][1] = fverts[2];
		finfo.svrtx[0][2] = fverts[1];
	}
	finfo.domainidx[0] = domainidx;

	// 将表面元素的索引 fid 和上述的 finfo 存储在 fmfi 中
	fid_xdMeshFaceInfo fmfi;
	fmfi.fid = fid;
	fmfi.mfi = finfo;

	//函数将 fmfi 存储在 sub_face_map 的相应位置，以便后续处理和使用
	sub_face_map[sub_face_map_index] = fmfi;

	// if (it == facemap.end())
	// {
	// 	finfo.outw = outw;
	// 	finfo.procids[0] = procid;
	// 	finfo.procids[1] = -1;
	// 	finfo.svrtx[0][0] = fverts[0];
	// 	if (!outw)
	// 	{
	// 		finfo.svrtx[0][1] = fverts[1];
	// 		finfo.svrtx[0][2] = fverts[2];
	// 	}
	// 	else
	// 	{
	// 		finfo.svrtx[0][1] = fverts[2];
	// 		finfo.svrtx[0][2] = fverts[1];
	// 	}
	// 	finfo.domainidx[0] = domainidx;
	// 	facemap[fid] = finfo;
	// }
	// else if ((it->second).procids[0] == procid)
	// { // internal face
	// 	facemap.erase(it);
	// }
	// else
	// { // partition boundary face
	// 	(it->second).procids[1] = procid;
	// 	(it->second).svrtx[1][0] = fverts[0];
	// 	if (!outw)
	// 	{
	// 		(it->second).svrtx[1][1] = fverts[1];
	// 		(it->second).svrtx[1][2] = fverts[2];
	// 	}
	// 	else
	// 	{
	// 		(it->second).svrtx[1][1] = fverts[2];
	// 		(it->second).svrtx[1][2] = fverts[1];
	// 	}
	// 	(it->second).domainidx[1] = domainidx;
	// }
}


//自定义MPI类型
MPI_Datatype fid_xdMeshFaceInfo::MPI_type;
//构建MPI类型

//定义了一个名为 fid_xdMeshFaceInfo 的结构体
//在结构体中添加了一个函数 build_MPIType，用于构建 MPI 自定义数据类型
void fid_xdMeshFaceInfo::build_MPIType() {
	//用于存储每个成员变量的长度
	int block_lengths[5];
	MPI_Aint displacements[5];
	MPI_Aint addresses[5], add_start;
	//用于存储每个成员变量的 MPI 数据类型。
	MPI_Datatype typelist[5];

	//用于获取每个成员变量的地址。
	fid_xdMeshFaceInfo temp;

	typelist[0] = MPI_INT;
	block_lengths[0] = 1;
	// MPI_Get_address 函数，获取了 temp 对象中每个成员变量的地址，并将这些地址存储在 addresses 数组中。
	MPI_Get_address(&temp.fid, &addresses[0]);

	typelist[1] = MPI_INT;
	block_lengths[1] = 2;
	MPI_Get_address(&temp.mfi.procids, &addresses[1]);

	typelist[2] = MPI_INT;
	block_lengths[2] = 6;
	MPI_Get_address(&temp.mfi.svrtx, &addresses[2]);

	typelist[3] = MPI_INT;
	block_lengths[3] = 2;
	MPI_Get_address(&temp.mfi.domainidx, &addresses[3]);

	typelist[4] = MPI_SHORT;
	block_lengths[4] = 1;
	MPI_Get_address(&temp.mfi.outw, &addresses[4]);

	//通过计算每个成员变量相对于 temp 对象起始地址的偏移量，将这些偏移量存储在 displacements 数组中。
	MPI_Get_address(&temp, &add_start);
	for(int i = 0; i < 5; i++) displacements[i] = addresses[i] - add_start;
	//基于上述定义的成员变量数据类型、长度和偏移量，创建了一个新的 MPI 自定义数据类型 MPI_type
	MPI_Type_create_struct(5, block_lengths, displacements, typelist, &MPI_type);
	//
	MPI_Type_commit(&MPI_type);
}


//	-func  : 将提取的子面网格（face_map）合并到std::map中
//	-param : facemap 输出的std::map 面网格
//  -param : sub_face_map 输入的子网格


void Allgather_Face_Map(std::map<int, xdMeshFaceInfo> &facemap, fid_xdMeshFaceInfo *sub_face_map, int ne, int sub_ne, int *every_sub_ne, int *every_offset, double *stage_time) {
	if (stage_time) {
		for (int i = 0; i < 3; i++) stage_time[i] = 0.0;
	}
	double gather_start = MPI_Wtime();

	int rank;
	//获取当前进程的MPI等级
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	//进行数据的同步
	MPI_Barrier(MPI_COMM_WORLD);
	//创建自定义的MPI数据类型
	fid_xdMeshFaceInfo::build_MPIType();
	// fid_xdMeshFaceInfo *sub_face_maps = new fid_xdMeshFaceInfo[ne*4];
	//分配内存空间，创建用于存储合并后面映射数据的数组
	fid_xdMeshFaceInfo *sub_face_maps = (fid_xdMeshFaceInfo*)malloc(sizeof(fid_xdMeshFaceInfo) *(ne)*4);
	double before_allgather = MPI_Wtime();
	if (stage_time) stage_time[0] = before_allgather - gather_start;

	
	/*将每个核所求得的sub_face_map全合并*/
	//将子进程的面映射数据进行全局合并
	MPI_Allgatherv(&sub_face_map[0], sub_ne*4, fid_xdMeshFaceInfo::MPI_type,
	 sub_face_maps, every_sub_ne, every_offset, fid_xdMeshFaceInfo::MPI_type,
	 MPI_COMM_WORLD);
	double after_allgather = MPI_Wtime();
	if (stage_time) stage_time[1] = after_allgather - before_allgather;
	//释放自定义的数据类型
	MPI_Type_free(&fid_xdMeshFaceInfo::MPI_type);
	//遍历合并后的面映射数据数组，根据面ID更新facemap
	for(int i = 0; i < ne*4; i++) {
		int cur_fid = sub_face_maps[i].fid;
		auto it = facemap.find(cur_fid);
		if(it == facemap.end()) {
			facemap[cur_fid] = sub_face_maps[i].mfi;
		} else if((it->second).procids[0] == sub_face_maps[i].mfi.procids[0]) {
			facemap.erase(it);
		} else {
			(it->second).procids[1] = sub_face_maps[i].mfi.procids[0];
			(it->second).svrtx[1][0] = sub_face_maps[i].mfi.svrtx[0][0];
			// if(!sub_face_maps[i].mfi.outw) {
				(it->second).svrtx[1][1] = sub_face_maps[i].mfi.svrtx[0][1];
				(it->second).svrtx[1][2] = sub_face_maps[i].mfi.svrtx[0][2];
			// } else {
			// 	(it->second).svrtx[1][1] = sub_face_maps[i].mfi.svrtx[0][2];
			// 	(it->second).svrtx[1][2] = sub_face_maps[i].mfi.svrtx[0][1];
			// }
			(it->second).domainidx[1] = sub_face_maps[i].mfi.domainidx[0];
		}
	}

	//释放内存空间
	free(sub_face_maps);
	double gather_end = MPI_Wtime();
	if (stage_time) stage_time[2] = gather_end - after_allgather;
}


//该函数主要用于确定面的朝向，根据输入的顶点数组和面顶点数组，
//通过匹配顶点的方式来判断面是否为外向面。如果面为外向面，返回1；否则，返回0。
int ExtractFaceOutward(int *tverts, int *fverts)
{
	int i, j, s, t;
	int sumt;
	int tetraor[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
	// int tetraor[4][3] = { {1,3,2}, {0,2,3}, {0,3,1}, {0,1,2} };
	//使用循环遍历两个顶点数组，找到共享的顶点。
	for (i = 0; i < 2; i++)
	{
		for (s = 0; s < 3; s++)
		{
			if (tverts[i] == fverts[s])
				break;
		}
		if (s < 3)
			break;
	}
	//如果没有找到共享的顶点，则输出错误信息并退出。
	if (s == 3)
	{
		printf("MY: error in outward face computation (1). \n");
		exit(1);
	}

	//使用两层循环遍历四个可能的面方向。
	sumt = 0;
	for (int i = 0; i < 4; i++)
	{
		t = 0;
		for (int j = 0; j < 3; j++)
		{
			//检查顶点数组中的顶点是否与面顶点数组中的顶点相匹配。
			if (tverts[tetraor[i][j]] == fverts[(s + j) % 3])
				t++;
		}
		if (t == 3)
			sumt++;
	}
	//如果匹配的顶点数为3，则表明该面是外向面
	if (sumt == 1)
	{
		return (1);	//如果找到外向面，则返回1。
	} //如果匹配的顶点数大于3，则输出错误信息并退出。
	else if (sumt > 1) //如果存在多个外向面或计算错误，则输出错误信息并退出
	{
		printf("MY: error in outward face computation (2).\n");
		exit(1);
	}
	return (0);
}


//根据三角面的顶点索引在表面映射中查找对应的表面元素
int FindSurfElem(void *mesh, int *elem, surfMap_t &surfMap)
{
	nglib::Ng_Mesh *newMesh = (nglib::Ng_Mesh *)mesh;
	int nse = nglib::Ng_GetNSE(newMesh);
	//将输入的三角面顶点索引排序，以确保索引数据的一致
	std::sort(elem, elem + 3);
	//创建一个包含排序后顶点索引的FacePoints对象。
	FacePoints fp;
	fp.p[0] = elem[0];
	fp.p[1] = elem[1];
	fp.p[2] = elem[2];
	//在表面映射数据结构中查找具有相同顶点索引的键值对
	surfMap_t::const_iterator it = surfMap.find(fp);
	if (it == surfMap.end())
		return -1;		//返回-1表示未找到对应的表面元素。
	else
		return it->second; //如果找到匹配的键值对，返回对应的表面元素。
}

//在网格中创建面元素
void PartFaceCreate(void *mesh, int belongNumberPartition, std::map<int, xdMeshFaceInfo> &facemap,
					int maxbarycoord, void *submesh, std::map<int, int> &g2lvrtxmap, std::map<Barycentric, int, CompBarycentric> &baryc2locvrtxmap, std::list<xdFace> &newfaces)
{
	nglib::Ng_Mesh *newMesh = (nglib::Ng_Mesh *)mesh;
	std::map<int, xdMeshFaceInfo>::iterator itf;
	std::map<int, int>::iterator itv;
	int fid;
	xdMeshFaceInfo finfo;
	xdFace f;
	int lsvrtx[3];
	int pid;
	double xyz[3];
	int idx = -1;
	std::map<IntPair, int, IntPairCompare> facepairs;
	int faceidx;
	int faceNum;
	Barycentric barycv;
	IntPair fpr;
	std::map<IntPair, int>::iterator fi;
	surfMap_t surfMap;
	//获取原始网格中的表面点信息，将其存储在表面映射数据结构surfMap中。
	GetSurfPoints(newMesh, surfMap);
	// add vertices to the mesh
	//遍历facemap中的每个面元素，处理属于当前分区的面元素。
	for (itf = facemap.begin(); itf != facemap.end(); ++itf)
	{
		fid = itf->first;
		finfo = itf->second;
		// insert face vertices into verts map
		for (int j = 0; j < 2; j++)
		{
			pid = finfo.procids[j];
			if (pid == belongNumberPartition)
			{
				// Ng_ AddSurfaceBlement (submesh, et, finfo. svrtx[j]);
				for (int k = 0; k < 3; k++)
				{
					itv = g2lvrtxmap.find(finfo.svrtx[j][k]);
					// finfo. svrtx[j][k] the index of the vertex in which zone
					if (itv == g2lvrtxmap.end())
					{
						// the vertex is not stored in the g2lvrtxmap, then put in
						nglib::Ng_GetPoint((nglib::Ng_Mesh *)mesh, finfo.svrtx[j][k], xyz); // netgen obtains the coordinates of the point based on the point index
						nglib::Ng_AddPoint((nglib::Ng_Mesh *)submesh, xyz, idx);
						// add the resulting coordinates to the new grid
						g2lvrtxmap[finfo.svrtx[j][k]] = idx;
						barycv = InitBarycv(finfo.svrtx[j][k], maxbarycoord);
						baryc2locvrtxmap[barycv] = idx;
					}
				}
			}
		}
	}
	int Isbound;
	// add face element to the grid
	//再次处理属于当前分区的面元素。
	for (itf = facemap.begin(); itf != facemap.end(); ++itf)
	{
		fid = itf->first;
		finfo = itf->second;
		for (int j = 0; j < 2; j++)
		{
			if (j == 1 && finfo.procids[1] == finfo.procids[0])
				// if (finfo.procids[1] == finfo.procids[0])
				break;
			pid = finfo.procids[j]; // get the partition where the face element belongs
			if (pid == belongNumberPartition)
			{
				f.outw = finfo.outw;
				Isbound = (finfo.procids[1] == -1) ? 1 : 0;	 // determine whether it is an interface, not the value of interface geoboundary is set to 1,and the value of interface
				f.lsvrtx[0] = g2lvrtxmap[finfo.svrtx[j][0]]; // f. lsvrts[] stores the id for each face vertex
				f.lsvrtx[1] = g2lvrtxmap[finfo.svrtx[j][1]];
				f.lsvrtx[2] = g2lvrtxmap[finfo.svrtx[j][2]];
				f.barycv[0] = InitBarycv(finfo.svrtx[j][0], maxbarycoord);
				f.barycv[1] = InitBarycv(finfo.svrtx[j][1], maxbarycoord);
				f.barycv[2] = InitBarycv(finfo.svrtx[j][2], maxbarycoord);
				f.geoboundary = FindSurfElem((nglib::Ng_Mesh *)mesh, finfo.svrtx[j], surfMap);

				fpr.x = finfo.domainidx[j];
				fpr.y = finfo.domainidx[(j + 1) % 2];
				if ((f.geoboundary == -1 || finfo.procids[0] != finfo.procids[1]) && !Isbound)
				{
					fi = facepairs.find(fpr);
					if (fi == facepairs.end())
					{
						faceNum = nglib::Ng_GetNFD((nglib::Ng_Mesh *)submesh);
						// faceidx = nglib::My_Ng_AddFaceDescriptor((nglib::Ng_Mesh*)submesh, faceNum, finfo.domainidx[j], 0, 0);
						faceidx = nglib::My_Ng_AddFaceDescriptor((nglib::Ng_Mesh *)submesh, faceNum + 1, finfo.domainidx[j], 0, 0);
						f.geoboundary = faceidx;
						facepairs[fpr] = faceidx;
					}
					else
					{
						f.geoboundary = facepairs[fpr];
					}
				}
				// std::cout << f.geoboundary << std::endl;
				newfaces.push_back(f);
				// place the face element to the newfaces
			}
		}
	}
}

void Refine(void *submesh, int numlevels, int belongNumberPartition, std::list<xdFace> &newfaces, std::map<Barycentric, int, CompBarycentric> &baryc2locvrtxmap, std::map<IntPair, int, IntPairCompare> &edgemap)
{
	int u[3];
	int v[3];
	std::list<xdFace>::iterator li;
	std::map<IntPair, int>::iterator ei;
	xdFace f;
	double vxyz[3][3];
	double uxyz[3][3];
	int listsize;
	IntPair pr;
	Barycentric midvrtxbaryc;
	Barycentric vbarycv[3];
	Barycentric ubarycv[3];
	std::map<Barycentric, int, CompBarycentric>::iterator bi;

	int index = -1;
	li = newfaces.begin();
	for (int l = 0; l < numlevels; l++)
	{
		listsize = newfaces.size();
		// if(belongNumberPartition == 3) std::cout << "listszie:" << listsize << std::endl;
		// std::cout << "id:" << belongNumberPartition << "listsize:" << listsize << std::endl;
		for (int t = 0; t < listsize; t++)
		{
			for (int k = 0; k < 3; k++)
			{
				// Each face has three points
				v[k] = (*li).lsvrtx[k];
				// each face has three points
				vbarycv[k] = (*li).barycv[k];
				nglib::Ng_GetPoint((nglib::Ng_Mesh *)submesh, v[k], vxyz[k]);
			}
			for (int k = 0; k < 3; k++)
			{
				if (v[k] < v[(k + 1) % 3])
				{
					pr.x = v[k];
					pr.y = v[(k + 1) % 3];
				}
				else
				{
					pr.y = v[k];
					pr.x = v[(k + 1) % 3];
				}
				BarycMidPoint(vbarycv[k], vbarycv[(k + 1) % 3], midvrtxbaryc);

				ei = edgemap.find(pr);

				bi = baryc2locvrtxmap.find(midvrtxbaryc);

				if (bi == baryc2locvrtxmap.end())
				{
					uxyz[k][0] = 0.5 * (vxyz[k][0] + vxyz[(k + 1) % 3][0]);
					uxyz[k][1] = 0.5 * (vxyz[k][1] + vxyz[(k + 1) % 3][1]);
					uxyz[k][2] = 0.5 * (vxyz[k][2] + vxyz[(k + 1) % 3][2]);
					nglib::Ng_AddPoint((nglib::Ng_Mesh *)submesh, uxyz[k], index);
					u[k] = index;
					edgemap[pr] = index;
					ubarycv[k] = midvrtxbaryc;
					baryc2locvrtxmap[midvrtxbaryc] = index;
				}
				else
				{
					// u[k] = edgemap[pr];
					u[k] = ei->second;
					ubarycv[k] = midvrtxbaryc;
				}
			}
			// outer triangles
			for (int k = 0; k < 3; k++)
			{
				f.lsvrtx[0] = v[k];
				f.barycv[0] = vbarycv[k];
				f.lsvrtx[1] = u[k];
				f.barycv[1] = ubarycv[k];
				f.lsvrtx[2] = u[(k + 2) % 3];
				f.barycv[2] = ubarycv[(k + 2) % 3];
				f.geoboundary = (*li).geoboundary;
				newfaces.push_back(f);
			}
			// interior triangle
			f.lsvrtx[0] = u[0];
			f.barycv[0] = ubarycv[0];
			f.lsvrtx[1] = u[1];
			f.barycv[1] = ubarycv[1];
			f.lsvrtx[2] = u[2];
			f.barycv[2] = ubarycv[2];
			f.geoboundary = (*li).geoboundary;
			newfaces.push_back(f);
			li = newfaces.erase(li);
		}
	}
	for (li = newfaces.begin(); li != newfaces.end(); ++li)
	{
		// nglib::Ng_AddSurfaceElementwithIndex((nglib::Ng_Mesh*)submesh, nglib::NG_TRIG, (*li).lsvrtx, 1);
		nglib::Ng_AddSurfaceElementwithIndex((nglib::Ng_Mesh *)submesh, nglib::NG_TRIG, (*li).lsvrtx, (*li).geoboundary);

		// nglib::Ng_AddSurfaceElement((nglib::Ng_Mesh*)submesh, nglib::NG_TRIG, (*li).lsvrtx);
	}
}


//初始化重心坐标（Barycentric）结构
Barycentric InitBarycv(int v, int maxbarycoord)
{
	Barycentric p;
	p.gvrtx[0] = 0;
	p.gvrtx[1] = 0;
	p.gvrtx[2] = v;
	p.coord[0] = 0;
	p.coord[1] = 0;
	p.coord[2] = maxbarycoord;
	return (p);
}

//计算两个重心坐标（Barycentric）之间的中点
void BarycMidPoint(Barycentric p1, Barycentric p2, Barycentric &res)
{	
	//创建一个集合 s，用于存储两个重心坐标中非零坐标对应的顶点索引
	std::set<int> s;
	std::set<int>::iterator is;
	int k;
	short w1, w2;
	//遍历 p1 和 p2 的坐标数组，将非零坐标对应的顶点索引插入集合 s 中。
	for (int i = 0; i < 3; i++)
	{
		if (p1.coord[i])
			s.insert(p1.gvrtx[i]);
		if (p2.coord[i])
			s.insert(p2.gvrtx[i]);
	}
	//检查集合 s 的大小，如果超过了 3，说明出现了错误，打印错误信息。
	if (s.size() > 3)
	{
		printf("Error in barycentric coordinates\n");
	}
	// the global result of the center of mass of the third point from the index of teo pl,p2 points (the third point is the insertion point)
	k = 0;
	for (is = s.begin(); is != s.end(); is++)
	{
		res.gvrtx[k] = *is;
		k++;
	}
	//如果集合 s 中的顶点索引不足三个，将剩余的索引用零填充。
	while (k < 3)
	{
		res.gvrtx[k] = 0;
		k++;
	}
	//对 res 的顶点索引数组进行排序。
	SortInt(res.gvrtx);
	// to ate the index of the insertion point
	//针对每个顶点索引，从 p1 和 p2 的坐标数组中找到对应的权重值 w1 和 w2。
	for (int i = 0; i < 3; i++)
	{
	w1 = w2 = 0;
		for (int j = 0; j < 3; j++)
		{
			if (res.gvrtx[i] == p1.gvrtx[j])
			{
				w1 = p1.coord[j];
				break;
			}
		}
		for (int j = 0; j < 3; j++)
		{
			if (res.gvrtx[i] == p2.gvrtx[j])
			{
				w2 = p2.coord[j];
				break;
			}
		}
		// determine whether the index of the pl,p2 two points has the same as the index of the insertion point,and then get the coordinates of the insertion point
		// based on the coordinates of the same index as the pl,p2 two points
		//根据 w1 和 w2 的值，计算插入点的权重值，即取 w1 和 w2 的平均值，并赋值给结果重心坐标 res 的坐标数组 coord。
		res.coord[i] = (w1 + w2) / 2;
	}
}


void Refineforvol(void *submesh, int belongNumberPartition, std::list<xdFace> &newfaces, std::map<Barycentric, int, CompBarycentric> &baryc2locvrtxmap, std::map<IntPair, int, IntPairCompare> &edgemap)
{
	int v[3];
	int u[3];
	std::list<xdFace>::iterator li;
	std::map<IntPair, int>::iterator ei;
	xdFace f;
	double vxyz[3][3];
	double uxyz[3][3];
	int listsize;
	IntPair pr;
	Barycentric midvrtxbaryc;
	Barycentric vbarycv[3];
	Barycentric ubarycv[3];
	std::map<Barycentric, int, CompBarycentric>::iterator bi;
	// std::map< IntPair, int, IntPairCompare >::iterator emi;
	int index = -1;
	int oldnse = nglib::Ng_GetNSE((nglib::Ng_Mesh *)submesh);
	li = newfaces.begin();
	int i = 0;
	for (i = 0; i < oldnse; i++)
	{
		for (int k = 0; k < 3; k++)
		{ // Bach face has three points
			v[k] = (*li).lsvrtx[k];
			// each face has three points
			vbarycv[k] = (*li).barycv[k];
			nglib::Ng_GetPoint((nglib::Ng_Mesh *)submesh, v[k], vxyz[k]);
		}
		for (int k = 0; k < 3; k++)
		{
			if (v[k] < v[(k + 1) % 3])
			{ // Find two points according to the define method and put them into the IntPair pr :
				pr.x = v[k];
				pr.y = v[(k + 1) % 3];
			}
			else
			{
				pr.y = v[k];
				pr.x = v[(k + 1) % 3];
			}
			BarycMidPoint(vbarycv[k], vbarycv[(k + 1) % 3], midvrtxbaryc);
			bi = baryc2locvrtxmap.find(midvrtxbaryc);
			ei = edgemap.find(pr);
			// if (bi = baryc2locvrtxmap. end() {
			if (ei == edgemap.end())
			{
				uxyz[k][0] = 0.5 * (vxyz[k][0] + vxyz[(k + 1) % 3][0]);
				uxyz[k][1] = 0.5 * (vxyz[k][1] + vxyz[(k + 1) % 3][1]);
				uxyz[k][2] = 0.5 * (vxyz[k][2] + vxyz[(k + 1) % 3][2]);
				nglib::Ng_AddPoint((nglib::Ng_Mesh *)submesh, uxyz[k], index);
				u[k] = index;
				edgemap[pr] = index;
				ubarycv[k] = midvrtxbaryc;
				baryc2locvrtxmap[midvrtxbaryc] = index;
			}
			else
			{
				u[k] = edgemap[pr];
				ubarycv[k] = midvrtxbaryc;
			}
		}
		// outer triangles
		for (int k = 0; k < 3; k++)
		{
			f.lsvrtx[0] = v[k];
			f.barycv[0] = vbarycv[k];
			f.lsvrtx[1] = u[k];
			f.barycv[1] = ubarycv[k];
			f.lsvrtx[2] = u[(k + 2) % 3];
			f.barycv[2] = ubarycv[(k + 2) % 3];
			f.geoboundary = (*li).geoboundary;
			newfaces.push_back(f);
		}
		// Ng_ AddSurfaceElement ((nglib::Ng_ Mesh*) submesh, nglib::NG TRIG,f. lsvrtx);
		// interior triangle
		f.lsvrtx[0] = u[0];
		f.barycv[0] = ubarycv[0];
		f.lsvrtx[1] = u[1];
		f.barycv[1] = ubarycv[1];
		f.lsvrtx[2] = u[2];
		f.barycv[2] = ubarycv[2];
		f.geoboundary = (*li).geoboundary;
		newfaces.push_back(f);
		li = newfaces.erase(li);
	}
	nglib::ClearSurfaceElements((nglib::Ng_Mesh *)submesh);
	for (li = newfaces.begin(); li != newfaces.end(); ++li)
	{
		Ng_AddSurfaceElementwithIndex((nglib::Ng_Mesh *)submesh, nglib::NG_TRIG, (*li).lsvrtx, (*li).geoboundary);
	}
	int oldne = nglib::Ng_GetNE((nglib::Ng_Mesh *)submesh);
	int Ev[10];
	double Evxyz[4][3];
	double Euxyz[6][3];
	int domainidx, edg;

	for (i = 0; i < oldne; i++)
	{
		nglib::Ng_GetVolumeElement((nglib::Ng_Mesh *)submesh, i + 1, Ev, domainidx);
		for (int k = 0; k < 4; k++)
		{
			// Each face has three points
			nglib::Ng_GetPoint((nglib::Ng_Mesh *)submesh, Ev[k], Evxyz[k]);
		}
		static int betw[6][3] =
			{{0, 1, 4},
			 {0, 2, 5},
			 {0, 3, 6},
			 {1, 2, 7},
			 {1, 3, 8},
			 {2, 3, 9}};

		for (int k = 0; k < 6; k++)
		{
			if (Ev[betw[k][0]] < Ev[betw[k][1]])
			{ // Find two points according to the define method and put them into the IntPair pr
				pr.x = Ev[betw[k][0]];
				pr.y = Ev[betw[k][1]];
			}
			else
			{
				pr.y = Ev[betw[k][0]];
				pr.x = Ev[betw[k][1]];
			}
			ei = edgemap.find(pr);
			if (ei == edgemap.end())
			{
				Euxyz[k][0] = 0.5 * (Evxyz[betw[k][0]][0] + Evxyz[betw[k][1]][0]);
				Euxyz[k][1] = 0.5 * (Evxyz[betw[k][0]][1] + Evxyz[betw[k][1]][1]);
				Euxyz[k][2] = 0.5 * (Evxyz[betw[k][0]][2] + Evxyz[betw[k][1]][2]);
				nglib::Ng_AddPoint((nglib::Ng_Mesh *)submesh, Euxyz[k], index);
				Ev[k + 4] = index;
				edgemap[pr] = index;
			}
			else
			{
				Ev[k + 4] = edgemap[pr];
			}
		}
		static int reftab[8][4] =
			{{0, 4, 5, 6},
			 {4, 1, 7, 8},
			 {5, 7, 2, 9},
			 {6, 8, 9, 3},
			 {4, 5, 6, 8},
			 {4, 5, 8, 7},
			 {5, 6, 8, 9},
			 {5, 7, 9, 8}};

		int pointindex[4];
		for (int j = 0; j < 8; j++)
		{
			for (int k = 0; k < 4; k++)
			{
				pointindex[k] = Ev[reftab[j][k]];
			}
			if (j == 0)
				nglib::My_Ng_SetVolumeElement((nglib::Ng_Mesh *)submesh, i + 1, pointindex, domainidx);
			else
				nglib::Ng_AddVolumeElement((nglib::Ng_Mesh *)submesh, nglib::NG_TET, pointindex, domainidx);
		}
	}
}

//将顶点和面的关系添加到一个映射中
void insbaryadjlist(
	Barycvrtx bvrtx,
	int pid,
	std::map<Barycvrtx, std::list<int>, CompBarycvrtx> &barycvrtx2adjprocsmap)
{
	std::map<Barycvrtx, std::list<int>, CompBarycvrtx>::iterator ivrtx;
	std::list<int> pidlist;
	std::list<int>::iterator li;
	// printf("%d %d\n" ,vrtx, pid) ;
	ivrtx = barycvrtx2adjprocsmap.find(bvrtx);
	if (ivrtx == barycvrtx2adjprocsmap.end())
	{
		barycvrtx2adjprocsmap[bvrtx] = pidlist;
		barycvrtx2adjprocsmap[bvrtx].push_back(pid);
	}
	else
	{
		for (li = (ivrtx->second).begin(); li != (ivrtx->second).end(); ++li)
		{
			if ((*li) == pid)
				break;
		}
		if (li == (ivrtx->second).end())
		{
			barycvrtx2adjprocsmap[bvrtx].push_back(pid);
		}
	}
}

//在 MPI 通信中发送和接收数据
int com_sr_datatype(
	MPI_Comm comm,
	int num_s,
	int num_r,
	int *dest,
	int *src,
	int *s_length,
	int *r_length,
	Barycentric **s_data,
	Barycentric **r_data,
	MPI_Datatype datatype,
	int mypid)
{
	int i;
	MPI_Status *stat;
	MPI_Request *req;
	int rc;
	if ((num_s + num_r) > 0)
	{
		MYCALLOC(req, MPI_Request *, (num_s + num_r), sizeof(MPI_Request));
		MYCALLOC(stat, MPI_Status *, (num_s + num_r), sizeof(MPI_Status));
	}
	else
	{
		return (MPI_SUCCESS);
	}
	for (i = 0; i < num_s; i++)
	{
		rc = MPI_Isend(s_data[i], s_length[i], datatype, dest[i], mypid, comm,
					   &(req[i]));
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	for (i = 0; i < num_r; i++)
	{
		rc = MPI_Irecv(r_data[i], r_length[i], datatype, src[i], src[i], comm,
					   &(req[num_s + i]));
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	if (num_s + num_r)
	{
		rc = MPI_Waitall(num_s + num_r, req, stat);
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	free(req);
	free(stat);
	return (rc);
}

//在 MPI 通信中发送和接收整数数据
int com_sr_int(
	MPI_Comm comm,
	int num_s,
	int num_r,
	int *dest,
	int *src,
	int **s_data,
	int **r_data,
	int mypid)
{
	int i;
	MPI_Status *stat;
	MPI_Request *req;
	int rc;
	if ((num_s + num_r) > 0)
	{
		MYCALLOC(req, MPI_Request *, (num_s + num_r), sizeof(MPI_Request));
		MYCALLOC(stat, MPI_Status *, (num_s + num_r), sizeof(MPI_Status));
	}
	else
	{
		return (MPI_SUCCESS);
	}
	for (i = 0; i < num_s; i++)
	{
		rc = MPI_Isend(s_data[i], 1, MPI_INT, dest[i], mypid, comm,
					   &(req[i]));
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	for (i = 0; i < num_r; i++)
	{
		rc = MPI_Irecv(r_data[i], 1, MPI_INT, src[i], src[i], comm,
					   &(req[num_s + i]));
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	if (num_s + num_r)
	{
		rc = MPI_Waitall(num_s + num_r, req, stat);
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	free(req);
	free(stat);
	return (rc);
}

//在 MPI 通信中发送和接收 xdVElement 结构体数据
int com_sr_volumelement(
	MPI_Comm comm,
	int num_s,
	int num_r,
	int *dest,
	int *src,
	int *s_length,
	int *r_length,
	xdVElement **s_data,
	xdVElement **r_data,
	MPI_Datatype datatype,
	int mypid)
{
	int i;
	MPI_Status *stat;
	MPI_Request *req;
	int rc;
	if ((num_s + num_r) > 0)
	{
		MYCALLOC(req, MPI_Request *, (num_s + num_r), sizeof(MPI_Request));
		MYCALLOC(stat, MPI_Status *, (num_s + num_r), sizeof(MPI_Status));
	}
	else
	{
		return (MPI_SUCCESS);
	}
	for (i = 0; i < num_s; i++)
	{
		rc = MPI_Isend(s_data[i], s_length[i], datatype, dest[i], mypid, comm,
					   &(req[i]));
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	for (i = 0; i < num_r; i++)
	{
		rc = MPI_Irecv(r_data[i], r_length[i], datatype, src[i], src[i], comm,
					   &(req[num_r + i]));
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	if (num_s + num_r)
	{
		rc = MPI_Waitall(num_s + num_r, req, stat);
		if (rc != MPI_SUCCESS)
		{
			printf("Error in mpi\n");
			exit(1);
		}
	}
	free(req);
	free(stat);
	return (rc);
}


//函数会计算出每个顶点与其邻接进程之间的关系，并将结果存储在 barycvrtx2adjprocsmap 中。
void computeadj(
	int mypid, std::map<int, xdMeshFaceInfo> &facemap, std::map<int, int> &g2lvrtxmap,
	std::map<Barycvrtx, std::list<int>, CompBarycvrtx> &barycvrtx2adjprocsmap)
{
	std::map<int, xdMeshFaceInfo>::iterator itf;
	std::map<int, int>::iterator itv;
	int fid;
	xdMeshFaceInfo finfo;
	int pid, i;
	std::map<Barycvrtx, std::list<int>, CompBarycvrtx>::iterator ib; // barycvrtx ȫ�ֶ���id������(��1��ʼ)
	std::list<int>::iterator li;
	int count;
	Barycvrtx bvrtx;
	Barycvrtx countbvrtx;

	for (itf = facemap.begin(); itf != facemap.end(); ++itf)
	{
		fid = itf->first;
		finfo = itf->second;
		if ((finfo.procids[0] != mypid) && (finfo.procids[1] == -1))
			continue;
		if ((finfo.procids[1] != mypid) && (finfo.procids[0] == -1))
			continue;
		if (finfo.procids[0] == finfo.procids[1])
			continue;
		for (int j = 0; j < 2; j++)
		{
			pid = finfo.procids[j];
			if ((pid == mypid) || (pid == -1))
				continue; // �ҽ����� pid != mypid II pid != 1��������ִ��,����ִ����һ��ѭ��
			count = 0;
			for (int k = 0; k < 3; k++)
			{
				countbvrtx.gvrtx[k] = 0;
				itv = g2lvrtxmap.find(finfo.svrtx[j][k]);
				if (itv != g2lvrtxmap.end())
				{
					bvrtx.gvrtx[0] = 0;
					bvrtx.gvrtx[1] = 0;
					bvrtx.gvrtx[2] = finfo.svrtx[j][k];
					countbvrtx.gvrtx[k] = finfo.svrtx[j][k]; // ���ϵ������
					count++;
					insbaryadjlist(bvrtx, pid, barycvrtx2adjprocsmap); // ��������ĵ���������ǩ
				}
			}
			sort3int(countbvrtx.gvrtx);
			if (count == 2)
			{															// insert edge adjacency
				insbaryadjlist(countbvrtx, pid, barycvrtx2adjprocsmap); // ��������.�ϵı߼��������ǩ
			}
			if (count == 3)
			{ // insert edge and face adjacencies ���� �ߺ����ڽ�
				bvrtx.gvrtx[0] = 0;
				bvrtx.gvrtx[1] = countbvrtx.gvrtx[0];
				bvrtx.gvrtx[2] = countbvrtx.gvrtx[1];
				insbaryadjlist(bvrtx, pid, barycvrtx2adjprocsmap);
				bvrtx.gvrtx[0] = 0;
				bvrtx.gvrtx[1] = countbvrtx.gvrtx[0];
				bvrtx.gvrtx[2] = countbvrtx.gvrtx[2];
				insbaryadjlist(bvrtx, pid, barycvrtx2adjprocsmap);
				bvrtx.gvrtx[0] = 0;
				bvrtx.gvrtx[1] = countbvrtx.gvrtx[1];
				bvrtx.gvrtx[2] = countbvrtx.gvrtx[2];
				insbaryadjlist(bvrtx, pid, barycvrtx2adjprocsmap);
				insbaryadjlist(countbvrtx, pid, barycvrtx2adjprocsmap); // face adjacency�� �ڽ�
			}
		}
	}
}

int *com_barycoords(
	void *submesh,
	MPI_Comm comm,
	std::map<Barycvrtx, std::list<int>, CompBarycvrtx> &barycvrtx2adjprocsmap,
	std::map<Barycentric, int, CompBarycentric> &baryc2locvrtxmap,
	std::map<int, std::list<int>> &adjbarycs,
	int numprocs, int *newgVEid,
	int mypid)
{
	int count = 3;
	int blocklens[3];
	MPI_Aint addrs[3];
	MPI_Datatype mpitypes[3];
	MPI_Datatype mpibaryctype;
	int num_s, num_r; // number of sends and receives ���ͺͽ��յ�����
	int *dest, *src;

	int *s_length, *r_length;
	// sent/received data size

	Barycentric **s_data, **r_data; // sent / received data

	Barycentric brcy;
	// struct used to get struct member

	std::map<Barycvrtx, std::list<int>>::iterator ibc;
	std::list<int>::iterator li;
	std::map<Barycentric, int, CompBarycentric>::iterator ib;
	Baryvrtx bvrtx;
	std::map<int, BarycVector *>::iterator ipdt;
	std::map<int, int>::iterator srcit; // new
	int i, j, indxowner, ownerpid, locid;
	std::vector<int> holders;
	int numverts, numNEs;
	int *globoffsets, *globoffsetsml;
	int *globoffsetsVE, *globoffsetsmlVE;
	std::map<int, BarycVector *> pidmap;
	std::map<int, int> srcmap; // new
	int newglobalnocounter = 0;
	int newglobalVEocounter = 0;
	int *newgid;
	std::list<int> pids;
	// initialize new global ids array ��ʼ���µ�ȫ��id����
	numverts = nglib::Ng_GetNP((nglib::Ng_Mesh *)submesh);
	numNEs = nglib::Ng_GetNE((nglib::Ng_Mesh *)submesh);
	MYCALLOC(newgid, int *, (numverts + 1), sizeof(int)); // ids start with 1,initialized to 0 ids��1��ʼ, ��ʼ��Ϊ0
	num_s = 0;
	num_r = 0;
	// construct MPI Datatype MPI������������
	blocklens[0] = 3;
	blocklens[1] = 3;
	blocklens[2] = 1;
	mpitypes[0] = MPI_INT;
	mpitypes[1] = MPI_SHORT;
	mpitypes[2] = MPI_INT;
	//MPI_Address(&brcy.gvrtx, addrs);
	//MPI_Address(&brcy.coord, addrs + 1);
	//MPI_Address(&brcy.newgid, addrs + 2);
    MPI_Get_address(&brcy.gvrtx, addrs);
	MPI_Get_address(&brcy.coord, addrs + 1);
	MPI_Get_address(&brcy.newgid, addrs + 2);

	addrs[1] = addrs[1] - addrs[0];
	addrs[2] = addrs[2] - addrs[0];
	addrs[0] = (MPI_Aint)0;
	//MPI_Type_struct(count, blocklens, addrs, mpitypes, &mpibaryctype);
    MPI_Type_create_struct(count, blocklens, addrs, mpitypes, &mpibaryctype);
	MPI_Type_commit(&mpibaryctype);
	newglobalnocounter = 0; // initialize global number counter��ʼ��ȫ �����ּ�����
	// Loop goes over the geometric and partition boundary vertices in the new meshѭ���� ���������еļ��κͻ��ֱ߽綥��
	int length = 0;
	for (ib = baryc2locvrtxmap.begin(); ib != baryc2locvrtxmap.end(); ++ib)
	{
		brcy = ib->first;
		locid = ib->second;

		// ���߽綥�������ȫ�ֶ�������.
		bvrtx.gvrtx[0] = brcy.gvrtx[0];
		bvrtx.gvrtx[1] = brcy.gvrtx[1];
		bvrtx.gvrtx[2] = brcy.gvrtx[2];
		ibc = barycvrtx2adjprocsmap.find(bvrtx);
		if (ibc == barycvrtx2adjprocsmap.end())
		{
			continue;
		}
		length++;
		pids = ibc->second;
		adjbarycs[locid] = pids;
		// compute owners of shared vertices(held by multiple processors - called holders) ������ ��ļ���������(�ɶ������������ - -��Ϊ������)
		holders.clear();
		holders.push_back(mypid); // i am also a holder
		for (li = (ibc->second).begin(); li != (ibc->second).end(); ++li)
		{
			holders.push_back((*li));
		}

		std::sort(holders.begin(), holders.end());
		indxowner = (brcy.gvrtx[0] + brcy.gvrtx[1] + brcy.gvrtx[2] +
					 brcy.coord[0] + brcy.coord[1] + brcy.coord[2]) %
					(ibc->second).size();
		ownerpid = holders[indxowner]; // if I am the owner, append it to the message to be sent �������������,���丽�ӵ�Ҫ���͵���Ϣ��
		if (ownerpid == mypid)
		{
			newglobalnocounter++;
			newgid[locid] = newglobalnocounter;
			brcy.newgid = newglobalnocounter;
			for (li = (ibc->second).begin(); li != (ibc->second).end(); ++li)
			{
				ipdt = pidmap.find((*li));
				if (ipdt == pidmap.end())
				{
					pidmap[(*li)] = new BarycVector();
				}
				pidmap[(*li)]->push_back(brcy);
			}
		}
		else
		{ // i am not the owner of this partition boundary vertex �Ҳ�������ֿ�߽綥���������
			newgid[locid] = -1;
			srcit = srcmap.find(ownerpid);
			if (srcit == srcmap.end())
			{
				srcmap[ownerpid] = 1;
			}
			else
			{
				srcit->second = srcit->second + 1;
			}
		}
	}
	for (locid = 1; locid <= numverts; locid++)
	{
		if (newgid[locid] == 0)
		{
			newglobalnocounter++;
			newgid[locid] = newglobalnocounter;
		}
	}
	// compute pre - scan of all newglobalnocounter in array globoffsets
	MYCALLOC(globoffsetsml, int *, (numprocs + 1), sizeof(int));
	globoffsets = globoffsetsml + 1;
	globoffsets[-1] = 0;
	MPI_Allgather(&newglobalnocounter, 1, MPI_INT, globoffsets, 1, MPI_INT, comm);
	for (i = 0; i < numprocs; i++)
		globoffsets[i] += globoffsets[i - 1];
	// add offsets to global numbers
	// new
	for (locid = 1; locid <= numverts; locid++)
	{
		if (newgid[locid] != -1)
		{
			newgid[locid] += globoffsets[mypid - 1];
		}
	}
	// now do global numbering of the interior vertices ���ڶ��ڲ��嵥Ԫ����ȫ�ֱ��
	int locVEid;
	// compute pre - scan of all newglobalnocounter in array globoffsets
	MYCALLOC(globoffsetsmlVE, int *, (numprocs + 1), sizeof(int));
	globoffsetsVE = globoffsetsmlVE + 1;
	globoffsetsVE[-1] = 0;
	MPI_Allgather(&numNEs, 1, MPI_INT, globoffsetsVE, 1, MPI_INT, comm);
	for (i = 0; i < numprocs; i++)
		globoffsetsVE[i] += globoffsetsVE[i - 1];
	// add offsets to global numbers
	// new
	for (locVEid = 1; locVEid <= numNEs; locVEid++)
	{
		newgVEid[locVEid] += locVEid + globoffsetsVE[mypid - 1];
	}
	// new
	num_s = pidmap.size();
	if (num_s > 0)
	{
		MYCALLOC(s_length, int *, num_s, sizeof(int));
		MYCALLOC(dest, int *, num_s, sizeof(int));
		MYCALLOC(s_data, Barycentric **, num_s, sizeof(Barycentric *));
	}
	else
	{
		dest = nullptr;
		s_length = nullptr;
		s_data = nullptr;
	}
	i = 0;
	for (ipdt = pidmap.begin(); ipdt != pidmap.end(); ++ipdt)
	{
		s_length[i] = ipdt->second->size();
		dest[i] = ipdt->first;
		s_data[i] = &((*ipdt->second)[0]);
		i++;
	}
	num_r = srcmap.size();
	if (num_r > 0)
	{
		MYCALLOC(r_length, int *, num_r, sizeof(int));
		MYCALLOC(src, int *, num_r, sizeof(int));
		MYCALLOC(r_data, Barycentric **, num_r, sizeof(Barycentric *));
	}
	else
	{
		src = nullptr;
		r_length = nullptr;
		r_data = nullptr;
	}
	// compute lengths of messages (no. of items) that will be sent
	// new
	for (srcit = srcmap.begin(), i = 0; srcit != srcmap.end(); ++srcit, ++i)
	{
		src[i] = srcit->first;
		r_length[i] = srcit->second;
	}
	// new
	for (i = 0; i < num_r; i++)
	{
		MYCALLOC(r_data[i], Barycentric *, r_length[i], sizeof(Barycentric));
	}
	com_sr_datatype(comm, num_s, num_r, dest, src, s_length, r_length,
					s_data, r_data, mpibaryctype, mypid);
	for (i = 0; i < num_r; i++)
	{
		for (j = 0; j < r_length[i]; j++)
		{
			locid = baryc2locvrtxmap[r_data[i][j]];
			if (newgid[locid] != -1)
				printf("%d> Error: remote global id %d\n", mypid, locid);
			// newgid[locid] = -(r_ data[i][j]. newgid + globoffsets[src[i] - 1]);
			newgid[locid] = (r_data[i][j].newgid + globoffsets[src[i] - 1]);
		}
	}
	for (i = 0; i < num_r; i++)
	{
		free(r_data[i]);
	}
	if (num_s > 0)
	{
		free(s_length);
		free(s_data);
		free(dest);
	}
	if (num_r > 0)
	{
		free(r_length);
		free(r_data);
		free(src);
	}
	free(globoffsetsml);
	free(globoffsetsmlVE);
	MPI_Type_free(&mpibaryctype);
	return newgid;
}

int *com_baryVolumeElements(
	void *submesh,
	MPI_Comm comm,
	std::map<Barycvrtx, std::list<int>, CompBarycvrtx> &barycvrtx2adjprocsmap,
	std::map<Barycentric, int, CompBarycentric> &baryc2locvrtxmap,
	std::map<int, std::list<int>> &adjbarycs,
	int *oldgid,
	int *VEgid,
	std::list<VEindex> &VEindexs,
	int numprocs,
	int mypid)
{
	nglib::Ng_Mesh *mesh = (nglib::Ng_Mesh *)submesh;
	int locid;
	std::list<int> pids;
	std::list<int>::iterator li;
	std::map<int, std::list<int>>::iterator ib;
	std::map<int, VEVector *> pidmap;
	std::map<int, VEVector *>::iterator ipm; // ������ ��id���ڵ������嵥Ԫ
	int num_keys = nglib::Ng_GetNE(mesh);
	std::set<int> pid_tmp;	// �嵥Ԫ��Ӧ�Ľ���
	std::set<int> vols_tmp; // �ᵥԪ����,��ʱ�����Ѿ�������ĵ�Ԫ,�����ظ�����
	std::set<int>::iterator pt;
	int i;
	int domainidx;
	double xyz[3];
	xdVElement ve;
	// construct MPI Datatype MPI���� ��������
	int count = 4;
	int blocklens[4];
	MPI_Aint addrs[4];
	MPI_Datatype mpitypes[4];
	MPI_Datatype mpivetype;
	blocklens[0] = 4;
	blocklens[1] = 12;
	blocklens[2] = 1;
	blocklens[3] = 1;
	mpitypes[0] = MPI_INT;
	mpitypes[1] = MPI_DOUBLE;
	mpitypes[2] = MPI_INT;
	mpitypes[3] = MPI_INT;
	//MPI_Address(&ve.Pindex, addrs);
	//MPI_Address(&ve.Vertexs, addrs + 1);
	//MPI_Address(&ve.gid, addrs + 2);
	//MPI_Address(&ve.domidx, addrs + 3);

    MPI_Get_address(&ve.Pindex, addrs);
    MPI_Get_address(&ve.Vertexs, addrs + 1);
    MPI_Get_address(&ve.gid, addrs + 2);
    MPI_Get_address(&ve.domidx, addrs + 3);
	addrs[3] = addrs[3] - addrs[0];
	addrs[2] = addrs[2] - addrs[0];
	addrs[1] = addrs[1] - addrs[0];
	addrs[0] = (MPI_Aint)0;
	//MPI_Type_struct(count, blocklens, addrs, mpitypes, &mpivetype);
	MPI_Type_create_struct(count, blocklens, addrs, mpitypes, &mpivetype);
	MPI_Type_commit(&mpivetype);
	int entity[4];
	int *vols;
	int volsize;

	for (i = 0; i < num_keys; i++)
	{
		nglib::Ng_GetVolumeElement(mesh, i + 1, entity, domainidx);
		pid_tmp.clear();
		std::map<int, int> num_adjPoints;
		std::map<int, int>::iterator igap;
		num_adjPoints.clear();
		
		for (int k = 0; k < 4; k++)
		{
			ib = adjbarycs.find(entity[k]);
			if (ib != adjbarycs.end())
			{
				for (li = (ib->second).begin(); li != (ib->second).end(); ++li)
				{
					num_adjPoints[*li]++;
				}
			}
		}

		for (igap = num_adjPoints.begin(); igap != num_adjPoints.end(); ++igap)
		{
			if (igap->second >= 3)
			{
				pid_tmp.insert(igap->first);
			}
		}
		if (pid_tmp.size())
		{

			for (int k = 0; k < 4; k++)
			{
				nglib::Ng_GetPoint(mesh, entity[k], xyz);
				ve.Vertexs[k].xyz[0] = xyz[0];
				ve.Vertexs[k].xyz[1] = xyz[1];
				ve.Vertexs[k].xyz[2] = xyz[2];
				ve.Pindex[k] = oldgid[entity[k]];
			}
			ve.domidx = domainidx;
			ve.gid = VEgid[i + 1];
			for (pt = pid_tmp.begin(); pt != pid_tmp.end(); ++pt)
			{
				ipm = pidmap.find((*pt));
				if (ipm == pidmap.end())
				{
					pidmap[(*pt)] = new VEVector();
				}
				pidmap[(*pt)]->push_back(ve);
			}
		}
	}

	int num_s, num_r; // number of sends and receives ���ͺͽ��յ�����
	num_s = num_r = pidmap.size();
	int *dest, *src;
	int **s_data, **r_data, *size;
	if (num_s > 0)
	{
		MYCALLOC(dest, int *, num_s, sizeof(int));
		MYCALLOC(s_data, int **, num_s, sizeof(int *));
		MYCALLOC(src, int *, num_s, sizeof(int));
		MYCALLOC(r_data, int **, num_s, sizeof(int *));
		MYCALLOC(size, int *, num_s, sizeof(int));
	}
	else
	{
		dest = nullptr;
		s_data = nullptr;
		src = nullptr;
		r_data = nullptr;
		size = nullptr;
	}
	for (i = 0; i < num_r; i++)
	{
		MYCALLOC(r_data[i], int *, 1, sizeof(int));
	}
	i = 0;
	for (ipm = pidmap.begin(); ipm != pidmap.end(); ++ipm)
	{
		dest[i] = ipm->first;
		src[i] = ipm->first;
		size[i] = ipm->second->size();
		s_data[i] = &size[i];
		// std::cout << "myid" << mypid << "dest:" << dest[i] << "size:" << size[i] << std::endl;
		i++;
	}
	com_sr_int(comm, num_s, num_r, dest, src, s_data, r_data, mypid);
	xdVElement **r_data_ves, **s_data_ves;
	int *r_length, *s_length;
	if (num_s > 0)
	{
		MYCALLOC(s_length, int *, num_s, sizeof(int));
		MYCALLOC(s_data_ves, xdVElement **, num_s, sizeof(xdVElement *));
		MYCALLOC(r_length, int *, num_r, sizeof(int));
		MYCALLOC(r_data_ves, xdVElement **, num_r, sizeof(xdVElement *));
	}
	else
	{
		r_length = nullptr;
		r_data_ves = nullptr;
		s_length = nullptr;
		s_data_ves = nullptr;
	}
	i = 0;
	for (ipm = pidmap.begin(); ipm != pidmap.end(); ++ipm)
	{
		s_length[i] = *s_data[i];
		r_length[i] = *r_data[i];
		s_data_ves[i] = &((*ipm->second)[0]);
		i++;
	}
	for (i = 0; i < num_r; i++)
	{
		MYCALLOC(r_data_ves[i], xdVElement *, r_length[i], sizeof(xdVElement));
	}
	// send/recv all the messages
	com_sr_volumelement(comm, num_s, num_r, dest, src, s_length, r_length,
						s_data_ves, r_data_ves, mpivetype, mypid);
	int oldpointnum;
	oldpointnum = nglib::Ng_GetNP(mesh);
	std::map<int, int> gid2lid;
	std::map<int, int>::iterator ig2l;
	std::map<int, int> gids_add;
	std::map<int, int>::iterator iga;
	std::map<int, int> gidVEs_add;
	std::map<int, int>::iterator igaVE;
	int index = -1;
	int pi[4];
	for (i = 0; i < nglib::Ng_GetNP(mesh); i++)
	{
		gid2lid[oldgid[i]] = i;
	}
	int numVEcount, numVEold;
	numVEcount = nglib::Ng_GetNE(mesh);
	numVEold = nglib::Ng_GetNE(mesh);
	for (i = 0; i < num_r; i++)
	{
		// std::cout << mypid << "-" << i << "-" << r_length[i] << std::endl;
		for (int j = 0; j < r_length[i]; j++)
		{
			ve = r_data_ves[i][j];

			for (int k = 0; k < 4; k++)
			{
				ig2l = gid2lid.find(ve.Pindex[k]);
				if (ig2l == gid2lid.end())
				{
					nglib::Ng_AddPoint(mesh, ve.Vertexs[k].xyz, index);
					gids_add[index] = ve.Pindex[k];
					gid2lid[ve.Pindex[k]] = index;
					pi[k] = index;
				}
				else
				{
					pi[k] = ig2l->second;
				}
			}
			nglib::Ng_AddVolumeElement(mesh, nglib::NG_TET, pi, ve.domidx);
			numVEcount++;
			gidVEs_add[numVEcount] = ve.gid;
		}
	}

	int *newgid;
	MYCALLOC(newgid, int *, nglib::Ng_GetNP(mesh) + 1, sizeof(int));
	for (i = 1; i < oldpointnum + 1; i++)
		newgid[i] = oldgid[i];
	for (iga = gids_add.begin(); iga != gids_add.end(); ++iga)
		newgid[iga->first] = iga->second;
	VEindex vei;
	for (i = 1; i < numVEold + 1; i++)
	{
		vei.gid = VEgid[i];

		vei.Isin = 0;
		VEindexs.push_back(vei);
	}
	for (igaVE = gidVEs_add.begin(); igaVE != gidVEs_add.end(); ++igaVE)
	{
		vei.gid = igaVE->second;
		vei.Isin = 1;
		VEindexs.push_back(vei);
	}
	for (i = 0; i < num_r; i++)
	{
		free(r_data[i]);
		free(r_data_ves[i]);
	}
	if (num_s > 0)
	{
		free(dest);
		free(src);
		free(size);
		free(r_length);
		free(r_data_ves);
		free(s_length);
		free(s_data_ves);

		free(s_data);
		free(r_data);
	}
	MPI_Type_free(&mpivetype);
	free(oldgid);
	return newgid;
}

void Record_LWR_count(double LWR, int *count)
{ // record count of length_width_ratio in each interval
	if (LWR >= 1 && LWR < 1.5)
		count[0]++;
	if (LWR >= 1.5 && LWR < 2)
		count[1]++;
	if (LWR >= 2 && LWR < 3)
		count[2]++;
	if (LWR >= 3 && LWR < 4)
		count[3]++;
	if (LWR >= 4 && LWR < 5)
		count[4]++;
	if (LWR >= 5 && LWR < 6)
		count[5]++;
}

bool meshQualityEvaluation(void *mesh, int id, std::string OUTPUT_PATH)
{
	nglib::Ng_Mesh *newMesh = (nglib::Ng_Mesh *)mesh;
	int surfidx;
	int nse = nglib::Ng_GetNSE(newMesh);
	int np = nglib::Ng_GetNP(newMesh);
	int *surfpoints = new int[3];
	double xyz[3][3];
	double LWR;
	double LWR_sum = 0;
	double LWR_max = 0;
	double LWR_min = 0x3f3f3f;
	double JAC;
	double JAC_sum = 0;
	double JAC_max = 0;
	double JAC_min = 0x3f3f3f;
	double MinIA;
	double MaxIA;
	double IA_max = 0;
	double IA_min = 0x3f3f3f;
	double TRIS;
	double TRIS_sum = 0;
	double TRIS_max = 0;
	double TRIS_min = 0x3f3f3f;

	int Aspect_Ratio_count[6] = {0, 0, 0, 0, 0, 0}; // 记录长宽比

	for (int i = 0; i < nse; i++)
	{
		nglib::Ng_GetSurfaceElement(newMesh, i + 1, surfpoints, surfidx);
		for (int k = 0; k < 3; k++)
		{ // Each face has three points
			nglib::Ng_GetPoint((nglib::Ng_Mesh *)newMesh, surfpoints[k], xyz[k]);
		}
		LWR = length_width_ratio(xyz);

		Record_LWR_count(LWR, Aspect_Ratio_count);

		LWR_sum += LWR;
		LWR_min = (std::min)(LWR_min, LWR);
		LWR_max = (std::max)(LWR_max, LWR);
		// JAC = triangle_jacobian_ratio(xyz) ;
		// JAC_sum += JAC;
		// JAC_min = (std::min)(JAC_min,JAC) ;
		// JAC_max = (std::max)(JAC_max,JAC) ;
		MinIA = min_internal_angle(xyz);
		MaxIA = max_internal_angle(xyz);
		IA_min = (std::min)(IA_min, MinIA);
		IA_max = (std::max)(IA_max, MaxIA);
		TRIS = triangle_skew(xyz);
		TRIS_sum += TRIS;
		TRIS_min = (std::min)(TRIS_min, TRIS);
		TRIS_max = (std::max)(TRIS_max, TRIS);
	}
	int *Sum_Aspect_Ratio_count;
	if(id == 0)
		Sum_Aspect_Ratio_count = new int(6);
	for(int i = 0; i < 6; i++)
		MPI_Reduce(&Aspect_Ratio_count[i],&Sum_Aspect_Ratio_count[i], 1 , MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
	if(id == 0)
	{
		// printf("Sum_Aspect_Ratio_count : %d\n",Sum_Aspect_Ratio_count[0]);
		std::string savename = OUTPUT_PATH + "meshQuality/meshQuality.txt";
		FILE *fp = std::fopen(savename.c_str(), "w");
		int Sum_Count_Surface = 0;
		for(int i = 0; i < 6; i++)
			Sum_Count_Surface += Sum_Aspect_Ratio_count[i];
		// for(int i = 0; i < 6; i++)
		fprintf(fp, "Sum_Aspect_Ratio(1-1.5): %f \r\n", (float)Sum_Aspect_Ratio_count[0]/(float)Sum_Count_Surface);
		fprintf(fp, "Sum_Aspect_Ratio(1.5-2): %f \r\n", (float)Sum_Aspect_Ratio_count[1]/(float)Sum_Count_Surface);
		fprintf(fp, "Sum_Aspect_Ratio(2-3): %f \r\n", (float)Sum_Aspect_Ratio_count[2]/(float)Sum_Count_Surface);
		fprintf(fp, "Sum_Aspect_Ratio(3-4): %f \r\n", (float)Sum_Aspect_Ratio_count[3]/(float)Sum_Count_Surface);
		fprintf(fp, "Sum_Aspect_Ratio(4-5): %f \r\n", (float)Sum_Aspect_Ratio_count[4]/(float)Sum_Count_Surface);
		fprintf(fp, "Sum_Aspect_Ratio(5-6): %f \r\n", (float)Sum_Aspect_Ratio_count[5]/(float)Sum_Count_Surface);
		delete []Sum_Aspect_Ratio_count;
		std::fclose(fp);
	}
	int volidx;
	int ne = nglib::Ng_GetNE(newMesh);
	int *volpoints = new int[4];
	double Vxyz[4][3];
	double VLWR;
	double VLWR_sum = 0;
	double VLWR_max = 0;
	double VLWR_min = 0x3f3f3f;
	double VJAC;
	double VJAC_sum = 0;
	double VJAC_max = 0;
	double VJAC_min = 0x3f3f3f;
	double VMinIA;
	double VMaxIA;
	double VIA_max = 0;
	double VIA_min = 0x3f3f3f;
	double VTRIS;
	double VTRIS_sum = 0;
	double VTRIS_max = 0;
	double VTRIS_min = 0x3f3f3f;
	for (int i = 0; i < ne; i++)
	{
		nglib::Ng_GetVolumeElement(newMesh, i + 1, volpoints, volidx);
		for (int k = 0; k < 4; k++)
		{ // Each face has three points
			nglib::Ng_GetPoint((nglib::Ng_Mesh *)newMesh, volpoints[k], Vxyz[k]);
		}
		VLWR = tetrahedrons_length_width_ratio(Vxyz);
		VLWR_sum += VLWR;
		VLWR_min = (std::min)(VLWR_min, VLWR);
		VLWR_max = (std::max)(VLWR_max, VLWR);
		// VJAC = tetrahedrons_jacobian_ratio(Vxyz) ;
		// VJAC_sum += VJAC;
		// VJAC_min = (std::min)(VJAC_min,VJAC) ;
		// VJAC_max = (std::max)(VJAC_max,VJAC) ;
		VIA_min = (std::min)(VIA_min, VMinIA);
		VIA_max = (std::max)(VIA_max, VMaxIA);
		VTRIS = tetrahedrons_skew(Vxyz);
		VTRIS_sum += VTRIS;
		VTRIS_min = (std::min)(VTRIS_min, VTRIS);
		VTRIS_max = (std::max)(VTRIS_max, VTRIS);
		double face_xyz[4][3][3];
		int u[4][3] =
			{
				{0, 1, 2},
				{0, 1, 3},
				{0, 2, 3},
				{1, 2, 3},
			};
		for (int k = 0; k < 4; k++)
			for (int j = 0; j < 3; j++)
			{
				face_xyz[k][j][0] = Vxyz[u[k][j]][0];
				face_xyz[k][j][1] = Vxyz[u[k][j]][1];
				face_xyz[k][j][2] = Vxyz[u[k][j]][2];
			}
		double face_VMinIA[4];
		double face_VMaxIA[4];
		for (int k = 0; k < 4; k++)
		{
			face_VMinIA[k] = min_internal_angle(face_xyz[k]);
			face_VMaxIA[k] = max_internal_angle(face_xyz[k]);
		}
		VMinIA = (std::min)({face_VMinIA[0], face_VMinIA[1], face_VMinIA[2], face_VMinIA[3]});
		VMaxIA = (std::max)({face_VMaxIA[0], face_VMaxIA[1], face_VMaxIA[2], face_VMaxIA[3]});
	}

	std::string savename = OUTPUT_PATH + "meshQuality/meshQuality" + std::to_string(id) + ".txt";
	FILE *fp = std::fopen(savename.c_str(), "w");
	if (fp == NULL)
	{
		std::cout << "File " << savename.c_str() << "canot open" << std::endl;
	}
	else
	{

		fprintf(fp, "Point_Num: %d SurfEle_Num: %d SoildEle_Num: %d \r\n", np, nse, ne);
		fprintf(fp, "\r\n");
		fprintf(fp, "triangle_length_width_ratio_min: %f \r\n", LWR_min);
		fprintf(fp, "triangle_length_width_ratio_max: %f \r\n", LWR_max);
		fprintf(fp, "triangle_length_width_ratio_mean: %f \r\n", LWR_sum / nse);

		fprintf(fp, "Aspect_Ratio(1-1.5): %d \r\n", Aspect_Ratio_count[0]);
		fprintf(fp, "Aspect_Ratio(1.5-2): %d \r\n", Aspect_Ratio_count[1]);
		fprintf(fp, "Aspect_Ratio(2-3): %d \r\n", Aspect_Ratio_count[2]);
		fprintf(fp, "Aspect_Ratio(3-4): %d \r\n", Aspect_Ratio_count[3]);
		fprintf(fp, "Aspect_Ratio(4-5): %d \r\n", Aspect_Ratio_count[4]);
		fprintf(fp, "Aspect_Ratio(5-6): %d \r\n", Aspect_Ratio_count[5]);

		// fprintf(fp,"triangle_jacobian_ratio_min: %f \r\n",JAC_min);
		// fprintf(fp, "triangle_jacobian_ratio_max: %f \r\n",JAC_max);
		// fprintf(fp, "triangle_jacobian_ratio_mean: %f \r\n",JAC_sum / nse);
		fprintf(fp, "triangle_skew_min: %f \r\n", TRIS_min);
		fprintf(fp, "triangle_skew_max: %f \r\n", TRIS_max);
		fprintf(fp, "triangle_skew_mean: %f \r\n", TRIS_sum / nse);
		fprintf(fp, "triangle_internal_angle_min: %f \r\n", MinIA);
		fprintf(fp, "triangle_internal_angle_max: %f \r(n", MaxIA);
		fprintf(fp, "\r\n");
		fprintf(fp, "tetrahedrons_length_width_ratio_min: %f \r\n", VLWR_min);
		fprintf(fp, "tetrahedrons_length_width_ratio_max: %f \r\n", VLWR_max);
		fprintf(fp, "tetrahedrons_length_width_ratio_mean: %f \r\n", VLWR_sum / ne);
		// fprintf(fp, "tetrahedrons_jacobian_ratio_min: %f \r\n",VJAC_min);
		// fprintf(fp, "tetrahedrons_jacobian_ratio_max: %f \r\n",VJAC_max) ;
		// fprintf(fp,"tetrahedrons_jacobian_ratio_mean: %f \r\n",VJAC_sum / ne);
		fprintf(fp, "tetrahedrons_skew_min: %f \r\n", VTRIS_min);
		fprintf(fp, "tetrahedrons_skew_max: %f \r\n", VTRIS_max);
		fprintf(fp, "tetrahedrons_skew_mean: %f \r\n", VTRIS_sum / ne);
		fprintf(fp, "tetrahedrons_internal_angle_min: %f \r\n", VMinIA);
		fprintf(fp, "tetrahedrons_internal_angle_max: %f \r\n", VMaxIA);
	}
	std::fclose(fp);
	return false;
}

double triangle_jacobian_ratio(const double points[][3])
{
	double j11 = points[1][0] - points[0][0];
	double j12 = points[1][1] - points[0][1];
	double j13 = points[1][2] - points[0][2];
	double j21 = points[2][0] - points[0][0];
	double j22 = points[2][1] - points[0][1];
	double j23 = points[2][2] - points[0][2];
	double determin = j11 * j22 + j12 * j23 + j13 * j21 - j13 * j22 - j12 * j21 - j11 * j23;
	if (determin <= 0)
		determin = -determin;
	return determin;
}

double tetrahedrons_jacobian_ratio(const double points[][3])
{
	double j11 = points[1][0] - points[0][0];
	double j12 = points[1][1] - points[0][1];
	double j13 = points[1][2] - points[0][2];
	double j21 = points[2][0] - points[0][0];
	double j22 = points[2][1] - points[0][1];
	double j23 = points[2][2] - points[0][2];
	double j31 = points[3][0] - points[0][0];
	double j32 = points[3][1] - points[0][1];
	double j33 = points[3][2] - points[0][2];
	double determin = j11 * j22 * j33 + j12 * j23 * j31 + j13 * j21 * j32 - j13 * j22 * j31 - j12 * j21 * j33 - j11 * j23 * j32;
	if (determin <= 0)
		determin = -determin;
	return determin;
}

double getLenght(const double a1[], const double a2[])
{
	return sqrt((a1[0] - a2[0]) * (a1[0] - a2[0]) + (a1[1] - a2[1]) * (a1[1] - a2[1]) + (a1[2] - a2[2]) * (a1[2] - a2[2]));
}

//计算三个点构成的长度宽度比列
double length_width_ratio(const double points[][3])
{
	double MIN = 0x3f3f3f, MAX = 0;
	for (int i = 0; i < 3; i++)
		for (int j = i + 1; j < 3; j++)
		{
			MIN = (std::min)(MIN, getLenght(points[i], points[j]));
			MAX = (std::max)(MAX, getLenght(points[i], points[j]));
		}

	return MAX / MIN;
}

double tetrahedrons_length_width_ratio(const double points[][3])
{
	double MIN = 0x3f3f3f, MAX = 0;
	for (int i = 0; i < 4; i++)
		for (int j = i + 1; j < 4; j++)
		{
			MIN = (std::min)(MIN, getLenght(points[i], points[j]));
			MAX = (std::max)(MAX, getLenght(points[i], points[j]));
		}
	return MAX / MIN;
}

double min_internal_angle(const double points[][3])
{
	double l1 = getLenght(points[0], points[1]);
	double l2 = getLenght(points[0], points[2]);
	double l3 = getLenght(points[1], points[2]);
	if (l1 > l2)
		std::swap(l1, l2);
	if (l2 > l3)
		std::swap(l2, l3);
	if (l1 > l2)
		std::swap(l1, l2);
	double cos_a = (l2 * l2 + l3 * l3 - l1 * l1) / (2 * l2 * l3);
	return acos(cos_a) * 180 / M_PI;
}

double max_internal_angle(const double points[][3])
{
	double l1 = getLenght(points[0], points[1]);
	double l2 = getLenght(points[0], points[2]);
	double l3 = getLenght(points[1], points[2]);
	if (l1 > l2)
		std::swap(l1, l2);
	if (l2 > l3)
		std::swap(l2, l3);
	if (l1 > l2)
		std::swap(l1, l2);
	double cos_c = (l2 * l2 + l1 * l1 - l3 * l3) / (2 * l2 * l1);
	return acos(cos_c) * 180 / M_PI;
}

double func(const double a1[], const double a2[], const double a3[])
{
	double mid_12[3] = {(a1[0] + a2[0]) / 2, (a1[1] + a2[1]) / 2, (a1[2] + a2[2]) / 2};
	double mid_23[3] = {(a2[0] + a3[0]) / 2, (a2[1] + a3[1]) / 2, (a2[2] + a3[2]) / 2};
	double mid_13[3] = {(a1[0] + a3[0]) / 2, (a1[1] + a3[1]) / 2, (a1[2] + a3[2]) / 2};

	double l_1_mid23 = getLenght(a1, mid_23);

	double l_1_mid13 = getLenght(a1, mid_13);
	double l_mid23_mid13 = getLenght(mid_23, mid_13);

	double A = acos((l_1_mid23 * l_1_mid23 + l_mid23_mid13 * l_mid23_mid13 - l_1_mid13 * l_1_mid13) / (2 * l_1_mid23 * l_mid23_mid13)) * 180 / M_PI;

	double l_1_mid12 = getLenght(a1, mid_12);
	double l_mid23_mid12 = getLenght(mid_12, mid_23);

	double B = acos((l_1_mid23 * l_1_mid23 + l_mid23_mid12 * l_mid23_mid12 - l_1_mid12 * l_1_mid12) / (2 * l_1_mid23 * l_mid23_mid12)) * 180 / M_PI;

	return (std::min)(A, B);
}
//计算三角形的偏斜度（skew）。偏斜度定义为三个内角中最小角度与90度的差值。
double triangle_skew(const double points[][3])
{
	return 90 - (std::min)({func(points[0], points[1], points[2]), func(points[1], points[0], points[2]), func(points[2], points[1], points[0])});
}

double tetrahedrons_skew(double points[][3])
{
	double MIN = 0x3f3f3f;
	for (int i = 0; i < 4; i++)
	{
		MIN = (std::min)(MIN, triangle_skew(points));
		if (i != 3)
		{
			for (int j = 0; j < 3; j++)
			{
				std::swap(points[3][j], points[i][j]);
			}
		}
	}
	return MIN;
}
