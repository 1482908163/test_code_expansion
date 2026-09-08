#!/usr/bin/env python3
"""第四次迭代：同规模几何/节点联合模型与当前作业能力测量（标准库）。

本文件由统一入口内部调用。训练种子固定 -1、17；41 不参与拟合。
"""
import argparse
from collections import defaultdict
import csv
import gzip
import json
import math
from pathlib import Path
import statistics as st
import sys

from fit_cost_model import digest, fit, predict, verify_preflight, check_identity, IDENTITY

FEATURE_CONTRACT = 'b7dc4d9555fe44c5e9cad750db6e002c3a87527964cf828030213dfb90ced5dd'
TRAIN_SEEDS = (-1, 17)
REFERENCE_SEED = -1


def need(ok, message):
    if not ok:
        raise ValueError(message)


def read_samples(root, p, seeds):
    folder=root/f'p{p}'; path=folder/'analysis/model_samples.csv.gz'
    need((folder/'analysis/issues.txt').read_text().strip()=='', '校准数据存在异常')
    meta=verify_preflight(folder/'partition_preflight')
    need(meta['variant']=='cell_order_v1', '需要真实粗单元重排样本')
    manifest_sha=digest(folder/'partition_preflight/manifest.json')
    samples=[]; seen=set(); groups=defaultdict(list)
    with gzip.open(path,'rt') as stream:
        for raw in csv.DictReader(stream):
            # 留出分区在读取任何数值特征/耗时之前排除。
            seed=int(raw['partition_seed'])
            if seed not in seeds: continue
            need(raw['EXPERIMENT_STAGE']=='calibration' and raw['algorithm']=='sparse' and
                 raw['feature_schema']=='mesh_phase_v3' and raw['sampling_protocol']=='partition_sampling_v1',
                 '不是第三次迭代的有效校准样本')
            check_identity(raw,meta['identity'])
            need(raw['MESH_PREFLIGHT_SHA256']==manifest_sha and
                 raw['MESH_PARTITION_SIGNATURE']==meta['partitions'][str(seed)]['signature'] and
                 raw['MESH_SOURCE_REVISION']==meta['source_revision'], '样本与预检不一致')
            rank=int(raw['rank']); logical=int(raw['logical_partition']); rep=int(raw['repeat'])
            shift=int(raw['rank_shift']); rpn=int(raw['RANKS_PER_NODE'])
            need(int(raw['ranks'])==p and rpn>0 and p%rpn==0 and shift%rpn==0 and
                 0<=logical<p and 0<=rank<p and (logical+shift)%p==rank and rep>0,
                 '无效的分区映射')
            key=(seed,rep,logical); need(key not in seen,'重复样本');seen.add(key)
            row=dict(ranks=p,seed=seed,rank=logical,physical=rank,repeat=rep,rpn=rpn,
                     host=raw['processor_name'],x=[float(raw[f'x{k}']) for k in range(14)],
                     y=[float(raw[f'y{k}']) for k in range(4)])
            need(row['host'] not in ('','unset') and row['x'][0]==1 and sum(row['y'])>0 and
                 all(math.isfinite(v) and v>=0 for v in row['x']+row['y']), '无效耗时/特征')
            samples.append(row);groups[seed,logical].append(row)
    need(len(groups)==p*len(seeds),'校准分区缺失')
    for rows in groups.values():
        need(len(rows)>=3 and len({r['host'] for r in rows})>=min(3,p//rows[0]['rpn']) and
             all(r['x']==rows[0]['x'] for r in rows), '重复或节点覆盖不足')
    need(len({r['rpn'] for r in samples})==1,'不一致的每节点进程数')
    return samples,meta,dict(path=str(path.resolve()),sha256=digest(path))


def joint_fit(samples, rounds=6):
    """交替拟合非负四阶段几何成本和共享的节点乘性因子；不拟合永久节点速度。"""
    groups=defaultdict(list)
    for row in samples: groups[row['seed'],row['rank']].append(row)
    factors={r['host']:1.0 for r in samples}
    def corrected():
        return [dict(ranks=rows[0]['ranks'],seed=key[0],rank=key[1],x=rows[0]['x'],
                     y=[st.median(r['y'][k]/factors[r['host']] for r in rows) for k in range(4)])
                for key,rows in sorted(groups.items())]
    for _ in range(rounds):
        model=fit(corrected(),tail_weighted=True,max_iterations=100000)
        logs=defaultdict(list)
        for row in samples:
            estimate=sum(predict(model,row));need(estimate>0,'零计算代价预测')
            logs[row['host']].append(math.log(sum(row['y'])/estimate))
        raw={h:st.fmean(v) for h,v in logs.items()};center=st.fmean(raw.values())
        factors={h:math.exp(v-center) for h,v in raw.items()}
    rows=corrected(); model=fit(rows,tail_weighted=True,max_iterations=100000)
    return model,factors,rows


def train(root, out, p, levels, refines, rpn):
    contract=Path(__file__).resolve().parents[1]/'mesh_occ_mpi/partition_cost.h'
    need(digest(contract)==FEATURE_CONTRACT,'几何特征定义已改变，不能直接复用第三次样本')
    samples,source,provenance=read_samples(root,p,TRAIN_SEEDS)
    need(int(source['identity']['numlevels'])==levels and int(source['identity']['numrefine'])==refines and
         samples[0]['rpn']==rpn,'训练与目标配置不一致')
    model,factors,corrected=joint_fit(samples)
    anchors={str(r['rank']):sum(r['y']) for r in corrected if r['seed']==REFERENCE_SEED}
    text=f'mesh_resource_v1 {p} {levels} {refines} {rpn} 14\n'
    text+=''.join(' '.join(format(v,'.17g') for v in phase)+'\n' for phase in model)
    out.mkdir(parents=True,exist_ok=True);path=out/f'p{p}.mapping'
    meta=dict(schema='mesh_resource_v1',ranks=p,rpn=rpn,levels=levels,refines=refines,
        training_seeds=list(TRAIN_SEEDS),evaluation_seed=41,reference_seed=REFERENCE_SEED,
        identity=source['identity'],source_revision=source['source_revision'],source=provenance,
        feature_contract_sha256=FEATURE_CONTRACT,reference_partition=source['partitions']['-1'],
        coefficients=model,anchors=anchors,
        transfer='same-P fixed geometry features; intentionally allows rebuilt executable; current binary separately recorded',
        training_node_slowdown_min=min(factors.values()),training_node_slowdown_max=max(factors.values()))
    # 提交重试可以复用冻结模型；禁止覆盖已有且不同的模型。
    if path.exists():
        need(path.read_text()==text,'已有冻结模型不同，请使用新结果目录')
        old=json.loads(path.with_suffix('.json').read_text())
        need(old['source']['sha256']==provenance['sha256'],'已有模型样本来源改变')
        return
    path.write_text(text);meta['model_sha256']=digest(path)
    path.with_suffix('.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n')
    print(f'p{p}: 已冻结同规模模型（训练 -1/17，评价 41）',flush=True)


def verify(path,p,levels,refines,rpn):
    meta=json.loads(path.with_suffix('.json').read_text())
    need(meta['schema']=='mesh_resource_v1' and meta['model_sha256']==digest(path) and
         (meta['ranks'],meta['levels'],meta['refines'],meta['rpn'])==(p,levels,refines,rpn) and
         meta['training_seeds']==list(TRAIN_SEEDS),'资源模型不匹配')
    contract=Path(__file__).resolve().parents[1]/'mesh_occ_mpi/partition_cost.h'
    need(meta['feature_contract_sha256']==digest(contract)==FEATURE_CONTRACT,'几何特征契约改变')
    return meta


def calibrate(path, preflight, profile, output, elapsed):
    meta=json.loads(path.with_suffix('.json').read_text());p=meta['ranks'];rpn=meta['rpn']
    current=verify_preflight(preflight)
    need(current['partitions']['-1']==meta['reference_partition'],'预热参考分区与训练分区不同')
    opener=gzip.open if profile.suffix=='.gz' else open
    with opener(profile,'rt') as stream: rows=[json.loads(line) for line in stream if line.strip()]
    rows.sort(key=lambda r:r['rank'])
    need([r['rank'] for r in rows]==list(range(p)),'当前节点预热缺失')
    from analyze_results import COMPUTE, seconds, inspect
    inspect(profile)  # 完整阶段、事件顺序、映射检查，与正式数据使用同一口径。
    metadata=rows[0]['metadata']
    need(metadata['MESH_MODEL_SHA256']==digest(path) and metadata['partition_seed']=='-1' and
         metadata['rank_shift']=='0' and metadata['algorithm']=='sparse' and
         metadata['timing_mode']=='natural' and rows[0]['repeat']==0,'不是当前模型的参考预热')
    check_identity(metadata,current['identity'])
    names=[];factors=[];used=set()
    for begin in range(0,p,rpn):
        group=rows[begin:begin+rpn];host=group[0]['processor_name']
        need(host not in used and all(r['processor_name']==host for r in group),'需要节点连续分配')
        used.add(host)
        ratios=[sum(seconds(r,s) for s in COMPUTE)/meta['anchors'][str(r['rank'])] for r in group]
        need(all(math.isfinite(v) and v>0 for v in ratios),'当前节点能力无效')
        factor=math.exp(st.fmean(map(math.log,ratios)))
        names.extend([host]*rpn);factors.extend([factor]*rpn)
    text=f'mesh_capacity_v1 {p} {rpn}\n'+''.join(f'{h} {v:.17g}\n' for h,v in zip(names,factors))
    output.write_text(text)
    report=dict(model_sha256=digest(path),capacity_sha256=digest(output),identity=current['identity'],
                setup_wall_seconds=elapsed,reference_profile_sha256=digest(profile),
                slowdown_min=min(factors),slowdown_max=max(factors),slowdown_ratio=max(factors)/min(factors),
                note='本次分配的一次稀疏预热；准备成本单列，正式计时包含映射成本；非永久节点标定')
    output.with_suffix('.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--train',type=Path);parser.add_argument('--output',type=Path)
    parser.add_argument('--model',type=Path);parser.add_argument('--preflight',type=Path)
    parser.add_argument('--profile',type=Path);parser.add_argument('--elapsed',type=float,default=0)
    parser.add_argument('--verify-capacity',type=Path)
    parser.add_argument('--target-ranks',type=int,nargs='+',required=True)
    parser.add_argument('--levels',type=int,required=True);parser.add_argument('--refines',type=int,required=True)
    parser.add_argument('--rpn',type=int,required=True)
    args=parser.parse_args()
    if args.train:
        for p in args.target_ranks:train(args.train,args.output,p,args.levels,args.refines,args.rpn)
        return
    need(len(args.target_ranks)==1,'一次只检查一个模型')
    meta=verify(args.model,args.target_ranks[0],args.levels,args.refines,args.rpn)
    if args.preflight:
        current=verify_preflight(args.preflight)
        for key in IDENTITY:
            if key=='MESH_BINARY_SHA256':continue  # 唯一允许的身份转移；特征契约已校验。
            a,b=current['identity'][key],meta['identity'][key]
            need(float(a)==float(b) if key in ('maxh','minh') else a==b,'模型配置不匹配: '+key)
        need(current['partitions']['-1']==meta['reference_partition'],'参考粗网格划分改变')
    if args.profile:calibrate(args.model,args.preflight,args.profile,args.output,args.elapsed)
    if args.verify_capacity:
        report=json.loads(args.verify_capacity.with_suffix('.json').read_text())
        need(report['model_sha256']==digest(args.model) and
             report['capacity_sha256']==digest(args.verify_capacity) and
             report['identity']==current['identity'],'缓存能力文件与本次作业配置不一致')


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,KeyError) as error:
        print('资源映射配置错误: '+str(error),file=sys.stderr);sys.exit(2)
