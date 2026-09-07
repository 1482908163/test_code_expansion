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
            "physical_boundary_faces", "coarse_cells"]
PHASES = ["surface", "remesh", "refine", "adjacency_numbering"]
IDENTITY = ["MESH_INPUT_SHA256", "MESH_BINARY_SHA256", "numlevels", "numrefine",
            "maxh", "minh", "omp_num_threads"]

def digest(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda:f.read(1024*1024), b""):
            h.update(block)
    return h.hexdigest()

def fit(rows, columns=tuple(range(8))):
    """Nonnegative ridge least squares on RMS-scaled features (lambda=1e-4).

    Each process-count group has equal total weight; an 8192-rank sample must
    not outweigh a 1024-rank sample eightfold. No evaluation data/hyperparameter search.
    """
    counts=defaultdict(int)
    for r in rows: counts[r["ranks"]]+=1
    weight=[1/(len(counts)*counts[r["ranks"]]) for r in rows]
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
        raw=[0.0]*8
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
    return result

def load_samples(root,target,levels,refines):
    paths=sorted(root.glob("p*/analysis/model_samples.csv.gz"))
    if not paths: raise ValueError("no phase samples: run calibration with the new executable first")
    grouped=defaultdict(list); identities=set(); sources=[]
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
                if r["feature_schema"]!="mesh_phase_v2": raise ValueError("wrong feature schema")
                if int(r["ranks"])!=p: raise ValueError("sample rank count differs from directory")
                if int(r["numlevels"])!=levels or int(r["numrefine"])!=refines:
                    raise ValueError("calibration levels/refines differ from evaluation")
                key=(p,int(r["rank"])); repeat=int(r["repeat"])
                if repeat<=0: continue
                if (key,repeat) in seen: raise ValueError("duplicate calibration sample")
                seen.add((key,repeat))
                identity=tuple(r[k] for k in IDENTITY)
                if "unset" in identity: raise ValueError("missing calibration provenance")
                identities.add(identity)
                x=[float(r[f"x{k}"]) for k in range(8)]
                y=[float(r[f"y{k}"]) for k in range(4)]
                if any(not math.isfinite(v) or v<0 for v in x+y) or sum(y)<=0:
                    raise ValueError("invalid calibration feature/time")
                grouped[key].append((x,y))
    if len(identities)!=1: raise ValueError("calibration must use one geometry, executable and parameter set")
    rows=[]
    for (p,rank), values in sorted(grouped.items()):
        if len(values)<2: raise ValueError("need at least two measured repeats per calibration rank")
        if any(x!=values[0][0] for x,y in values): raise ValueError("seed features changed across repeats")
        rows.append(dict(ranks=p,rank=rank,x=values[0][0],
                         y=[st.median(y[k] for x,y in values) for k in range(4)]))
    counts=sorted({r["ranks"] for r in rows})
    if len(counts)<2: raise ValueError("need at least two other process counts, excluding target")
    for p in counts:
        if [r["rank"] for r in rows if r["ranks"]==p]!=list(range(p)):
            raise ValueError("missing calibration ranks")
    return rows,dict(zip(IDENTITY,next(iter(identities)))),sources

def train(root,target,levels,refines):
    rows,identity,sources=load_samples(root,target,levels,refines)
    counts=sorted({r["ranks"] for r in rows})
    validation=[]
    for p in counts:
        train_rows=[r for r in rows if r["ranks"]!=p]
        held_out=[r for r in rows if r["ranks"]==p]
        validation.append(dict(ranks=p,phase_model=score(fit(train_rows),held_out),
                               mean_spacing_control=score(fit(train_rows,(0,2)),held_out)))
    model=fit(rows)
    contents=f"mesh_phase_v2 {levels} {refines} {target} 8 4\n"+"".join(
        " ".join(format(v,".17g") for v in phase)+"\n" for phase in model)
    manifest=dict(schema="mesh_phase_v2",held_out_ranks=target,training_ranks=counts,
        rank_samples=len(rows),identity=identity,sources=sources,features=FEATURES,phases=PHASES,
        ridge=1e-4,validation=validation,model_sha256=hashlib.sha256(contents.encode()).hexdigest(),
        limitations="Internal cross-scale validation only; evaluate on held-out process count. No CAD curvature features or guaranteed speedup.")
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
            "| 目标规模 | 内部留出规模 | 计算 WAPE | 单间距对照 WAPE | 最慢进程预测误差 |", "|---:|---:|---:|---:|---:|"]
    for target in args.target_ranks:
        contents,meta=train(args.calibration_root,target,args.levels,args.refines)
        path=args.output/f"p{target}.model"
        # A resumed evaluation may already reference these exact immutable files.
        if path.exists() and path.read_text()!=contents:
            raise ValueError("refusing to replace a frozen model; use a new RUN_ROOT")
        path.write_text(contents)
        path.with_suffix(".json").write_text(json.dumps(meta,indent=2)+"\n")
        for row in meta["validation"]:
            m=row["phase_model"];control=row["mean_spacing_control"]
            report.append(f"| {target} | {row['ranks']} | {m['compute_wape']:.2%} | {control['compute_wape']:.2%} | {m['actual_slowest_rank_relative_error']:+.2%} |")
    (args.output/"MODEL_REPORT.md").write_text("\n".join(report)+"\n")
    print(f"Frozen models: {args.output}; inspect MODEL_REPORT.md before interpreting evaluation results")

if __name__=="__main__":
    try: main()
    except (ValueError,KeyError,OSError) as error: raise SystemExit(str(error))
