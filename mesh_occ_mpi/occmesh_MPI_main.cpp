#include <iostream>
#include <climits>
#include "mpi.h"
#include "TopTools_IndexedMapOfShape.hxx"
#include "TopoDS.hxx"
#include "TopoDS_Face.hxx"
#include "TopoDS_Shape.hxx"
#include "GProp_GProps.hxx"
#include "BRepGProp.hxx"
#include "3DNgmesher.h"
#include "scaling_profiler.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdlib>
#include <sstream>

using namespace std;

namespace nglib {
#include <nglib.h>
}
#include "createElmerOutput.h"

//打印帮助信息
void print_help() {
    cerr << "参数输入帮助" << endl <<
         "-o : 输出文件目录 默认为./output" << endl <<
         "-i : 输入文件目录 默认为/work/home/moussa/wholewall3.stp" << endl <<
         "-l --levels : 细化等级 默认为0" << endl <<
         "-r --refine : 细化次数 默认为0" << endl <<
         "-maxh : 网格最大值 默认为1000.0" << endl <<
         "-minh : 网格最小值 默认为10.0" << endl <<
         "-v : 保存细化文件" << endl <<
         "-adj : 通信" << endl <<
         "--profile : 开启强扩展指标采集" << endl <<
         "--profile-cache : 尝试采集硬件缓存访问/未命中计数" << endl <<
         "--profile-core-only : 跳过普通网格结果写出和质量评价，仅测试核心算法" << endl <<
         "--profile-dir <目录> : 分析结果根目录，默认 <输出目录>/strong_scaling_results" << endl <<
         "--profile-experiment <名称> : 实验名称" << endl <<
         "--profile-repeat <编号> : 重复实验编号" << endl <<
         "-h --help : 参数输入帮助" << endl;
}

