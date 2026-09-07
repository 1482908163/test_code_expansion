#!/usr/bin/env python3
"""Frozen, leave-process-count-out phase models. Python standard library only."""
import argparse
import csv
import gzip
import hashlib
import json
import math
import statistics as st
from collections import defaultdict
from pathlib import Path

FEATURES = ["constant", "refined_boundary_faces", "mean_spacing_volume",
            "small_face_tail_volume", "tet_shape_volume", "boundary_shape",
            "physical_boundary_faces", "coarse_cells", "area_variation_work",
            "volume_variation_work", "boundary_shape_second_moment", "tet_shape_second_moment_work",
            "isoperimetric_defect_work", "normal_anisotropy_work"]
PHASES = ["surface", "remesh", "refine", "adjacency_numbering"]
IDENTITY = ["MESH_INPUT_SHA256", "MESH_BINARY_SHA256", "numlevels", "numrefine",
            "maxh", "minh", "omp_num_threads"]

def digest(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda:f.read(1024*1024), b""):
            h.update(block)
    return h.hexdigest()

def fit(rows, columns=None, tail_weighted=False):
    """Nonnegative ridge least squares on RMS-scaled features (lambda=1e-4).

    Each process-count group has equal total weight; an 8192-rank sample must
    not outweigh a 1024-rank sample eightfold. No evaluation data/hyperparameter search.
    """
    features=len(rows[0]["x"])
    if columns is None: columns=tuple(range(features))
    groups=defaultdict(list)
    for i,r in enumerate(rows): groups[r["ranks"],r.get("seed",-1)].append(i)
    weight=[0.0]*len(rows)
    for ids in groups.values():
        # Training labels only. Fourfold weight on the slowest 10% of each
        # partition group; all groups still have equal total weight.
        tail=set(sorted(ids,key=lambda i:sum(rows[i]["y"]),reverse=True)[:max(1,math.ceil(.1*len(ids)))]) if tail_weighted else set()
        total=len(ids)+3*len(tail)
        for i in ids: weight[i]=(4 if i in tail else 1)/(len(groups)*total)
    scale=[math.sqrt(sum(w*r["x"][j]**2 for r,w in zip(rows,weight))) or 1 for j in columns]
    x=[[r["x"][j]/s for j,s in zip(columns,scale)] for r in rows]
    n=len(columns)
    gram=[[sum(w*a[j]*a[k] for a,w in zip(x,weight))+(1e-4 if j==k else 0)
           for k in range(n)] for j in range(n)]
    coefficients=[]
    for phase in range(4):
        rhs=[sum(w*a[j]*r["y"][phase] for a,r,w in zip(x,rows,weight)) for j in range(n)]
        beta=[0.0]*n
        for _ in range(20000):
            delta=0.0
            for j in range(n):
                value=max(0.0,(rhs[j]-sum(gram[j][k]*beta[k] for k in range(n) if k!=j))/gram[j][j])
                delta=max(delta,abs(value-beta[j])); beta[j]=value
            if delta<1e-9*max(1.0,max(beta)): break
        else: raise ValueError("nonnegative phase fit did not converge")
        raw=[0.0]*features
        for j,b,s in zip(columns,beta,scale): raw[j]=b/s
        coefficients.append(raw)
    return coefficients

def predict(model,row):
    return [sum(w*x for w,x in zip(phase,row["x"])) for phase in model]

