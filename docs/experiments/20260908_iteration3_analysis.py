#!/usr/bin/env python3
"""复算第三次迭代校准；保留目标规模留出，不修改原始数据或算法。"""
import argparse
from collections import Counter,defaultdict
from concurrent.futures import ProcessPoolExecutor
import csv
import gzip
import hashlib
import itertools
import json
import math
from pathlib import Path
import runpy
import statistics as st

DATA_COMMIT='35ba2e5094eafef14b0ac25b5ed3abb31e55bef4'
SOURCE_COMMIT='c6a2f1114603f811ac3cbb9ccf06d91c6c66e417'
FIT_SHA256='d6196655d1b77b17edc07e7e78414dc28f081a436a68d115b4a25ef17daccd31'
BATCH='strong_scaling_results/mesh_algorithms_20260908-135236'
SAMPLE_BLOBS={1024:'c594b23c8ae9b45b3a9151a1a3da6dae7643778a',2048:'f91554992472babf131094fdcbab75d7d4142034',4096:'f53377c4f05be725198dd017da9e823432db3d87',8192:'e2bc50b1f9c32c4cd414765b247f08a98c027689'}
SEEDS=(-1,17,41)

def require(ok,message):
    if not ok: raise ValueError(message)

def read_csv(path,delimiter=','):
    with path.open(newline='') as f:return list(csv.DictReader(f,delimiter=delimiter))

def quantiles(values):
    values=sorted(values)
    def q(p):
        pos=(len(values)-1)*p;lo=int(pos);hi=min(lo+1,len(values)-1)
        return values[lo]+(values[hi]-values[lo])*(pos-lo)
    return dict(minimum=values[0],median=q(.5),p95=q(.95),maximum=values[-1])

def ranks(a):
    ordered=sorted(range(len(a)),key=a.__getitem__);result=[0.0]*len(a);i=0
    while i<len(a):
        j=i+1
        while j<len(a) and a[ordered[j]]==a[ordered[i]]:j+=1
        for k in ordered[i:j]:result[k]=(i+j-1)/2
        i=j
    return result

def correlation(a,b):
    ma,mb=st.fmean(a),st.fmean(b)
    denom=math.sqrt(sum((x-ma)**2 for x in a)*sum((x-mb)**2 for x in b))
    return sum((x-ma)*(y-mb) for x,y in zip(a,b))/denom if denom else None

def partition_overlap(a,b):
    choose=lambda n:n*(n-1)//2
    same=sum(choose(n) for n in Counter(zip(a,b)).values())
    ca=sum(choose(n) for n in Counter(a).values());cb=sum(choose(n) for n in Counter(b).values())
    expected=ca*cb/choose(len(a));denom=(ca+cb)/2-expected
    return dict(adjusted_rand=(same-expected)/denom if denom else 1,
                same_pair_recall=same/ca,same_pair_precision=same/cb)

def fit_target(arguments):
    fit_path,data,out,target=arguments;m=runpy.run_path(str(fit_path))
    path=out/f'p{target}.model';manifest=path.with_suffix('.json')
    if path.exists() and manifest.exists():
        meta=json.loads(manifest.read_text())
        require(meta['held_out_ranks']==target and target not in meta['training_ranks'],'wrong cached target')
        require(m['digest'](path)==meta['model_sha256'],'cached model changed')
        require(all(m['digest'](Path(s['path']))==s['sha256'] for s in meta['sources']),'cached source changed')
        return target
    contents,meta=m['train'](data,target,3,3)
    path.write_text(contents);manifest.write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n')
    return target