int main(int argc, char **argv) {

    using namespace nglib;

    int id; //进程号
    int p = 1;  //进程总数
    MPI_Init(nullptr, nullptr);
    MPI_Comm_rank(MPI_COMM_WORLD, &id); //获取进程号
    MPI_Comm_size(MPI_COMM_WORLD, &p);  //获取进程总数

    // if(id == 0) cout << "MPI:" << p << endl;


    Ng_Init();

    Ng_Mesh *occ_mesh;

//Parameters
    string OUTPUT_PATH = "./output/";
    string INPUT_PATH = "/work/home/moussa/wholewall3.stp";
    bool save_vol = false;
    bool isComputeAdj = false;
    int numlevels = 0;
    int numrefine = 0;
    double maxh = 1000.0,minh = 10.0;
    bool profile_enabled = false;
    bool profile_cache = false;
    bool profile_core_only = false;
    string profile_dir;
    string profile_experiment = "strong_scaling";
    int profile_repeat = 1;
//

    if(argc <= 1) {
        print_help();
        MPI_Finalize();
        return 1;
    }

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i],"-o")) {
            if(argv[i+1] != NULL) OUTPUT_PATH = argv[i+1];
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"-i")) {
            if(argv[i+1] != NULL) INPUT_PATH = argv[i+1];
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"-l") || !strcmp(argv[i],"--levels")) {
            if(argv[i+1] != NULL) numlevels = atoi(argv[i+1]);
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"-r") || !strcmp(argv[i],"--refine")) {
            if(argv[i+1] != NULL) {
                numrefine = atoi(argv[i+1]);
            }
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"--maxh")) {
            if(argv[i+1] != NULL) maxh = atof(argv[i+1]);
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"--minh")) {
            if(argv[i+1] != NULL) minh = atof(argv[i+1]);
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"-h") || !strcmp(argv[i],"--help")) {
            print_help();
            MPI_Finalize();
            return 1;
        }
        else if(!strcmp(argv[i],"-v")) {
            save_vol = true;
        }
        else if(!strcmp(argv[i],"-adj")) {
            isComputeAdj = true;

        }
        else if(!strcmp(argv[i],"--profile")) {
            profile_enabled = true;
        }
        else if(!strcmp(argv[i],"--profile-cache")) {
            profile_enabled = true;
            profile_cache = true;
        }
        else if(!strcmp(argv[i],"--profile-core-only")) {
            profile_enabled = true;
            profile_core_only = true;
        }
        else if(!strcmp(argv[i],"--profile-dir")) {
            profile_enabled = true;
            if(i + 1 < argc) profile_dir = argv[++i];
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"--profile-experiment")) {
            profile_enabled = true;
            if(i + 1 < argc) profile_experiment = argv[++i];
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
        else if(!strcmp(argv[i],"--profile-repeat")) {
            profile_enabled = true;
            if(i + 1 < argc) profile_repeat = atoi(argv[++i]);
            else {
                print_help();
                MPI_Finalize();
                return 1;
            }
        }
    }

    if (profile_repeat < 1) profile_repeat = 1;
    if (profile_dir.empty()) {
        profile_dir = OUTPUT_PATH;
        if (!profile_dir.empty() && profile_dir.back() != '/') profile_dir += '/';
        profile_dir += "strong_scaling_results";
    }

    scaling::ProfileConfig profile_config;
    profile_config.enabled = profile_enabled;
    profile_config.collect_hardware_cache = profile_cache;
    profile_config.core_only = profile_core_only;
    profile_config.output_root = profile_dir;
    profile_config.experiment = profile_experiment;
    profile_config.repeat = profile_repeat;
    auto &profiler = scaling::Profiler::instance();
    profiler.configure(MPI_COMM_WORLD, profile_config);

    std::ostringstream command_line;
    for (int argument = 0; argument < argc; ++argument) {
        if (argument != 0) command_line << ' ';
        command_line << argv[argument];
    }
    profiler.add_metadata("command", command_line.str());
    profiler.add_metadata("input_path", INPUT_PATH);
    profiler.add_metadata("output_path", OUTPUT_PATH);
    profiler.add_metadata("numlevels", std::to_string(numlevels));
    profiler.add_metadata("numrefine", std::to_string(numrefine));
    profiler.add_metadata("maxh", std::to_string(maxh));
    profiler.add_metadata("minh", std::to_string(minh));
    profiler.add_metadata("adjacency_enabled", isComputeAdj ? "true" : "false");
    profiler.add_metadata("save_vol", save_vol ? "true" : "false");
    const char *omp_threads = std::getenv("OMP_NUM_THREADS");
    profiler.add_metadata("omp_num_threads", omp_threads ? omp_threads : "unset");

    string savepvname = OUTPUT_PATH + "test_occ/test_occ.vol";

    if(id == 0) {

        cerr << "========参数========" << endl <<
             "使用核数 : " << p << endl <<
             "输出文件目录 : " << OUTPUT_PATH << endl <<
             "输入文件目录 : " << INPUT_PATH << endl <<
             "细化等级 : " << numlevels << endl <<
             "细化次数 : " << numrefine << endl <<
             "网格最大值:" << maxh << endl <<
             "网格最小值:" << minh << endl;
        if (profile_enabled) {
            cerr << "强扩展分析 : 开启" << endl <<
                    "分析结果目录 : " << profile_dir << endl <<
                    "实验名称 : " << profile_experiment << endl <<
                    "重复编号 : " << profile_repeat << endl <<
                    "仅核心算法 : " << (profile_core_only ? "是" : "否") << endl <<
                    "硬件缓存计数 : " << (profile_cache ? "请求" : "关闭") << endl;
        }


        // int ret = mkdir(OUTPUT_PATH.c_str(), 0777);
        mkdir(OUTPUT_PATH.c_str(), 0777);

        string meshQuality_path = OUTPUT_PATH + string("meshQuality/");
        // int meshQuality_ret = mkdir(meshQuality_path.c_str(), 0777);
        mkdir(meshQuality_path.c_str(), 0777);

        string refinedSurfmesh_path = OUTPUT_PATH + string("refinedSurfmesh/");
        // int refinedSurfmesh_ret = mkdir(refinedSurfmesh_path.c_str(), 0777);
        mkdir(refinedSurfmesh_path.c_str(), 0777);

        string test_occ_path = OUTPUT_PATH + string("test_occ/");
        // int test_occ_ret = mkdir(test_occ_path.c_str(), 0777);
        mkdir(test_occ_path.c_str(), 0777);

        string testout_path = OUTPUT_PATH + string("testout/");
        // int testout_ret = mkdir(testout_path.c_str(), 0777);
        mkdir(testout_path.c_str(), 0777);

        string volfined_path = OUTPUT_PATH + string("volfined/");
        // int volfined_ret = mkdir(volfined_path.c_str(), 0777);
        mkdir(volfined_path.c_str(), 0777);

        string volwithadj_path = OUTPUT_PATH + string("volwithadj/");
        // int volwithadj_ret = mkdir(volwithadj_path.c_str(), 0777);
        mkdir(volwithadj_path.c_str(), 0777);
    }

    // Define pointer to OCC Geometry
    Ng_OCC_Geometry *occ_geom;

    // Ng_Mesh *occ_mesh;

    Ng_Meshing_Parameters mp;

    TopTools_IndexedMapOfShape FMap;

    // Ng_OCC_TopTools_IndexedMapOfShape *occ_fmap = (Ng_OCC_TopTools_IndexedMapOfShape*)&FMap;

    // Result of Netgen Operations
    Ng_Result ng_res;

    // Initialise the Netgen Core library
    // Ng_Init();

    // Read in the OCC File
    MPI_Barrier(MPI_COMM_WORLD);
    double startTime = MPI_Wtime();

    string STEP_PATH = INPUT_PATH;
    {
        scaling::StageScope profile_stage("geometry_load", "io");
        occ_geom = Ng_OCC_Load_STEP(STEP_PATH.c_str());
    }
    if (!occ_geom)
    {
        cout << "Error reading in STEP File: " << STEP_PATH << endl;
        MPI_Finalize();
        return 1;
    }
    if(id == 0)
        cout << "Successfully loaded STEP File: " << STEP_PATH << endl;


    occ_mesh = Ng_NewMesh();

    mp.uselocalh = 1;
    mp.elementsperedge = 2.0;
    mp.elementspercurve = 2.0;
    mp.maxh = maxh;
    mp.minh = minh;
    mp.grading = 0.3;
    mp.closeedgeenable = 0;
    mp.closeedgefact = 1.0;
    mp.optsurfmeshenable = 0;


    if(id == 0) {
        cout << "Setting Local Mesh size....." << endl;
        cout << "OCC Mesh Pointer before call = " << occ_mesh << endl;
    }
    {
        scaling::StageScope profile_stage("coarse_local_size", "setup");
        Ng_OCC_SetLocalMeshSize(occ_geom, occ_mesh, &mp);
    }
    if(id == 0) {
        cout << "Local Mesh size successfully set....." << endl;
        cout << "OCC Mesh Pointer after call = " << occ_mesh << endl;
        cout << "Creating Edge Mesh....." << endl;
    }

    {
        scaling::StageScope profile_stage("coarse_edge_mesh", "setup");
        ng_res = Ng_OCC_GenerateEdgeMesh(occ_geom, occ_mesh, &mp);
    }
    if (ng_res != NG_OK)
    {
        Ng_DeleteMesh(occ_mesh);
        cout << "Error creating Edge Mesh.... Aborting!!" << endl;
        MPI_Finalize();
        return 1;
    }
    else
    {
        if(id == 0) {
            cout << "Edge Mesh successfully created....." << endl;
            cout << "Number of points = " << Ng_GetNP(occ_mesh) << endl;
        }
    }

    id == 0 ? cout << "Creating Surface Mesh....." << endl: cout << "";

    {
        scaling::StageScope profile_stage("coarse_surface_mesh", "setup");
        ng_res = Ng_OCC_GenerateSurfaceMesh(occ_geom, occ_mesh, &mp);
    }
    if (ng_res != NG_OK)
    {
        Ng_DeleteMesh(occ_mesh);    //删除体网格
        cout << "Error creating Surface Mesh..... Aborting!!" << endl;
        MPI_Finalize();
        return 1;
    }
    else
    {
        if(id == 0) {
            cout << "Surface Mesh successfully created....." << endl;
            cout << "Number of points = " << Ng_GetNP(occ_mesh) << endl;
            cout << "Number of surface elements = " << Ng_GetNSE(occ_mesh) << endl;
        }
    }

    if(id == 0)
        cout << "Creating Volume Mesh....." << endl;

    {
        scaling::StageScope profile_stage("coarse_volume_mesh", "setup");
        ng_res = Ng_GenerateVolumeMesh(occ_mesh, &mp);
    }

    if(id == 0) {

        cout << "Volume Mesh successfully created....." << endl;
        cout << "Number of points = " << Ng_GetNP(occ_mesh) << endl;
        cout << "Number of volume elements = " << Ng_GetNE(occ_mesh) << endl;
    }


    // if(id == 0) cout << "Saving Mesh as VOL file....." << endl;
    int faceNum = nglib::Ng_GetNFD((nglib::Ng_Mesh*)occ_mesh);
    int x[4];
    for (int i = 1; i < faceNum; i++) {
        nglib::My_Ng_GetFaceDescriptor((nglib::Ng_Mesh*)occ_mesh, i, x);
        //std::cout <<"+++++++++++++++" << x[0] << "=" << x[1] << "=" << x[2] << "=" << x[3] << std::endl;

    }

    profiler.set_metric("coarse_points", Ng_GetNP(occ_mesh));
    profiler.set_metric("coarse_surface_elements", Ng_GetNSE(occ_mesh));
    profiler.set_metric("coarse_volume_elements", Ng_GetNE(occ_mesh));

    if(id == 0 && !profiler.core_only()) {
        scaling::StageScope profile_stage("coarse_mesh_save", "io");
        Ng_SaveMesh(occ_mesh, savepvname.c_str());
    }


    if(id == 0) cout << "Generate Coarse Mesh Done..." << endl;
    MPI_Barrier(MPI_COMM_WORLD);


    // double Coarse_endTime = clock();
    double Coarse_endTime = MPI_Wtime();
    double Coarse_Time = (double)(Coarse_endTime - startTime);



    if (p >= 1) {

        double time[5] = {0,0,0,0,0};
        double time_part1_detail[6] = {0,0,0,0,0,0};
        //Set the number of partitions
        int numParts = p;
        FILE *fp;
        FILE *fp_time;
        //set the level of refinement
        // int optvolmeshenable = 0;
        // int optsteps_3d = 0;
        // double gradingp = 0.3;
        // char paramname[20];

        //maxbarycoord is the n secondary of 2
        int maxbarycoord = 1 << (numlevels + numrefine+1);
        //int maxbarycoord = 1 << (numlevels + numrefine);
        map< int, xdMeshFaceInfo > facemap;
        map< int, int > g2lvrtxmap;
        map< Barycentric, int, CompBarycentric > baryc2locvrtxmap;
        map< int, list<int>> adjbarycs;
        list<xdFace> newfaces;
        list<VEindex> VEindexs;
        list<VEindex>::iterator VEi;
        std::map< IntPair, int, IntPairCompare > edgemap;

        std::string str_id = std::to_string(id);

        nglib::Ng_Mesh * submesh = nglib::Ng_NewMesh();
        {
            scaling::StageScope profile_stage("submesh_initialization", "setup");
            NewSubmesh(occ_mesh, submesh);
        }

        idx_t *edest = nullptr;
        {
            scaling::StageScope profile_stage("metis_partition", "compute");
            edest = PartitionMesh(occ_mesh, numParts);
        }

        //MPI_Barrier(MPI_COMM_WORLD);
        double currtime0 = MPI_Wtime();
        time[0] = double(currtime0 - Coarse_endTime);//NewSubmesh + PartitionMesh 等细化前准备/分区

        //遍历本进程负责的体单元，抽取四个面并记录面朝向/所属分区/域信息；然后 MPI_Allgatherv 全量汇总并合并成全局 facemap，区分内部面和分区交界面
        ExtractPartitionSurfaceMesh(occ_mesh, edest, facemap, time_part1_detail);
        //MPI_Barrier(MPI_COMM_WORLD);
        double currtime1 = MPI_Wtime();
        time[1] = double(currtime1 - currtime0);


        {
            scaling::StageScope profile_stage("part_face_create", "compute");
            PartFaceCreate(occ_mesh, id, facemap, maxbarycoord, submesh, g2lvrtxmap, baryc2locvrtxmap, newfaces);
        }
        // savepvname = OUTPUT_PATH + "PartFaceCreate/PartFaceCreate" + str_id + ".vol";
        // if(save_vol) {
        // 	Ng_SaveMesh(submesh, savepvname.c_str());
        // }

        //MPI_Barrier(MPI_COMM_WORLD);
        double currtime2 = MPI_Wtime();
        time[2] = double(currtime2 - currtime1);

        {
            scaling::StageScope profile_stage("surface_refine", "compute");
            Refine(submesh, numlevels, id, newfaces, baryc2locvrtxmap, edgemap);
        }
        //MPI_Barrier(MPI_COMM_WORLD);
        double currtime3 = MPI_Wtime();
        time[3] = double(currtime3 - currtime2);


        savepvname = OUTPUT_PATH + "refinedSurfmesh/refinedSurfmesh" + str_id + ".vol";
        if(save_vol && !profiler.core_only()) {
            scaling::StageScope profile_stage("refined_surface_save", "io");
            Ng_SaveMesh(submesh, savepvname.c_str());
        }




        //The face mesh grid is refined in parallel to each partition.


        int i;

        nglib::Ng_Meshing_Parameters nmp;
        //nmp.maxh = 1e6;
        nmp.fineness = 1;

        double volumeMesh_start = MPI_Wtime();
        {
            scaling::StageScope profile_stage("local_volume_mesh", "compute");
            nglib::Ng_GenerateVolumeMesh(submesh, &nmp);
        }
        double volumeMesh_end = MPI_Wtime();
        //MPI_Barrier(MPI_COMM_WORLD);
        if(id == 0) printf("meshing done \n");
        double currtime4 = MPI_Wtime();
        time[4] = double(currtime4 - currtime3);

        {
            scaling::StageScope profile_stage("volume_refine", "compute");
            for (i = 0; i < numrefine; i++) {
                Refineforvol(submesh, id, newfaces, baryc2locvrtxmap, edgemap);
            }
        }

        std::string savepvname = OUTPUT_PATH + "volfined/volfined" + str_id + ".vol";

        if(save_vol && !profiler.core_only()) {
            scaling::StageScope profile_stage("refined_volume_save", "io");
            nglib::Ng_SaveMesh(submesh, savepvname.c_str());
        }

        profiler.set_metric("local_points_before_adjacency", nglib::Ng_GetNP(submesh));
        profiler.set_metric("local_surface_elements_before_adjacency", nglib::Ng_GetNSE(submesh));
        profiler.set_metric("local_volume_elements_before_adjacency", nglib::Ng_GetNE(submesh));


        if (isComputeAdj) {
            map<Barycvrtx, list<int>, CompBarycvrtx> barycvrtx2adjprocsmap;
            {
                scaling::StageScope profile_stage("adjacency_build", "compute");
                computeadj(id,facemap,g2lvrtxmap, barycvrtx2adjprocsmap);
            }

            int *VEgid;
            int numNEs = nglib::Ng_GetNE(submesh);
            MYCALLOC(VEgid, int *, (numNEs + 1), sizeof(int));

            printf("start com_barycoords, id: %d\n", id);
            // cout << id << "start com_barycoords" << endl;

            int *newid = com_barycoords(submesh, MPI_COMM_WORLD, barycvrtx2adjprocsmap,
                                        baryc2locvrtxmap, adjbarycs, numParts, VEgid, id);

            
            // int pointdebug = nglib::Ng_GetNP((nglib::Ng_Mesh *)submesh);
            // char *debugpath = new char[512];
            // sprintf(debugpath,"pointdebugpath%d.txt",id);
            // std::string pointdebugpath = OUTPUT_PATH + debugpath;
            // ofstream outpointdebugpath1(pointdebugpath.c_str());
            // for(int i=1;i<=pointdebug;i++){
            //     outpointdebugpath1 << newid[i] << endl;
            // }
            // outpointdebugpath1.close();


            //newid 本地点id--》全局点id
            //VEgid 本地体网格id --》全局体网格id
            //adjbarycs 共享点再哪些处理器上

            // try{
            //     createElmerOutput(submesh,VEgid,newid,adjbarycs,numParts,id,OUTPUT_PATH);
            // }catch(...){
            //     printf("createElmerOutput error id is %d\n", id);
            // }
#if 1
            if (!profiler.core_only()) {
                scaling::StageScope profile_stage("partition_result_io", "io");
                nglib::Ng_Mesh* mesh = (nglib::Ng_Mesh *)submesh;
                char * boundaryfile1 = new char[512];
                char * elementfile1 = new char[512];
                char * headerfile1 = new char[512];
                char * nodefile1 = new char[512];
                char * sharedfile1 = new char[512];
                char * path1 = new char[512];

            sprintf(path1,"partitioning.%d",numParts);
            sprintf(boundaryfile1, "partitioning.%d/part.%d.boundary", numParts, id+1);
            sprintf(elementfile1, "partitioning.%d/part.%d.elements", numParts, id+1);
            sprintf(headerfile1, "partitioning.%d/part.%d.header", numParts, id+1);
            sprintf(nodefile1, "partitioning.%d/part.%d.nodes", numParts, id+1);
            sprintf(sharedfile1, "partitioning.%d/part.%d.shared", numParts, id+1);

            string path = OUTPUT_PATH + string(path1);
            string boundaryfile = OUTPUT_PATH + string(boundaryfile1);
            string elementfile = OUTPUT_PATH + string(elementfile1);
            string headerfile = OUTPUT_PATH + string(headerfile1);
            string nodefile = OUTPUT_PATH + string(nodefile1);
            string sharedfile = OUTPUT_PATH + string(sharedfile1);

            mkdir(path.c_str(),0777);

            int ne = nglib::Ng_GetNE(mesh); //体网格的数量
            int nse = nglib::Ng_GetNSE(mesh); //面网格的数量
            int np = nglib::Ng_GetNP(mesh); //点的数量

            //输出elements文件
            ofstream outelements(elementfile.c_str());
            int tet[4];
            for(int i=0;i < ne;i++){
                nglib::Ng_GetVolumeElement (mesh, i+1, tet);
                outelements << VEgid[i+1] << " 1 504 " << newid[tet[0]] << " " << newid[tet[1]] << " " << newid[tet[2]] << " " << newid[tet[3]] << endl;
            }
            outelements.close();

            //输出nodes文件
            ofstream outnodes(nodefile.c_str());
            double point[3];
            for(int i=0; i<np;i++){
                nglib::Ng_GetPoint (mesh, i+1, point);
                outnodes << newid[i+1] << " -1 " << point[0] << " " << point[1] << " " << point[2] << endl;
            }
            outnodes.close();

            //输出shared文件
            ofstream outshareds(sharedfile.c_str());
            for(auto it = adjbarycs.begin();it != adjbarycs.end();it++){
                int locid = it->first;
                std::list<int> proceid = it->second;
                int sizeid = proceid.size() + 1;
                std::string sharednode;
                sharednode += std::to_string(sizeid);
                sharednode += " ";
                sharednode += std::to_string(id+1); //elmerID = 核心ID + 1
                sharednode += " ";
                int m = 0;
                for(auto itt = proceid.begin(); itt != proceid.end() ; itt++){
                    sharednode += std::to_string(((*itt)+1));     //elmerID = 核心ID + 1
                    if( m != (sizeid-1) ){
                        sharednode += " ";
                    }
                    m++;
                }
                outshareds << newid[locid] << " " << sharednode << endl;
            }
            outshareds.close();


            //输出boundary文件
            ofstream outboundarys(boundaryfile.c_str());
            //求边界面网格所在的体网格
            Index3 i3;
            int l;
            bool (*fn_pt)(Index3,Index3) = fncomp;
            std::multimap<Index3,int, bool(*)(Index3, Index3)> face2vol(fn_pt);
            std::multimap<Index3,int, bool(*)(Index3, Index3)>::iterator myit;
            for(int i=1; i<=ne;i++){
                nglib::Ng_GetVolumeElement (mesh, i, tet);
                for (int j = 1; j <= 4; j++){
                    l = 0;
                    for (int k = 1; k <= 4; k++)
                    {
                        if (k != j)
                        {
                            i3.x[l] = newid[tet[k-1]];
                            l++;
                        }
                    }
                    i3.Sort();
                    face2vol.insert(pair<Index3,int>(i3,VEgid[i]));
                }
            }

            int *surfpointss = new int[3];
            int surfidx;
            int geoid;
            int number = 0;
            //int nfd = ((Mesh*)mesh)->GetNFD();
            for(int j=0; j< nse; j++){
                nglib::Ng_GetSurfaceElement(mesh, j + 1, surfpointss, surfidx);
                // if((Mesh*)mesh->GetFaceDesriptor(mesh->SurfaceElement(j).GetIndex()).BCProperty()==nfd){
                //     continue;
                // }
                geoid = nglib::GetBoundaryID(mesh,j+1) +1;
                if(nglib::ispatbound(mesh,j+1)){
                    continue;
                }
                i3.x[0] = newid[surfpointss[0]];
                i3.x[1] = newid[surfpointss[1]];
                i3.x[2] = newid[surfpointss[2]];
                i3.Sort();
                myit = face2vol.find(i3);
                if(myit!= face2vol.end()){
                    number++;
                    outboundarys << number << " " << geoid << " " << myit->second << " " << "0" << " 303 " << newid[surfpointss[0]]  << " " << newid[surfpointss[1]] << " " << newid[surfpointss[2]] <<endl;
                }
            }
            outboundarys.close();

            //输出header文件
            ofstream outheader(headerfile.c_str());
            outheader << np << " " << ne << " " << number << endl;
            outheader << 2 << endl;
            outheader << "504 " << ne << endl;
            outheader << "303 " << number << endl;
            if(adjbarycs.size() != 0)
            {
                outheader << adjbarycs.size() << " 0" << endl;
            }
            outheader.close();

            }
#endif

            printf("start com_baryVolumeElements, id: %d\n", id);
            com_baryVolumeElements(submesh, MPI_COMM_WORLD, barycvrtx2adjprocsmap,
                                   baryc2locvrtxmap, adjbarycs, newid, VEgid, VEindexs, numParts, id);
            printf("createElmerOutput, id: %d\n", id);

            profiler.set_metric("local_points_after_adjacency", nglib::Ng_GetNP(submesh));
            profiler.set_metric("local_surface_elements_after_adjacency", nglib::Ng_GetNSE(submesh));
            profiler.set_metric("local_volume_elements_after_adjacency", nglib::Ng_GetNE(submesh));
            

            savepvname = OUTPUT_PATH + "volwithadj/volwithadj" + str_id + ".vol";
            if(save_vol && !profiler.core_only()) {
                scaling::StageScope profile_stage("final_mesh_save", "io");
                nglib::Ng_SaveMesh(submesh, savepvname.c_str());

                // string openfoampath = OUTPUT_PATH + "openfoam/part" + str_id;
                // mkdir(openfoampath.c_str(), 0777);
                // const std::filesystem::path  &outfile = openfoampath;
                // nglib::My_WriteOpenFOAMFormat(submesh,outfile);
            }


        }

        if (!isComputeAdj) {
            profiler.set_metric("local_points_after_adjacency", nglib::Ng_GetNP(submesh));
            profiler.set_metric("local_surface_elements_after_adjacency", nglib::Ng_GetNSE(submesh));
            profiler.set_metric("local_volume_elements_after_adjacency", nglib::Ng_GetNE(submesh));
        }

        /*double endTime = MPI_Wtime();
        double Fine_Time = (double)(endTime - Coarse_endTime);
        double runtime = (double)(endTime - startTime);

        savepvname = OUTPUT_PATH + "volwithadj/volwithadj" + str_id + ".vol";
        if(save_vol) {

        nglib::Ng_SaveMesh(submesh, savepvname.c_str());
        }
        if(id == 0) {
            string savepvname_time = OUTPUT_PATH + "testout/testout_time" + str_id + ".txt";
            fp_time = fopen(savepvname_time.c_str(), "w");
            if (fp_time == NULL) {
                cout << "File " << savepvname << "canot open" << endl;
            }
            else {
            fprintf(fp_time, "Coarse_Time for id:%d is %.2f s\r\n", id, Coarse_Time);
            fprintf(fp_time, "Fine_Time for id:%d is %.2f s\r\n", id, Fine_Time);
            fprintf(fp_time, "runtime for id:%d is %.2f s\r\n", id, runtime);
            for(int i = 0; i < 5; i++) {
                fprintf(fp_time, "part %d time : %.2f \n", i, time[i]);
            }
            }
            fclose(fp_time);
        }

        savepvname = OUTPUT_PATH + "testout/testout_mesh.txt";
        fp = fopen(savepvname.c_str(), "a");
        if (fp == NULL) {
            cout << "File " << savepvname << "canot open" << endl;
        }
        else {
            //fprintf(fp, "the num of points for id:%d is %d\r\n", id, nglib::Ng_GetNP(submesh));
            //fprintf(fp, "the num of Surelemments for id:%d is %d\r\n", id, nglib::Ng_GetNSE(submesh));
            fprintf(fp, "the num of Volelements for id:%d is %d\r\n", id, nglib::Ng_GetNE(submesh));
            fprintf(fp, "the volmesh generate time for if: %d is %f\r\n", id, volumeMesh_end-volumeMesh_start);
        }
        if(id == 0) {
            int Volelements_Sum = nglib::Ng_GetNE(submesh);
            for(int i = 1; i < p; i++) {
                int Volelements_Buf = 0;
                MPI_Recv(&Volelements_Buf, sizeof(Volelements_Buf), MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                Volelements_Sum += Volelements_Buf;
            }
            fprintf(fp, "the Sum of Volelements id %d\r\n", Volelements_Sum);

        }
        else {
            int Volelements_Buf = nglib::Ng_GetNE(submesh);
            MPI_Send(&Volelements_Buf, sizeof(Volelements_Buf), MPI_INT, 0, 0, MPI_COMM_WORLD);
        }


        fclose(fp);		/*
        int *VEgids_list, *VEgid_isin_list;
        MYCALLOC(VEgids_list, int *, (VEindexs.size() + 1), sizeof(int));
        MYCALLOC(VEgid_isin_list, int *, (VEindexs.size() + 1), sizeof(int));
        i = 1;
        for (VEi = VEindexs.begin(); VEi != VEindexs.end(); ++VEi) {
            VEgids_list[i] = (*VEi).gid;
            VEgid_isin_list[i] = (*VEi).Isin;
            i++;
        }
        */
        //}
        //else {
        double endTime = MPI_Wtime();
        double Fine_Time = (double)(endTime - Coarse_endTime);
        double runtime = (double)(endTime - startTime);
        savepvname = OUTPUT_PATH + "testout/testout_mesh.txt";
        if (!profiler.core_only()) {
            if(id == 0) {
                scaling::StageScope profile_stage("testout_io", "io");
                string savepvname_time = OUTPUT_PATH + "testout/testout_time" + str_id + ".txt";
                fp_time = fopen(savepvname_time.c_str(), "w");
                if (fp_time == NULL) {
                    cout << "File " << savepvname << "canot open" << endl;
                }
                else {
                    fprintf(fp_time, "Coarse_Time for id:%d is %.2f s\r\n", id, Coarse_Time);
                    fprintf(fp_time, "Fine_Time for id:%d is %.2f s\r\n", id, Fine_Time);
                    fprintf(fp_time, "runtime for id:%d is %.2f s\r\n", id, runtime);
                    for(int i = 0; i < 5; i++) {
                        fprintf(fp_time, "part %d time : %.2f \n", i, time[i]);
                    }
                    for(int i = 0; i < 6; i++) {
                        fprintf(fp_time, "part 1 detail %d time : %.2f \n", i, time_part1_detail[i]);
                    }
                    fclose(fp_time);
                }
            }

            {
                scaling::StageScope profile_stage("testout_io", "io");
                fp = fopen(savepvname.c_str(), "a");
                if (fp == NULL) {
                    cout << "File " << savepvname << "canot open" << endl;
                }
                else {
                    fprintf(fp, "the num of Volelements for id:%d is %d\r\n", id, nglib::Ng_GetNE(submesh));
                    fprintf(fp, "the volmesh generate time for if: %d is %f\r\n", id, volumeMesh_end-volumeMesh_start);
                    fflush(fp);
                }
            }

            int Volelements_Sum = 0;
            {
                scaling::StageScope profile_stage("final_count_exchange", "communication");
                if(id == 0) {
                    Volelements_Sum = nglib::Ng_GetNE(submesh);
                    for(int i = 1; i < p; i++) {
                        int Volelements_Buf = 0;
                        MPI_Recv(&Volelements_Buf, 1, MPI_INT, i, 0,
                                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                        Volelements_Sum += Volelements_Buf;
                    }
                }
                else {
                    int Volelements_Buf = nglib::Ng_GetNE(submesh);
                    MPI_Send(&Volelements_Buf, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
                }
            }
            profiler.add_communication(
                "final_count_exchange",
                id == 0 ? 0 : 1,
                id == 0 ? static_cast<std::uint64_t>(p - 1) : 0,
                id == 0 ? 0 : sizeof(int),
                id == 0 ? static_cast<std::uint64_t>(p - 1) * sizeof(int) : 0);

            {
                scaling::StageScope profile_stage("testout_io", "io");
                if (id == 0 && fp != NULL) {
                    fprintf(fp, "the Sum of Volelements id %d\r\n", Volelements_Sum);
                }
                if (fp != NULL) fclose(fp);
            }
        }
        //}
        if (!profiler.core_only()) {
            scaling::StageScope profile_stage("quality_evaluation", "postprocess");
            meshQualityEvaluation(submesh, id, OUTPUT_PATH);
        }


    }

    if(id == 0) cout << "successful!!!" << endl;
    profiler.set_total_elapsed(MPI_Wtime() - startTime);
    profiler.finalize();
    MPI_Finalize();

    return 0;
}