def score(model,rows):
    estimated=[predict(model,r) for r in rows]
    result={}
    for k,name in enumerate(PHASES):
        total=sum(r["y"][k] for r in rows)
        result[name+"_wape"]=sum(abs(y[k]-r["y"][k]) for r,y in zip(rows,estimated))/total if total else 0
    actual=[sum(r["y"]) for r in rows]; pred=[sum(y) for y in estimated]
    result["compute_wape"]=sum(abs(a-b) for a,b in zip(actual,pred))/sum(actual)
    worst=max(range(len(rows)),key=lambda i:actual[i])
    result["actual_slowest_rank_relative_error"]=(pred[worst]-actual[worst])/actual[worst]
    result["predicted_max_relative_error"]=(max(pred)-max(actual))/max(actual)
    top=max(1,math.ceil(.01*len(rows)))
    a=set(sorted(range(len(rows)),key=actual.__getitem__,reverse=True)[:top])
    b=set(sorted(range(len(rows)),key=pred.__getitem__,reverse=True)[:top])
    result["slowest_one_percent_recall"]=len(a&b)/top
    groups=defaultdict(list)
    for row in rows: groups[row["ranks"],row.get("seed",-1)].append(row)
    result["worst_group_slowest_relative_error"]=result["actual_slowest_rank_relative_error"]
    if len(groups)>1:
        details=[dict(ranks=p,seed=s,metrics=score(model,group)) for (p,s),group in sorted(groups.items())]
        result["partition_groups"]=details
        # A seed holdout contains several process counts. Do not let the more
        # expensive small-P ranks hide wrong ordering inside the large-P group.
        result["slowest_one_percent_recall"]=st.fmean(g["metrics"]["slowest_one_percent_recall"] for g in details)
        result["worst_group_slowest_relative_error"]=max(
            (g["metrics"]["actual_slowest_rank_relative_error"] for g in details),key=abs)
    return result

def load_samples(root,target,levels,refines):
    paths=sorted(root.glob("p*/analysis/model_samples.csv.gz"))
    if not paths: raise ValueError("no phase samples: run calibration with the new executable first")
    grouped=defaultdict(list); identities=set(); sources=[];schemas=set();revisions=set()
    for path in paths:
        # Do not read target-rank samples, even to estimate normalization ranges.
        p=int(path.parent.parent.name[1:])
        if p==target: continue
        issues=path.parent/"issues.txt"
        if not issues.exists() or issues.read_text().strip():
            raise ValueError(f"incomplete calibration: {path.parent}")
        sources.append(dict(path=str(path.resolve()),sha256=digest(path)))
        seen=set()
        with gzip.open(path,"rt") as f:
            for r in csv.DictReader(f):
                if r["EXPERIMENT_STAGE"]!="calibration" or r["algorithm"]!="sparse": continue
                schema=r["feature_schema"]
                if schema not in ("mesh_phase_v2","mesh_phase_v3"): raise ValueError("wrong feature schema")
                schemas.add(schema)
                features=14 if schema=="mesh_phase_v3" else 8
                if int(r["ranks"])!=p: raise ValueError("sample rank count differs from directory")
                if int(r["numlevels"])!=levels or int(r["numrefine"])!=refines:
                    raise ValueError("calibration levels/refines differ from evaluation")
                seed=int(r.get("partition_seed",-1))
                if seed< -1: raise ValueError("invalid partition seed")
                key=(p,seed,int(r["rank"])); repeat=int(r["repeat"])
                if repeat<=0: continue
                if (key,repeat) in seen: raise ValueError("duplicate calibration sample")
                seen.add((key,repeat))
                identity=tuple(r[k] for k in IDENTITY)
                if any(v in ("unset","") for v in identity): raise ValueError("missing calibration provenance")
                if schema=="mesh_phase_v3":
                    revision=r.get("MESH_SOURCE_REVISION","")
                    if revision in ("","unset"): raise ValueError("missing source revision")
                    revisions.add(revision)
                identities.add(identity)
                x=[float(r[f"x{k}"]) for k in range(features)]
                y=[float(r[f"y{k}"]) for k in range(4)]
                if any(not math.isfinite(v) or v<0 for v in x+y) or sum(y)<=0 or x[0]!=1:
                    raise ValueError("invalid calibration feature/time")
                grouped[key].append((x,y))
    if len(identities)!=1 or len(schemas)!=1 or len(revisions)>1:
        raise ValueError("calibration must use one geometry, executable, schema and parameter set")
    rows=[]
    for (p,seed,rank), values in sorted(grouped.items()):
        if len(values)<2: raise ValueError("need at least two measured repeats per calibration rank")
        if any(x!=values[0][0] for x,y in values): raise ValueError("seed features changed across repeats")
        rows.append(dict(ranks=p,seed=seed,rank=rank,x=values[0][0],
                         y=[st.median(y[k] for x,y in values) for k in range(4)]))
    counts=sorted({r["ranks"] for r in rows})
    if len(counts)<2: raise ValueError("need at least two other process counts, excluding target")
    seeds=sorted({r["seed"] for r in rows})
    for p in counts:
        for seed in seeds:
            if [r["rank"] for r in rows if r["ranks"]==p and r["seed"]==seed]!=list(range(p)):
                raise ValueError("missing calibration ranks or partition seeds")
    if schemas=={"mesh_phase_v3"}:
        if len(seeds)<3: raise ValueError("v3 calibration requires at least three partition seeds")
        for p in counts:
            signatures={tuple(sorted(tuple(r["x"]) for r in rows if r["ranks"]==p and r["seed"]==s)) for s in seeds}
            if len(signatures)!=len(seeds):
                raise ValueError("partition seeds produced identical feature multisets; need distinct partitions")
    return rows,dict(zip(IDENTITY,next(iter(identities)))),sources