def main():
    repo=Path(__file__).resolve().parents[2]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',type=Path,default=repo/BATCH)
    parser.add_argument('--fit-script',type=Path,default=repo/'strong_scaling/fit_cost_model.py')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();data=args.data.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    require(hashlib.sha256(args.fit_script.read_bytes()).hexdigest()==FIT_SHA256,'Use the recorded fitting source')
    m=runpy.run_path(str(args.fit_script));result=dict(data_commit=DATA_COMMIT,source_commit=SOURCE_COMMIT,batch=BATCH)
    rows,identity,sources=m['load_samples'](data,-1,3,3)
    result.update(identity=identity,sources=sources,calibration=m['sampling_readiness'](data),scales=[])
    require(result['calibration']['ready'],'sampling readiness failed')
    for p,blob in SAMPLE_BLOBS.items():
        folder=data/f'p{p}';path=folder/'analysis/model_samples.csv.gz';payload=path.read_bytes()
        require(hashlib.sha1(b'blob '+str(len(payload)).encode()+b'\0'+payload).hexdigest()==blob,'raw source changed')
        statuses=read_csv(folder/'run_status.tsv','\t')
        require(len(statuses)==12 and all(r['exit_code']=='0' for r in statuses),'failed/missing executions')
        require({(int(r['partition_seed']),int(r['repeat'])) for r in statuses}==set(itertools.product(SEEDS,range(4))),'wrong execution matrix')
        groups=defaultdict(dict);seen=set()
        with gzip.open(path,'rt',newline='') as f:
            for r in csv.DictReader(f):
                require(r['MESH_SOURCE_REVISION']==SOURCE_COMMIT and r['MESH_MODEL_SHA256']=='none','unexpected model/source')
                s,i,n=(int(r[k]) for k in ('partition_seed','logical_partition','repeat'))
                require((s,i,n) not in seen,'duplicate measurement');seen.add((s,i,n))
                groups[s,i][n]=dict(host=r['processor_name'],physical=int(r['rank']),shift=int(r['rank_shift']),
                    y=[float(r[f'y{k}']) for k in range(4)],volume=float(r['volume_elements']))
        require(seen==set(itertools.product(SEEDS,range(p),(1,2,3))),'missing measurements')
        cv=[st.pstdev(v[n]['y'][1] for n in (1,2,3))/st.fmean(v[n]['y'][1] for n in (1,2,3)) for v in groups.values()]
        host_counts=[len({v[n]['host'] for n in (1,2,3)}) for v in groups.values()]
        stability=[]
        for s in SEEDS:
            times={n:[sum(groups[s,i][n]['y']) for i in range(p)] for n in (1,2,3)}
            top=max(1,math.ceil(p*.01))
            for a,b in itertools.combinations((1,2,3),2):
                ta=set(sorted(range(p),key=times[a].__getitem__,reverse=True)[:top])
                tb=set(sorted(range(p),key=times[b].__getitem__,reverse=True)[:top])
                stability.append(dict(seed=s,repeats=[a,b],spearman=correlation(ranks(times[a]),ranks(times[b])),
                                      slowest_one_percent_overlap=len(ta&tb)/top))
        labels={s:list(map(int,(folder/f'partition_preflight/seed_{s}.labels').read_text().split()[5:])) for s in SEEDS}
        selected=[r for r in rows if r['ranks']==p]
        total=sum(sum(r['y']) for r in selected)
        result['scales'].append(dict(ranks=p,nodes=p//16,formal_rank_rows=len(seen),logical_samples=len(selected),
            distinct_features=len({tuple(r['x']) for r in selected}),hosts_per_partition=quantiles(host_counts),
            remesh_cross_node_cv=quantiles(cv),repeat_stability=stability,
            changed_volume_partitions=sum(len({r['volume'] for r in v.values()})>1 for v in groups.values()),
            shifts=sorted({r['shift'] for v in groups.values() for r in v.values()}),
            phase_shares={name:sum(r['y'][k] for r in selected)/total for k,name in enumerate(m['PHASES'])},
            coarse_cells=quantiles([r['x'][7] for r in selected]),
            partition_comparisons=[dict(seeds=[a,b],**partition_overlap(labels[a],labels[b])) for a,b in itertools.combinations(SEEDS,2)]))
    result['summaries']=read_csv(data/'analysis/summary.csv')
    result['stages']=[]
    for p in SAMPLE_BLOBS:
        for seed in SEEDS:
            rs=[r for r in read_csv(data/'analysis/stages.csv') if int(r['ranks'])==p and int(r['partition_seed'])==seed]
            grouped=defaultdict(list)
            for r in rs:grouped[r['stage']].append(r)
            result['stages'].append(dict(ranks=p,seed=seed,values={name:{col:st.median(float(r[col]) for r in v) for col in ('mean_seconds','max_seconds')} for name,v in grouped.items()}))
    (out/'integrity_and_placement.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print('分区与节点核验通过；开始按既定训练器复算四个留出模型。',flush=True)
    models=out/'models';models.mkdir(exist_ok=True)
    with ProcessPoolExecutor(max_workers=2) as pool:
        for p in pool.map(fit_target,[(args.fit_script.resolve(),data,models,p) for p in SAMPLE_BLOBS]):
            print(f'模型完成：留出 {p} 进程',flush=True)
    result['held_out']=[]
    for p in SAMPLE_BLOBS:
        train=[r for r in rows if r['ranks']!=p];held=[r for r in rows if r['ranks']==p]
        meta=json.loads((models/f'p{p}.json').read_text())
        coefficients=[list(map(float,line.split())) for line in (models/f'p{p}.model').read_text().splitlines()[1:5]]
        controls={'v2_eight_features':m['fit'](train,tuple(range(8))),
                  'fourteen_unweighted':m['fit'](train),
                  'fourteen_tail_weighted':coefficients}
        outside=[];ranges=meta['feature_ranges']
        for j,(lo,hi) in enumerate(ranges):
            count=sum(r['x'][j]<lo-1e-8*max(1,lo) or r['x'][j]>hi+1e-8*max(1,hi) for r in held)
            if count:outside.append(dict(feature=j,name=m['FEATURES'][j],count=count))
        out_count=sum(any(r['x'][j]<lo-1e-8*max(1,lo) or r['x'][j]>hi+1e-8*max(1,hi) for j,(lo,hi) in enumerate(ranges)) for r in held)
        envelope=meta['residual_envelope']
        gate=[]
        for seed in SEEDS:
            group=[r for r in held if r['seed']==seed]
            bad=[r['rank'] for r in group if any(r['x'][j]<lo-1e-8*max(1,lo) or r['x'][j]>hi+1e-8*max(1,hi) for j,(lo,hi) in enumerate(ranges))]
            seed_max=max(sum(m['predict'](coefficients,r)) for r in group)
            intercept=sum(phase[0] for phase in coefficients)
            required=max(.35,.05*seed_max)
            lower=seed_max*max(0,1-envelope['over'])
            intercept_reject=lower-intercept*(1+envelope['under'])<=required
            gate.append(dict(seed=seed,outside_count=len(bad),outside_examples=bad[:8],predicted_seed_max=seed_max,
                feature_range_would_skip=bool(bad),intercept_would_skip=intercept_reject,
                nominal_reduction_required_before_correction=1-(lower-required)/(seed_max*(1+envelope['under']))))
        result['held_out'].append(dict(ranks=p,training_ranks=meta['training_ranks'],scores={k:m['score'](model,held) for k,model in controls.items()},
            residual_envelope=envelope,minimum_nominal_reduction_ignoring_costs=1-max(0,1-envelope['over'])/(1+envelope['under']),
            outside_feature_box=out_count,outside_feature_fraction=out_count/len(held),outside_features=outside,
            model_sha256=meta['model_sha256'],internal_validation=meta['validation'],gate_audit=gate))
        print(f'目标验证完成：{p}',flush=True)
    path=out/'analysis.json';path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(path,flush=True)

if __name__=='__main__':main()
