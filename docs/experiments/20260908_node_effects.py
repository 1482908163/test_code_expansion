#!/usr/bin/env python3
"""节点关联诊断：逻辑分区、运行轮次及主机的对数耗时固定效应。

这是观察性残差分析，不把拟合方差解释率当作因果比例。
需要 NumPy 与 SciPy；不进入集群网格生成或生产训练路径。
"""
import argparse
from collections import defaultdict
import csv,gzip,itertools,json,hashlib,runpy
from pathlib import Path
import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.linalg import lsqr

def effects(y,group,run,host=None):
    arrays=[group,run]+([host] if host is not None else [])
    n=len(y);columns=[];offsets=[0]
    for array in arrays:
        columns.append(array+offsets[-1]);offsets.append(offsets[-1]+int(array.max())+1)
    matrix=coo_matrix((np.ones(n*len(arrays)),(np.tile(np.arange(n),len(arrays)),np.concatenate(columns))),shape=(n,offsets[-1])).tocsr()
    solution=lsqr(matrix,y,atol=1e-11,btol=1e-11,iter_lim=3000)
    if solution[1] not in (1,2): raise ValueError(f'fixed effects did not converge: {solution[1]}')
    residual=y-matrix@solution[0]
    host_coeff=solution[0][offsets[-2]:] if host is not None else None
    return residual,host_coeff

def ids(values):return np.unique(values,return_inverse=True)[1]

def summarize(values):
    return dict(zip(('min','median','p95','max'),map(float,np.quantile(values,[0,.5,.95,1]))))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();result=[];host_sets={}
    archive=runpy.run_path(str(Path(__file__).with_name('20260908_iteration3_analysis.py')))
    for p in (1024,2048,4096,8192):
        source=args.data/f'p{p}/analysis/model_samples.csv.gz';payload=source.read_bytes()
        if hashlib.sha1(b'blob '+str(len(payload)).encode()+b'\0'+payload).hexdigest()!=archive['SAMPLE_BLOBS'][p]:
            raise ValueError('raw sample source changed')
        with gzip.open(source,'rt') as f:rows=list(csv.DictReader(f))
        y=np.log([float(r['y1']) for r in rows]);seeds=np.array([int(r['partition_seed']) for r in rows])
        group=ids([r['partition_seed']+'/'+r['logical_partition'] for r in rows])
        run=ids([r['partition_seed']+'/'+r['repeat'] for r in rows])
        hosts=np.array([r['processor_name'] for r in rows]);host=ids(hosts);host_sets[p]=set(hosts)
        simple,_=effects(y,group,run);full,b=effects(y,group,run,host)
        validation=[]
        for seed in (-1,17,41):
            training=seeds!=seed;test=~training
            _,beta=effects(y[training],ids(group[training]),ids(run[training]),host[training])
            before,_=effects(y[test],ids(group[test]),ids(run[test]))
            after,_=effects(y[test]-beta[host[test]],ids(group[test]),ids(run[test]))
            validation.append(dict(held_out_seed=seed,within_partition_run_sse_reduction=float(1-np.dot(after,after)/np.dot(before,before))))
        # 主机系数可能分成互不连通的轮换组；不输出组间不可识别的绝对速度比。
        result.append(dict(ranks=p,hosts=len(host_sets[p]),
            within_partition_run_log_rmse=float(np.sqrt(np.mean(simple**2))),
            with_host_log_rmse=float(np.sqrt(np.mean(full**2))),
            host_associated_sse_reduction=float(1-np.dot(full,full)/np.dot(simple,simple)),
            cross_seed_validation=validation))
        print(p,result[-1],flush=True)
    overlap=[dict(scales=[a,b],shared_nodes=len(host_sets[a]&host_sets[b])) for a,b in itertools.combinations(host_sets,2)]
    args.output.write_text(json.dumps(dict(data_commit=archive['DATA_COMMIT'],scales=result,node_overlap=overlap,
        definition='log(local_volume_mesh time) = logical-partition effect + seed/repeat run effect + host effect; observational association, not causal attribution',
        validation='Host effects fitted on the other two seeds. Held-out residuals are centered by partition/run for diagnosis only, not a deployable predictor.'),ensure_ascii=False,indent=2)+'\n')

if __name__=='__main__':main()
