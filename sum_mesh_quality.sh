#!/bin/bash

# 获取目标目录（支持命令行传参，默认使用预设路径）
TARGET_DIR="${1:-/vol8/home/hnu_lhz/cjz/NETGEN/test_code_part03/test_r23_c32/meshQuality}"
ERR_DIR="/vol8/home/hnu_lhz/cjz/NETGEN/test_code_part03/err"

# 1. 解析层数和核数 (从目录名中匹配 _rXX_cYY)
JOB_NAME=$(basename $(dirname "$TARGET_DIR"))
if [[ "$JOB_NAME" =~ _r([0-9]+)_c([0-9]+) ]]; then
    LAYERS="${BASH_REMATCH[1]}"
    CORES="${BASH_REMATCH[2]}"
    # 拆分表面细化和体细化：如果层数是2位数(例如 33)，第一位是表面，第二位是体
    if [ ${#LAYERS} -eq 2 ]; then
        SURF_REF="${LAYERS:0:1}"
        VOL_REF="${LAYERS:1:1}"
    else
        SURF_REF="$LAYERS"
        VOL_REF="$LAYERS"
    fi
else
    SURF_REF="N/A"
    VOL_REF="N/A"
    CORES="N/A"
fi

# 2. 从对应的超算 err 目录中提取该作业最新的运行时间
OUT_FILE=$(ls -t "$ERR_DIR"/*_r${LAYERS}_c${CORES}*.out 2>/dev/null | head -n 1)
if [ -n "$OUT_FILE" ] && [ -f "$OUT_FILE" ]; then
    RUNTIME=$(grep "运行时间:" "$OUT_FILE" | head -n 1 | sed 's/.*运行时间:[[:space:]]*//')
    [ -z "$RUNTIME" ] && RUNTIME="未找到"
else
    RUNTIME="日志文件未生成"
fi

# 3. 统计网格信息并格式化统一输出
awk -v surf="$SURF_REF" -v vol="$VOL_REF" -v cores="$CORES" -v runtime="$RUNTIME" '
/Point_Num:/ {
    p+=$2;
    s+=$4;
    v+=$6;
}
END {
    print  "=====================================";
    printf "表面细化层数 (numlevels): %s\n", surf;
    printf "体细化层数   (numrefine): %s\n", vol;
    printf "核数         (Cores):     %s\n", cores;
    printf "运行时间     (Time):      %s\n", runtime;
    print  "-------------------------------------";
    printf "Total Point_Num:    %'\''d\n", p;
    printf "Total SurfEle_Num:  %'\''d\n", s;
    printf "Total SoildEle_Num: %'\''d\n", v;
    print  "=====================================";
}' "$TARGET_DIR"/*.txt
