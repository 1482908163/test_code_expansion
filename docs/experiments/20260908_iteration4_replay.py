#!/usr/bin/env python3
"""第四次迭代离线核验：训练 -1/17，评价 41；不把重映射预测当成实测加速。"""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics as st
import sys

REPO=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(REPO/'strong_scaling'))
from resource_model import read_samples, train, digest
from fit_cost_model import predict


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',type=Path,default=REPO/'strong_scaling_results/mesh_algorithms_20260908-135236')
    parser.add_argument('--models',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();reports=[]
    for p in (1024,2048,4096,8192):
        path=args.models/f'p{p}.mapping'
        if not path.exists():train(args.data,args.models,p,3,3,16)
        meta=json.loads(path.with_suffix('.json').read_text());model=meta['coefficients']
        training,_,source=read_samples(args.data,p,(-1,17))
        if meta['source']['sha256']!=source['sha256']:raise ValueError('模型来源不同')
        held,_,_=read_samples(args.data,p,(41,))
        # 只使用参考分区 -1 的第一次正式测量近似本次作业的参考预热。
        # 这些历史参考测量也参与了训练，故能力标定误差可能偏乐观。
        logs=defaultdict(list)
        for row in training:
            if row['seed']==-1 and row['repeat']==1:
                logs[row['host']].append(math.log(sum(row['y'])/meta['anchors'][str(row['rank'])]))
        capacities={h:math.exp(st.fmean(v)) for h,v in logs.items()}
        groups=defaultdict(list)
        for row in held:groups[row['repeat']].append(row)
        runs=[]
        for rep,rows in sorted(groups.items()):
            work={r['rank']:sum(predict(model,r)) for r in rows}
            actual=[sum(r['y']) for r in rows]
            geometry=[work[r['rank']] for r in rows]
            joint=[work[r['rank']]*capacities[r['host']] for r in rows]
            bundle=[max(work[i] for i in range(b*16,(b+1)*16)) for b in range(p//16)]
            candidate=max(w*s for w,s in zip(sorted(bundle,reverse=True),sorted(capacities.values())))
            before=max(joint);gain=before-candidate
            worst=max(range(p),key=actual.__getitem__)
            runs.append(dict(repeat=rep,observed_compute_max=max(actual),predicted_max_before=before,
                predicted_max_candidate=candidate,predicted_gain_seconds=gain,
                predicted_adopt=gain>max(.35,.05*before),
                geometry_compute_wape=sum(abs(a-b) for a,b in zip(actual,geometry))/sum(actual),
                resource_compute_wape=sum(abs(a-b) for a,b in zip(actual,joint))/sum(actual),
                slowest_rank_prediction_error=(joint[worst]-actual[worst])/actual[worst]))
        reports.append(dict(ranks=p,nodes=p//16,model_sha256=digest(path),runs=runs,
            node_slowdown_ratio=max(capacities.values())/min(capacities.values()),
            median={k:st.median(r[k] for r in runs) for k in runs[0] if k not in ('repeat','predicted_adopt')},
            predicted_adoptions=sum(r['predicted_adopt'] for r in runs)))
    report=dict(data_commit='35ba2e5094eafef14b0ac25b5ed3abb31e55bef4',
        source_commit='c6a2f1114603f811ac3cbb9ccf06d91c6c66e417',
        training_seeds=[-1,17],evaluation_seed=41,training_protocol='same-P held-partition-out',
        warning='离线预测核验，不是重映射后的实际核心时间；历史参考测量参与过训练，能力标定可能偏乐观。',
        process_counts=reports)
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    for r in reports:
        m=r['median']
        print(r['ranks'], 'WAPE geometry/resource',round(m['geometry_compute_wape']*100,2),
              round(m['resource_compute_wape']*100,2),'predicted gain',round(m['predicted_gain_seconds'],3),
              'adopt',r['predicted_adoptions'])

if __name__=='__main__':main()
