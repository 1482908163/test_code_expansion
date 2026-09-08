#include <iostream>
#include <climits>
#include <cmath>
#include "mpi.h"
#include "TopTools_IndexedMapOfShape.hxx"
#include "TopoDS.hxx"
#include "TopoDS_Face.hxx"
#include "TopoDS_Shape.hxx"
#include "GProp_GProps.hxx"
#include "BRepGProp.hxx"
#include "3DNgmesher.h"
#include "scaling_profiler.h"
#include "mpi_debug.h"
#include "mesh_mpi_types.h"
#include "research_mesh.h"
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
         "--algorithm <baseline|balance|sparse|combined> : 原算法/均衡/稀疏/组合" << endl <<
         "--balance-sweeps <整数> : 分区修正轮数，默认4" << endl <<
         "--cut-growth <比例> : 允许新增切分面比例，默认0.05" << endl <<
         "--cost-weights <a,b,c,d> : 四阶段代价权重，默认1,1,1,1" << endl <<
         "--phase-model <文件> : 冻结的分阶段耗时模型，由统一脚本提供" << endl <<
         "--resource-model / --rank-capacities : 几何与当前节点能力模型，由统一入口提供" << endl <<
         "--min-gain-seconds / --min-gain-fraction : 预计最大耗时下降门槛" << endl <<
         "--closure-growth <比例> : 最终依赖闭包增长预算，默认0" << endl <<
         "--partition-seed <整数> : METIS 分区种子，-1保留库默认" << endl <<
         "--partition-variant <metis_seed|cell_order_v1> : 库种子/可复现粗单元重排" << endl <<
         "--rank-shift <整数> : 逻辑分区到物理进程的循环映射，由统一脚本提供" << endl <<
         "--partition-reference <文件> : 核对预检粗单元归属，由统一脚本提供" << endl <<
         "--preflight-parts / --preflight-seeds / --preflight-dir : 仅分区预检，由统一脚本提供" << endl <<
         "--search-seconds <秒> : v3 候选搜索时间预算，默认0.35" << endl <<
         "--verify-faces : 与全收集结果核对；仅用于小规模正确性运行" << endl <<
         "--profile-natural : 采集自然运行时间，关闭诊断用前置屏障" << endl <<
         "--profile : 开启强扩展指标采集" << endl <<
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
    bool profile_split = true;
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
        else if(!strcmp(argv[i],"--profile-natural")) {
            profile_enabled = true; profile_split = false;
        }
        else if(!strcmp(argv[i],"--verify-faces")) {
            mesh_research::options().verify_faces = true;
        }
        else if(!strcmp(argv[i],"--algorithm") || !strcmp(argv[i],"--balance-sweeps") ||
                !strcmp(argv[i],"--cut-growth") || !strcmp(argv[i],"--cost-weights") ||
                !strcmp(argv[i],"--resource-model") || !strcmp(argv[i],"--rank-capacities") ||
                !strcmp(argv[i],"--phase-model") || !strcmp(argv[i],"--min-gain-seconds") ||
                !strcmp(argv[i],"--min-gain-fraction") || !strcmp(argv[i],"--closure-growth") ||
                !strcmp(argv[i],"--partition-seed") || !strcmp(argv[i],"--search-seconds") ||
                !strcmp(argv[i],"--partition-variant") || !strcmp(argv[i],"--rank-shift") ||
                !strcmp(argv[i],"--partition-reference") || !strcmp(argv[i],"--preflight-parts") ||
                !strcmp(argv[i],"--preflight-seeds") || !strcmp(argv[i],"--preflight-dir")) {
            const std::string option=argv[i];
            if(i+1>=argc) { if(id==0) print_help(); MPI_Abort(MPI_COMM_WORLD,2); }
            const std::string value=argv[++i];
            try {
                auto &research=mesh_research::options();
                std::size_t consumed=0;
                if(option=="--algorithm") research.algorithm=value;
                else if(option=="--phase-model") research.model_path=value;
                else if(option=="--resource-model") research.resource_path=value;
                else if(option=="--rank-capacities") research.capacity_path=value;
                else if(option=="--partition-variant") research.partition_variant=value;
                else if(option=="--partition-reference") research.reference_path=value;
                else if(option=="--preflight-dir") research.preflight_dir=value;
                else if(option=="--rank-shift" || option=="--preflight-parts") {
                    const int n=std::stoi(value,&consumed);
                    if(consumed!=value.size() || n<0) throw std::runtime_error("invalid placement/preflight count");
                    if(option=="--rank-shift") research.rank_shift=n;
                    else research.preflight_parts=n;
                }
                else if(option=="--preflight-seeds") {
                    std::string list=value;std::replace(list.begin(),list.end(),',',' ');
                    std::istringstream in(list);std::string field;std::set<int> seen;
                    while(in>>field) {
                        const int seed=std::stoi(field,&consumed);
                        if(consumed!=field.size() || seed< -1 || !seen.insert(seed).second)
                            throw std::runtime_error("invalid preflight seeds");
                        research.preflight_seeds.push_back(seed);
                    }
                }
                else if(option=="--partition-seed") {
                    research.partition_seed=std::stoi(value,&consumed);
                    if(consumed!=value.size() || research.partition_seed< -1)
                        throw std::runtime_error("invalid partition seed");
                }
                else if(option=="--min-gain-seconds" || option=="--min-gain-fraction" || option=="--closure-growth" || option=="--search-seconds") {
                    const double v=std::stod(value,&consumed);
                    if(consumed!=value.size() || !std::isfinite(v) || v<0)
                        throw std::runtime_error("invalid selection budget");
                    if(option=="--min-gain-seconds") research.cost.min_gain_seconds=v;
                    else if(option=="--min-gain-fraction") research.cost.min_gain_fraction=v;
                    else if(option=="--search-seconds") research.cost.search_seconds=v;
                    else research.cost.closure_growth=v;
                }
                else if(option=="--balance-sweeps") {
                    research.cost.sweeps=std::stoi(value,&consumed);
                    if(consumed!=value.size()) throw std::runtime_error("invalid sweeps");
                } else if(option=="--cut-growth") {
                    research.cost.cut_growth=std::stod(value,&consumed);
                    if(consumed!=value.size()) throw std::runtime_error("invalid cut growth");
                } else {
                    std::istringstream in(value);std::string field;
                    for(int k=0;k<4;++k) {
                        if(!std::getline(in,field,','))throw std::runtime_error("need four cost weights");
                        research.cost.weights[k]=std::stod(field,&consumed);
                        if(consumed!=field.size())throw std::runtime_error("invalid cost weight");
                    }
                    if(std::getline(in,field,','))throw std::runtime_error("too many cost weights");
                }
            } catch(const std::exception &e) {
                if(id==0) std::cerr<<e.what()<<std::endl;
                MPI_Abort(MPI_COMM_WORLD,2);
            }
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

    auto &research = mesh_research::options();
    research.cost.levels=numlevels;research.cost.refines=numrefine;
    // Only rank zero evaluates partitions. Load the immutable small model once,
    // before coarse meshing; do not issue thousands of shared-filesystem reads.
    if(id==0 && !research.model_path.empty()) {
        try {
            research.cost.model.read(research.model_path);
            if(research.cost.model.levels!=numlevels || research.cost.model.refines!=numrefine ||
               research.cost.model.held_out_ranks!=p) throw std::runtime_error("phase model configuration mismatch");
        }
        catch(const std::exception &e) {std::cerr<<e.what()<<std::endl;MPI_Abort(MPI_COMM_WORLD,2);}
    }
    if(!research.resource_path.empty()) {
        std::vector<char> hosts;
        if(id==0)hosts.resize(static_cast<std::size_t>(p)*MPI_MAX_PROCESSOR_NAME);
        char host[MPI_MAX_PROCESSOR_NAME]={};int length=0;
        MPI_Get_processor_name(host,&length);
        MPI_Gather(host,MPI_MAX_PROCESSOR_NAME,MPI_CHAR,hosts.data(),MPI_MAX_PROCESSOR_NAME,MPI_CHAR,0,MPI_COMM_WORLD);
        if(id==0)try {
            research.resource.read(research.resource_path);
            if(!research.model_path.empty() || research.resource.ranks!=p ||
               research.resource.levels!=numlevels || research.resource.refines!=numrefine ||
               research.rank_shift%research.resource.rpn)
                throw std::runtime_error("resource model configuration mismatch");
            if(!research.capacity_path.empty()) {
                std::vector<std::string> names;
                for(int i=0;i<p;++i)names.emplace_back(hosts.data()+static_cast<std::size_t>(i)*MPI_MAX_PROCESSOR_NAME);
                research.resource.read_capacities(research.capacity_path,names);
            } else if(research.balance())throw std::runtime_error("node mapping requires current allocation capacities");
        } catch(const std::exception &e) {std::cerr<<e.what()<<std::endl;MPI_Abort(MPI_COMM_WORLD,2);}
    } else if(!research.capacity_path.empty())MPI_Abort(MPI_COMM_WORLD,2);
    if ((research.algorithm!="baseline" && research.algorithm!="balance" &&
         research.algorithm!="sparse" && research.algorithm!="combined") ||
        (research.verify_faces && (!research.sparse() || profile_enabled)) ||
        (research.partition_variant!="metis_seed" && research.partition_variant!="cell_order_v1") ||
        research.rank_shift>=p ||
        (research.preflight_parts>0 && (research.preflight_dir.empty() || research.preflight_seeds.empty())) ||
        numlevels<0 || numrefine<0 || numlevels+numrefine>13 ||
        research.cost.sweeps<0 || !std::isfinite(research.cost.cut_growth) ||
        research.cost.cut_growth<0) {
        if(id==0) std::cerr<<"Invalid algorithm/configuration; face verification requires sparse/combined and profiling OFF. levels+refines must be <=13 (short barycentric coordinates)."<<std::endl;
        MPI_Abort(MPI_COMM_WORLD,2);
    }
    if (profile_dir.empty()) {
        profile_dir = OUTPUT_PATH;
        if (!profile_dir.empty() && profile_dir.back() != '/') profile_dir += '/';
        profile_dir += "strong_scaling_results";
    }

    scaling::ProfileConfig profile_config;
    profile_config.enabled = profile_enabled;
    profile_config.split_collectives = profile_split;
    profile_config.core_only = profile_core_only;
    profile_config.output_root = profile_dir;
    profile_config.experiment = profile_experiment;
    profile_config.repeat = profile_repeat;
    auto &profiler = scaling::Profiler::instance();
    profiler.configure(MPI_COMM_WORLD, profile_config);
    profiler.add_metadata("global_id_bits", "64");
    profiler.add_metadata("local_index_bits", "32");

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
    profiler.add_metadata("profiler_schema_version", "research_1");
    profiler.add_metadata("feature_schema", "mesh_phase_v3");
    profiler.add_metadata("partition_seed",std::to_string(research.partition_seed));
    profiler.add_metadata("partition_variant",research.partition_variant);
    profiler.add_metadata("rank_shift",std::to_string(research.rank_shift));
    profiler.add_metadata("sampling_protocol",research.reference_path.empty()?"legacy":"partition_sampling_v1");
    profiler.add_metadata("algorithm",research.algorithm);
    profiler.add_metadata("timing_mode",profile_split?"split":"natural");
    profiler.add_metadata("timing_boundary","post_coarse_barrier_to_adjacency_complete");
    profiler.add_metadata("core_only",profile_core_only?"true":"false");
    profiler.add_metadata("partition_contract","one partition per MPI rank");
    profiler.add_metadata("cost_model",!research.resource_path.empty()?"phase_seconds_resource":
        (research.model_path.empty()?"geometric_proxy":"phase_seconds"));
    profiler.add_metadata("balance_method",research.resource_path.empty()?"boundary":"node_mapping");
    for(const char *key:{"MESH_INPUT_SHA256","MESH_BINARY_SHA256","MESH_SOURCE_REVISION","MESH_MODEL_SHA256","EXPERIMENT_STAGE",
                        "MESH_PARTITION_SIGNATURE","MESH_PREFLIGHT_SHA256","RANKS_PER_NODE","MESH_CAPACITY_SHA256"}) {
        const char *v=std::getenv(key);profiler.add_metadata(key,v?v:"unset");
    }
    profiler.add_metadata("cut_growth",std::to_string(research.cost.cut_growth));
    profiler.add_metadata("balance_sweeps",std::to_string(research.cost.sweeps));
    std::ostringstream weights;
    for(int k=0;k<4;++k)weights<<(k?",":"")<<research.cost.weights[k];
    profiler.add_metadata("cost_weights",weights.str());
    profiler.add_metadata(
        "collective_timing",
        profile_split?"pre_barrier_wait_then_execution":"natural_collective_includes_arrival_skew");
    profiler.add_metadata(
        "communication_fraction_definition",
        "off_rank_logical_payload; no transport_headers_or_collective_algorithm_bytes");
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
                    "算法 : " << mesh_research::options().algorithm << endl <<
                    "前置同步诊断 : " << (profile_split ? "开启" : "关闭") << endl;
        }


        // Core-only runs write profiles through the profiler, not mesh artifacts.
        if (!profiler.core_only()) {
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
        netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.coarse.begin");
        Ng_SaveMesh(occ_mesh, savepvname.c_str());
        netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.coarse.end");
    }


    // 参考归属在已有粗网格结束屏障之前读取；正式划分仍在核心区间重新执行。
    if(id==0 && !research.reference_path.empty()) {
        try {research.reference_labels=mesh_research::read_partition_reference(research.reference_path,
                p,nglib::Ng_GetNE(occ_mesh),research.partition_seed,research.partition_variant);}
        catch(const std::exception &e) {std::cerr<<e.what()<<std::endl;MPI_Abort(MPI_COMM_WORLD,2);}
    }
    if(id == 0) cout << "Generate Coarse Mesh Done..." << endl;
    MPI_Barrier(MPI_COMM_WORLD);

    if(research.preflight_parts>0) {
        RunPartitionPreflight(occ_mesh);
        MPI_Finalize();
        return 0;
    }


    // double Coarse_endTime = clock();
    double Coarse_endTime = MPI_Wtime();
    profiler.begin_core();
    double Coarse_Time = (double)(Coarse_endTime - startTime);



    if (p >= 1) {

        double time[5] = {0,0,0,0,0};
        double time_part1_detail[6] = {0,0,0,0,0,0};
        //Set the number of partitions
        int numParts = p;
        // Current algorithmic contract: one METIS partition is owned by one
        // MPI rank.  Record it explicitly so experiment reports do not confuse
        // Slurm nodes, MPI ranks, and mesh partitions.
        profiler.set_metric("partition_count", static_cast<double>(numParts));
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
            edest = PartitionResearchMesh(occ_mesh, numParts);
        }

        //MPI_Barrier(MPI_COMM_WORLD);
        double currtime0 = MPI_Wtime();
        time[0] = double(currtime0 - Coarse_endTime);//NewSubmesh + PartitionMesh 等细化前准备/分区

        //遍历本进程负责的体单元，抽取四个面并记录面朝向/所属分区/域信息；然后 MPI_Allgatherv 全量汇总并合并成全局 facemap，区分内部面和分区交界面
        {
            scaling::StageScope phase("face_pipeline_total", "algorithm");
            ExtractPartitionSurfaceMesh(occ_mesh, edest, facemap, time_part1_detail);
        }
        profiler.mark_elapsed("face_complete_elapsed");
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
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.refinedSurfmesh.begin");
            Ng_SaveMesh(submesh, savepvname.c_str());
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.refinedSurfmesh.end");
        }




        //The face mesh grid is refined in parallel to each partition.


        int i;

        nglib::Ng_Meshing_Parameters nmp;
        //nmp.maxh = 1e6;
        nmp.fineness = 1;

        double volumeMesh_start = MPI_Wtime();
        {
            scaling::StageScope profile_stage("local_volume_mesh", "compute");
            const auto local_status=nglib::Ng_GenerateVolumeMesh(submesh, &nmp);
            if(local_status!=nglib::NG_OK) {
                std::cerr<<"局部体网格生成失败，进程 "<<id<<std::endl;
                MPI_Abort(MPI_COMM_WORLD,3);
            }
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
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.volfined.begin");
            nglib::Ng_SaveMesh(submesh, savepvname.c_str());
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.volfined.end");
        }

        const int local_points_before_adjacency = nglib::Ng_GetNP(submesh);
        const int local_surface_elements_before_adjacency = nglib::Ng_GetNSE(submesh);
        const int local_volume_elements_before_adjacency = nglib::Ng_GetNE(submesh);
        profiler.set_metric("local_points_before_adjacency", local_points_before_adjacency);
        profiler.set_metric("local_surface_elements_before_adjacency", local_surface_elements_before_adjacency);
        profiler.set_metric("local_volume_elements_before_adjacency", local_volume_elements_before_adjacency);


        if (isComputeAdj) {
            map<Barycvrtx, list<int>, CompBarycvrtx> barycvrtx2adjprocsmap;
            {
                scaling::StageScope profile_stage("adjacency_build", "compute");
                computeadj(id,facemap,g2lvrtxmap, barycvrtx2adjprocsmap);
            }

            GlobalId *VEgid;
            int numNEs = nglib::Ng_GetNE(submesh);
            require_local_mesh_capacity(numNEs, MPI_COMM_WORLD);
            MYCALLOC(VEgid, GlobalId *, (static_cast<std::size_t>(numNEs) + 1), sizeof(GlobalId));

            if (netgen_mpi_trace_enabled()) {
                printf("start com_barycoords, id: %d\n", id);
                fflush(stdout);
            }
            // cout << id << "start com_barycoords" << endl;

            GlobalId *newid = com_barycoords(submesh, MPI_COMM_WORLD, barycvrtx2adjprocsmap,
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
                write_partition_element(outelements, VEgid[i+1], tet, newid);
            }
            outelements.close();

            //输出nodes文件
            ofstream outnodes(nodefile.c_str());
            double point[3];
            for(int i=0; i<np;i++){
                nglib::Ng_GetPoint (mesh, i+1, point);
                write_partition_node(outnodes, newid[i+1], point);
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
                write_partition_shared(outshareds, newid[locid], sharednode);
            }
            outshareds.close();


            //输出boundary文件
            ofstream outboundarys(boundaryfile.c_str());
            //求边界面网格所在的体网格
            Index3 i3;
            int l;
            bool (*fn_pt)(Index3,Index3) = fncomp;
            std::multimap<Index3,GlobalId, bool(*)(Index3, Index3)> face2vol(fn_pt);
            std::multimap<Index3,GlobalId, bool(*)(Index3, Index3)>::iterator myit;
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
                    face2vol.insert(pair<Index3,GlobalId>(i3,VEgid[i]));
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
                if(nglib::ispatbound(mesh,j)){
                    continue;
                }
                i3.x[0] = newid[surfpointss[0]];
                i3.x[1] = newid[surfpointss[1]];
                i3.x[2] = newid[surfpointss[2]];
                i3.Sort();
                myit = face2vol.find(i3);
                if(myit!= face2vol.end()){
                    number++;
                    write_partition_boundary(outboundarys, number, geoid, myit->second, surfpointss, newid);
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

            if (netgen_mpi_trace_enabled()) {
                printf("start com_baryVolumeElements, id: %d\n", id);
                fflush(stdout);
            }
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "com_baryVolumeElements.begin");
            newid = com_baryVolumeElements(
                submesh, MPI_COMM_WORLD, barycvrtx2adjprocsmap,
                baryc2locvrtxmap, adjbarycs, newid, VEgid,
                VEindexs, numParts, id);
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "com_baryVolumeElements.end");
            if (netgen_mpi_trace_enabled()) {
                printf("createElmerOutput, id: %d\n", id);
                fflush(stdout);
            }

            profiler.set_total_elapsed(MPI_Wtime()-Coarse_endTime);
            const int local_points_after_adjacency = nglib::Ng_GetNP(submesh);
            const int local_surface_elements_after_adjacency = nglib::Ng_GetNSE(submesh);
            const int local_volume_elements_after_adjacency = nglib::Ng_GetNE(submesh);
            profiler.set_metric("local_points_after_adjacency", local_points_after_adjacency);
            profiler.set_metric("local_surface_elements_after_adjacency", local_surface_elements_after_adjacency);
            profiler.set_metric("local_volume_elements_after_adjacency", local_volume_elements_after_adjacency);
            profiler.set_metric(
                "ghost_points_added",
                local_points_after_adjacency >= local_points_before_adjacency
                    ? local_points_after_adjacency - local_points_before_adjacency
                    : 0);
            profiler.set_metric(
                "ghost_volume_elements_added",
                local_volume_elements_after_adjacency >= local_volume_elements_before_adjacency
                    ? local_volume_elements_after_adjacency - local_volume_elements_before_adjacency
                    : 0);


            savepvname = OUTPUT_PATH + "volwithadj/volwithadj" + str_id + ".vol";
            if(save_vol && !profiler.core_only()) {
                scaling::StageScope profile_stage("final_mesh_save", "io");
                netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.volwithadj.begin");
                nglib::Ng_SaveMesh(submesh, savepvname.c_str());
                netgen_mpi_checkpoint(MPI_COMM_WORLD, "Ng_SaveMesh.volwithadj.end");

                // string openfoampath = OUTPUT_PATH + "openfoam/part" + str_id;
                // mkdir(openfoampath.c_str(), 0777);
                // const std::filesystem::path  &outfile = openfoampath;
                // nglib::My_WriteOpenFOAMFormat(submesh,outfile);
            }

            free(newid);
            free(VEgid);


        }

        if (!isComputeAdj) {
            profiler.set_total_elapsed(MPI_Wtime()-Coarse_endTime);
            profiler.set_metric("local_points_after_adjacency", nglib::Ng_GetNP(submesh));
            profiler.set_metric("local_surface_elements_after_adjacency", nglib::Ng_GetNSE(submesh));
            profiler.set_metric("local_volume_elements_after_adjacency", nglib::Ng_GetNE(submesh));
            profiler.set_metric("ghost_points_added", 0.0);
            profiler.set_metric("ghost_volume_elements_added", 0.0);
        }

        std::free(edest);
        double endTime = MPI_Wtime();
        double Fine_Time = (double)(endTime - Coarse_endTime);
        double runtime = (double)(endTime - startTime);
        savepvname = OUTPUT_PATH + "testout/testout_mesh.txt";
        if (!profiler.core_only()) {
            if (id == 0) {
                scaling::StageScope profile_stage("testout_io", "io");
                string savepvname_time = OUTPUT_PATH + "testout/testout_time" + str_id + ".txt";
                fp_time = fopen(savepvname_time.c_str(), "w");
                if (fp_time == NULL) {
                    fprintf(stderr, "[IO_ERROR] rank=0 cannot open %s\n", savepvname_time.c_str());
                    fflush(stderr);
                    MPI_Abort(MPI_COMM_WORLD, MPI_ERR_OTHER);
                    abort();
                }
                fprintf(fp_time, "Coarse_Time for id:%d is %.2f s\r\n", id, Coarse_Time);
                fprintf(fp_time, "Fine_Time for id:%d is %.2f s\r\n", id, Fine_Time);
                fprintf(fp_time, "runtime for id:%d is %.2f s\r\n", id, runtime);
                for (int i = 0; i < 5; ++i)
                    fprintf(fp_time, "part %d time : %.2f \n", i, time[i]);
                for (int i = 0; i < 6; ++i)
                    fprintf(fp_time, "part 1 detail %d time : %.2f \n", i, time_part1_detail[i]);
                fclose(fp_time);
            }

            const long long local_volume_elements = nglib::Ng_GetNE(submesh);
            const double local_volume_mesh_time = volumeMesh_end - volumeMesh_start;
            long long total_volume_elements = 0;
            double sum_volume_mesh_time = 0.0;
            double max_volume_mesh_time = 0.0;

            netgen_mpi_checkpoint(MPI_COMM_WORLD, "testout.reduce.begin");
            {
                scaling::StageScope profile_stage("final_count_exchange", "communication");
                netgen_mpi_check(
                    MPI_COMM_WORLD,
                    MPI_Reduce(&local_volume_elements, &total_volume_elements, 1,
                               MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD),
                    "testout/MPI_Reduce(volume_elements)");
                netgen_mpi_check(
                    MPI_COMM_WORLD,
                    MPI_Reduce(&local_volume_mesh_time, &sum_volume_mesh_time, 1,
                               MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD),
                    "testout/MPI_Reduce(volume_mesh_time_sum)");
                netgen_mpi_check(
                    MPI_COMM_WORLD,
                    MPI_Reduce(&local_volume_mesh_time, &max_volume_mesh_time, 1,
                               MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD),
                    "testout/MPI_Reduce(volume_mesh_time_max)");
            }
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "testout.reduce.end");

            const std::uint64_t reduce_bytes = sizeof(long long) + 2 * sizeof(double);
            profiler.add_communication(
                "final_count_exchange",
                id == 0 ? 0 : 3,
                id == 0 ? static_cast<std::uint64_t>(3) * (p - 1) : 0,
                id == 0 ? 0 : reduce_bytes,
                id == 0 ? static_cast<std::uint64_t>(p - 1) * reduce_bytes : 0);

            if (id == 0) {
                scaling::StageScope profile_stage("testout_io", "io");
                fp = fopen(savepvname.c_str(), "w");
                if (fp == NULL) {
                    fprintf(stderr, "[IO_ERROR] rank=0 cannot open %s\n", savepvname.c_str());
                    fflush(stderr);
                    MPI_Abort(MPI_COMM_WORLD, MPI_ERR_OTHER);
                    abort();
                }
                fprintf(fp, "the Sum of Volelements is %lld\r\n", total_volume_elements);
                fprintf(fp, "volume mesh generate time mean is %.6f s\r\n",
                        sum_volume_mesh_time / static_cast<double>(p));
                fprintf(fp, "volume mesh generate time max is %.6f s\r\n",
                        max_volume_mesh_time);
                fclose(fp);
            }
        }
        if (!profiler.core_only()) {
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "meshQualityEvaluation.begin");
            meshQualityEvaluation(submesh, id, OUTPUT_PATH);
            netgen_mpi_checkpoint(MPI_COMM_WORLD, "meshQualityEvaluation.end");
        }


    }

    if(id == 0) cout << "successful!!!" << endl;
    profiler.finalize();
    netgen_mpi_checkpoint(MPI_COMM_WORLD, "MPI_Finalize.begin");
    MPI_Finalize();

    return 0;
}
