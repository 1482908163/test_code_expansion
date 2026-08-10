#!/bin/bash

#SBATCH --job-name=Mesh_test # 程序的作业名
#SBATCH --output=/vol8/home/hnu_lhz/cjz/NETGEN/test_code_time/err/Mesh_8dsp_%j.out         # 标准输出文件
#SBATCH --error=/vol8/home/hnu_lhz/cjz/NETGEN/test_code_time/err/Mesh_8dsp_%j.err          # 错误输出文件
#SBATCH -p mt_module
#SBATCH --nodes=1                     # 请求的节点数
#SBATCH --ntasks-per-node=1         # 每个节点上的任务数 (进程数)
#SBATCH --ntasks=1                    # 请求的任务数 (总核数)
#SBATCH --cpus-per-task=1             # 每个任务的CPU核心数

# 环境变量设置
export GCCHOME=/vol8/home/hnu_lhz/cjz/gcc-12
export CMAKE_HOME=/vol8/home/hnu_lhz/cjz/Lib/cmake
export PATH=$GCCHOME/bin:$CMAKE_HOME/bin:$PATH
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$GCCHOME/lib64
export C_INCLUDE_PATH=$GCCHOME/include/
export CPLUS_INCLUDE_PATH=$GCCHOME/include/
export LD_PRELOAD=$GCCHOME/lib64/libstdc++.so.6:$LD_PRELOAD
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/vol8/home/hnu_lhz/cjz/aarch64-linux-gnu
export DSPGCCROOT=/vol8/appsoftware/mt3000_programming_env/dsp_compiler
export PATH="$DSPGCCROOT/bin:$PATH"
export LD_LIBRARY_PATH="$DSPGCCROOT/lib:$DSPGCCROOT/lib64:$LD_LIBRARY_PATH"

# 作业参数设置
numlevels=1 #分层数
numrefine=1 #细化次数
maxh=1000.0 #网格最大/最小单元尺度
minh=0.0
input_path=/vol8/home/hnu_lhz/cjz/NETGEN/test_code_time/inputData/wholewall3solid.STEP
output_path=/vol8/home/hnu_lhz/cjz/NETGEN/test_code_time/result/test/

# 创建输出目录
mkdir -p $output_path

# 加载模块和设置线程数
module load mpich/mpi-x
export OMP_NUM_THREADS=1   # 每个 MPI 进程开 1 线程

echo "=== ENV BEFORE YHRUN ==="
env | egrep 'LD_|PMIX|OMPI|MPI|PATH'
echo "=== LDD CHECK ==="
ldd ./build/mesh_occ_mpi/mesh_occ_mpi | grep "not found" || true

# 记录开始时间
echo "=== 开始运行 ==="
echo "开始时间: $(date)"
start_time=$(date +%s)

# 运行程序 - 使用pmix而不是pmi2
yhrun -p mt_module --mpi=pmi2 ./build/mesh_occ_mpi/mesh_occ_mpi  -i $input_path -o $output_path -l $numlevels -r $numrefine --maxh $maxh --minh $minh -v -adj

# 记录结束时间和运行时间
end_time=$(date +%s)
runtime=$((end_time - start_time))
echo "结束时间: $(date)"
echo "运行时间: $runtime 秒"
echo "=== 运行结束 ==="
