#!/usr/bin/env python3
"""复算第四次四算法评价；仅分析数据，不修改生产算法。需要 NumPy/SciPy。"""
import argparse,csv,gzip,hashlib,json,math,statistics as st,runpy
from pathlib import Path
import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--data',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();DATA=args.data
fx=runpy.run_path(str(Path(__file__).with_name('20260908_node_effects.py')))['effects']
SAMPLE_BLOBS={1024:'4efefc0383571300828d2ee6fb929e85320e23fe',2048:'dbabb4a655af41ee80daa18d3813682f5f49575b',4096:'a1e4ffdc0567e8dc9b8dd2677dee294c7af93b3e',8192:'7e502d72b0ecc0cad943a1368446686ffc03f041'}
def blob(path):
 payload=path.read_bytes()
 return hashlib.sha1(b'blob '+str(len(payload)).encode()+b'\0'+payload).hexdigest()
assert blob(DATA/'analysis/runs.csv')=='15d35c22cf0e0162ffbe6870f0746f86626f10d4'
assert not (DATA/'analysis/issues.txt').read_text().strip()
ALG=('baseline','balance','sparse','combined')
runs=list(csv.DictReader((DATA/'analysis/runs.csv').open()))
assert len(runs)==160 and len({(r['algorithm'],r['timing'],r['ranks'],r['repeat']) for r in runs})==160
output=[]
for p in (1024,2048,4096,8192):
 folder=DATA/f'p{p}'
 assert blob(folder/'analysis/model_samples.csv.gz')==SAMPLE_BLOBS[p]
 statuses=list(csv.DictReader((folder/'run_status.tsv').open(),delimiter='\t'))
 assert len(statuses)==48 and all(r['exit_code']=='0' for r in statuses)
 capacity_lines=(folder/'node_capacities.txt').read_text().splitlines()[1:]
 cap={h:float(v) for h,v in (line.split() for line in capacity_lines)};hosts=sorted(cap);hid={h:i for i,h in enumerate(hosts)}
 model=json.load(open(DATA/f'models/p{p}.json'));coef=np.array(model['coefficients']).sum(axis=0)
 X=np.zeros((p,14));initialized=np.zeros(p,dtype=bool);tets=np.zeros(p,dtype=np.int64)
 ys=np.zeros((20,p,4));hs=np.full((20,p),-1,dtype=int);physical=np.full((20,p),-1,dtype=int)
 changed_features=changed_tets=0;metadata=set();shifts={}
 with gzip.open(folder/'analysis/model_samples.csv.gz','rt') as f:
  for row in csv.DictReader(f):
   a=ALG.index(row['algorithm']);rep=int(row['repeat']);g=int(row['logical_partition']);idx=a*5+rep-1
   assert 1<=rep<=5 and 0<=g<p and hs[idx,g]==-1
   assert row['partition_seed']=='41' and row['balance_method']=='node_mapping' and int(row['ranks'])==p
   x=np.array([float(row[f'x{k}']) for k in range(14)]);n=int(float(row['volume_elements']))
   if not initialized[g]:X[g]=x;tets[g]=n;initialized[g]=True
   else:changed_features+=not np.array_equal(X[g],x);changed_tets+=n!=tets[g]
   ys[idx,g]=[float(row[f'y{k}']) for k in range(4)]
   hs[idx,g]=hid[row['processor_name']];physical[idx,g]=int(row['rank'])
   metadata.add(tuple(row[k] for k in ('MESH_SOURCE_REVISION','MESH_BINARY_SHA256','MESH_MODEL_SHA256','MESH_CAPACITY_SHA256','MESH_INPUT_SHA256','numlevels','numrefine','RANKS_PER_NODE')))
   shifts[idx]=int(row['rank_shift'])
 assert np.all(hs>=0) and np.all(initialized) and len(metadata)==1
 identity=next(iter(metadata))
 assert identity[0]=='8f1a0a4ab6c96c01aea1d66e9f15b3933793b336'
 assert identity[1]=='b9583a23108eb270b681aa6ee5e77e572c89c661555708d6baca90bf15747904'
 assert identity[4]=='8193db1235a9d0dc24d19dd3b9a4dafccfe62d6667ab5a395f72b30a02339547'
 assert identity[5:]==('3','3','16')
 assert hashlib.sha256((DATA/f'models/p{p}.mapping').read_bytes()).hexdigest()==model['model_sha256']==identity[2]
 serialized=(DATA/f'models/p{p}.mapping').read_text().split()
 assert serialized[:6]==['mesh_resource_v1',str(p),'3','3','16','14']
 assert np.array_equal(np.array(serialized[6:],dtype=float).reshape(4,14),np.array(model['coefficients']))
 assert hashlib.sha256((folder/'node_capacities.txt').read_bytes()).hexdigest()==identity[3]
 assert all(len(set(rr))==p for rr in physical)
 work=X@coef;slow=np.array([cap[h] for h in hosts]);total=ys.sum(axis=2);pred=work[None,:]*slow[hs]
 actual_cv=np.std(total,axis=0,ddof=1)/np.mean(total,axis=0)
 # 每个逻辑分区在相同主机上的重复波动，与跨主机波动分开。
 gg=np.tile(np.arange(p),20);rr=np.repeat(np.arange(20),p);hh=hs.ravel();logy=np.log(total.ravel())
 residual,_=fx(logy,gg,rr);full,beta=fx(logy,gg,rr,hh)
 # 检查节点系数可识别的连通分量，只报告最大分量内部的速度跨度与相关性。
 a=hs[0].repeat(19);b=hs[1:].T.ravel();graph=coo_matrix((np.ones(len(a)),(a,b)),shape=(len(hosts),len(hosts)))
 nc,component=connected_components(graph,directed=False);largest=np.argmax(np.bincount(component));mask=component==largest
 beta_center=beta[mask]-beta[mask].mean();cap_center=np.log(slow[mask])-np.log(slow[mask]).mean()
 node_corr=float(np.corrcoef(beta_center,cap_center)[0,1])
 corrected=logy-np.log(slow[hh]);capres,_=fx(corrected,gg,rr)
 byalg={}
 for ai,algorithm in enumerate(ALG):
  sl=slice(ai*5,(ai+1)*5)
  groups=[r for r in runs if int(r['ranks'])==p and r['algorithm']==algorithm and r['timing']=='natural']
  checks=[]
  for r in groups:
   idx=ai*5+int(r['repeat'])-1
   assert abs(float(r['compute_max_seconds'])-total[idx].max())<1e-6
   assert sum(tets)==int(r['volume_elements_sum'])
   worst=int(total[idx].argmax());predworst=int(pred[idx].argmax())
   top=max(1,math.ceil(.01*p));aset=set(np.argsort(total[idx])[-top:]);pset=set(np.argsort(pred[idx])[-top:])
   checks.append(dict(repeat=int(r['repeat']),shift=shifts[idx],actual_max=float(total[idx].max()),
     predicted_max=float(pred[idx].max()),actual_slowest_logical=worst,predicted_slowest_logical=predworst,
     predicted_at_slowest=float(pred[idx,worst]),top1pct_recall=len(aset&pset)/top,
     wape=float(np.sum(abs(pred[idx]-total[idx]))/np.sum(total[idx])),
     geometry_only_wape=float(np.sum(abs(work-total[idx]))/np.sum(total[idx]))))
  byalg[algorithm]=checks
 obs=dict(ranks=p,nodes=len(hosts),samples=20*p,changed_features=int(changed_features),changed_tets=int(changed_tets),
   provenance=list(next(iter(metadata))),
   capacity_ratio=float(slow.max()/slow.min()),
   compute_repeat_cv_median=float(np.median(actual_cv)),compute_repeat_cv_p95=float(np.quantile(actual_cv,.95)),
   fixed_effects=dict(connected_components=int(nc),largest_nodes=int(mask.sum()),
     uncorrected_log_rmse=float(np.sqrt(np.mean(residual**2))),
     capacity_corrected_log_rmse=float(np.sqrt(np.mean(capres**2))),
     empirical_host_log_rmse=float(np.sqrt(np.mean(full**2))),
     empirical_host_sse_reduction=float(1-np.sum(full**2)/np.sum(residual**2)),
     observed_node_ratio_largest_component=float(np.exp(beta_center.max()-beta_center.min())),
     capacity_empirical_node_log_correlation=node_corr),algorithms=byalg)
 # 用同一分区跨全部正式运行的校正中位数作事后固有成本，仅定位被16个分区捆绑限制的下界。
 intrinsic=np.median(total/np.exp(beta)[hs],axis=0)
 bundle=intrinsic.reshape(-1,16).max(axis=1)
 obs['hindsight_mapping_bound']=dict(note='使用评价数据和单因子事后模型，不是可部署预测或实测反事实',
   current_bundle_optimum=float(np.max(np.sort(bundle)[::-1]*np.sort(np.exp(beta)))),
   free_partition_optimum=float(np.max(np.sort(intrinsic)[::-1]*np.repeat(np.sort(np.exp(beta)),16))))
 summary={}
 for timing in ('natural','split'):
  summary[timing]={}
  for algorithm in ALG:
   selected=[r for r in runs if int(r['ranks'])==p and r['algorithm']==algorithm and r['timing']==timing]
   numeric=('core_seconds','compute_max_seconds','face_pipeline_seconds','face_exchange_seconds','vertex_wait_seconds',
            'partition_adopted','seed_max_seconds','candidate_max_seconds','mapping_moved_nodes',
            'partition_node_mapping_seconds','metis_partition_seconds','face_payload_receive_bytes','volume_elements_sum')
   summary[timing][algorithm]={k:st.median(float(r[k]) for r in selected) if selected[0][k] else None for k in numeric}
   vals=[float(r['core_seconds']) for r in selected]
   summary[timing][algorithm]['core_cv']=st.stdev(vals)/st.mean(vals)
  left={int(r['repeat']):float(r['core_seconds']) for r in runs if int(r['ranks'])==p and r['algorithm']=='sparse' and r['timing']==timing}
  right={int(r['repeat']):float(r['core_seconds']) for r in runs if int(r['ranks'])==p and r['algorithm']=='combined' and r['timing']==timing}
  summary[timing]['paired_combined_core_savings']=[left[i]-right[i] for i in sorted(left)]
 obs['summary']=summary;obs['capacity_setup']=json.load(open(folder/'node_capacities.json'))
 output.append(obs);print(p,'verified',obs['samples'],'natural rank samples',flush=True)
args.output.write_text(json.dumps(dict(data_commit='0deaf8584f249d219d8182f5ced06cf2f4a1eb90',
  source_commit='8f1a0a4ab6c96c01aea1d66e9f15b3933793b336',formal_runs=160,normal_warmups=32,
  natural_rank_samples=sum(r['samples'] for r in output),scales=output,
  scope='原始逐进程JSON及参考预热逐进程文件未上传；自然运行由导出样本复核，split阶段及核心时间引用上传汇总。',
  hindsight_note='事后固定效应与最优映射仅用于诊断，不是可部署预测、严格真实性能下界或因果实验。'),ensure_ascii=False,indent=2)+'\n')