def train(root,target,levels,refines):
    rows,identity,sources=load_samples(root,target,levels,refines)
    counts=sorted({r["ranks"] for r in rows})
    features=len(rows[0]["x"]); guarded=features==14
    schema="mesh_phase_v3" if guarded else "mesh_phase_v2"
    seeds=sorted({r["seed"] for r in rows})
    validation=[];over_error=under_error=0.0
    folds=[("ranks",p) for p in counts]+([("seed",s) for s in seeds] if guarded else [])
    for kind,value in folds:
        train_rows=[r for r in rows if r[kind]!=value]
        held_out=[r for r in rows if r[kind]==value]
        fitted=fit(train_rows,tail_weighted=guarded)
        validation.append(dict(group_kind=kind,group=value,ranks=value if kind=="ranks" else None,
            phase_model=score(fitted,held_out),
            mean_spacing_control=score(fit(train_rows,(0,2)),held_out),
            v2_control=score(fit(train_rows,tuple(range(8))),held_out)))
        for row in held_out:
            estimate=sum(predict(fitted,row)); actual=sum(row["y"])
            if estimate<=0: raise ValueError("nonpositive held-out compute prediction")
            over_error=max(over_error,(estimate-actual)/estimate)
            under_error=max(under_error,(actual-estimate)/estimate)
    model=fit(rows,tail_weighted=guarded)
    contents=f"{schema} {levels} {refines} {target} {features} 4\n"+"".join(
        " ".join(format(v,".17g") for v in phase)+"\n" for phase in model)
    ranges=[[min(r["x"][j] for r in rows),max(r["x"][j] for r in rows)] for j in range(features)]
    if guarded:
        contents+=f"{over_error:.17g} {under_error:.17g}\n"+"".join(f"{lo:.17g} {hi:.17g}\n" for lo,hi in ranges)
    manifest=dict(schema=schema,held_out_ranks=target,training_ranks=counts,partition_seeds=seeds,
        rank_samples=len(rows),identity=identity,sources=sources,features=FEATURES[:features],phases=PHASES,
        tail_weight=4 if guarded else 1,tail_fraction=.1 if guarded else 0,
        residual_envelope=dict(over=over_error,under=under_error),feature_ranges=ranges,
        ridge=1e-4,validation=validation,model_sha256=hashlib.sha256(contents.encode()).hexdigest(),
        limitations="Group-held-out empirical envelope, not a confidence interval or simultaneous guarantee. Feature box does not prove in-distribution. Target scale excluded. No CAD curvature, Netgen internal-size-field or guaranteed speedup.")
    return contents,manifest

def verify(path,input_sha,binary_sha,levels,refines,maxh,minh,threads,target):
    meta=json.loads(path.with_suffix(".json").read_text())
    if digest(path)!=meta["model_sha256"] or target!=meta["held_out_ranks"] or target in meta["training_ranks"]:
        raise ValueError("model hash or held-out identity mismatch")
    expected=[input_sha,binary_sha,str(levels),str(refines),maxh,minh,threads]
    for key,value in zip(IDENTITY,expected):
        actual=meta["identity"][key]
        equal=float(actual)==float(value) if key in ("maxh","minh") else actual==value
        if not equal: raise ValueError("model calibration mismatch: "+key)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration-root",type=Path)
    parser.add_argument("--target-ranks",type=int,nargs="+")
    parser.add_argument("--levels",type=int,required=True)
    parser.add_argument("--refines",type=int,required=True)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--require-v3",action="store_true")
    parser.add_argument("--verify",type=Path)
    parser.add_argument("--input-sha");parser.add_argument("--binary-sha")
    parser.add_argument("--maxh");parser.add_argument("--minh");parser.add_argument("--threads")
    args=parser.parse_args()
    if args.verify:
        verify(args.verify,args.input_sha,args.binary_sha,args.levels,args.refines,args.maxh,args.minh,args.threads,args.target_ranks[0]);return
    if not args.calibration_root or not args.target_ranks or not args.output:
        parser.error("training requires calibration root, target ranks, output")
    args.output.mkdir(parents=True,exist_ok=True)
    report=["# 冻结模型与内部验证", "", "目标进程规模的样本完全排除；表中误差来自训练规模之间的再次留出。",
            "尚未运行目标规模对照；WAPE 是绝对误差之和 / 实际耗时之和。", "",
            "误差包络采用整组留出预测的最坏双侧相对残差，不是置信区间。新增特征和尾部权重需要用新批次验证。", "",
            "WAPE 汇总全部留出样本；最慢进程误差取各(规模,种子)组中绝对值最差者，最慢1%召回先组内计算再等权平均。各组明细在 JSON 中。", "",
            "| 目标规模 | 留出分组 | 计算 WAPE | 原8特征 WAPE | 单间距 WAPE | 最差组最慢误差 | 组均最慢1%召回 |", "|---:|---|---:|---:|---:|---:|---:|"]
    for target in args.target_ranks:
        contents,meta=train(args.calibration_root,target,args.levels,args.refines)
        if args.require_v3 and meta["schema"]!="mesh_phase_v3":
            raise ValueError("v3 evaluation needs new multi-seed calibration; old eight-feature samples remain archival")
        path=args.output/f"p{target}.model"
        # A resumed evaluation may already reference these exact immutable files.
        manifest_path=path.with_suffix(".json")
        if path.exists() and (path.read_text()!=contents or not manifest_path.exists() or
                              json.loads(manifest_path.read_text())!=meta):
            raise ValueError("refusing to replace a frozen model; use a new RUN_ROOT")
        path.write_text(contents)
        manifest_path.write_text(json.dumps(meta,indent=2)+"\n")
        for row in meta["validation"]:
            m=row["phase_model"];control=row["mean_spacing_control"]
            report.append(f"| {target} | {row['group_kind']}={row['group']} | {m['compute_wape']:.2%} | {row['v2_control']['compute_wape']:.2%} | {control['compute_wape']:.2%} | {m['worst_group_slowest_relative_error']:+.2%} | {m['slowest_one_percent_recall']:.2%} |")
        report.append(f"\n目标 {target}：相对高估包络 {meta['residual_envelope']['over']:.2%}，低估包络 {meta['residual_envelope']['under']:.2%}。\n")
    (args.output/"MODEL_REPORT.md").write_text("\n".join(report)+"\n")
    print(f"Frozen models: {args.output}; inspect MODEL_REPORT.md before interpreting evaluation results")

if __name__=="__main__":
    try: main()
    except (ValueError,KeyError,OSError) as error: raise SystemExit(str(error))
