#!/usr/bin/env python3
"""Focused analysis of research_1 profiles; standard library only."""
import argparse
import csv
import json
import math
import statistics as st
from collections import defaultdict
from pathlib import Path

COMPUTE = ("part_face_create", "surface_refine", "local_volume_mesh",
           "volume_refine", "adjacency_build", "vertex_numbering_local")

def mean(x):
    return st.fmean(x) if x else 0.0

def corr(a, b):
    if len(a) < 2:
        return None
    av, bv = mean(a), mean(b)
    xx = sum((x-av)**2 for x in a)
    yy = sum((y-bv)**2 for y in b)
    return sum((x-av)*(y-bv) for x, y in zip(a, b))/math.sqrt(xx*yy) if xx*yy else None

def seconds(row, name):
    return row["stages"].get(name, {}).get("seconds", 0.0)

def inspect(path):
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    if not rows:
        raise ValueError("empty rank profiles")
    n = rows[0]["ranks"]
    if len(rows) != n or sorted(r["rank"] for r in rows) != list(range(n)):
        raise ValueError("missing or duplicate rank rows")
    metadata = rows[0]["metadata"]
    for r in rows:
        if r["ranks"] != n or r["repeat"] != rows[0]["repeat"] or r["metadata"] != metadata:
            raise ValueError("inconsistent run metadata")
        if not math.isfinite(r["metrics"]["core_seconds"]) or r["metrics"]["core_seconds"] <= 0:
            raise ValueError("invalid core time")
    if metadata.get("core_only") != "true":
        raise ValueError("performance comparison requires core_only=true")
    compute = [sum(seconds(r, stage) for stage in COMPUTE) for r in rows]
    waiting = [seconds(r, "vertex_count_pre_collective_wait") for r in rows]
    tets = [r["metrics"].get("local_volume_elements_before_adjacency", 0) for r in rows]
    predicted = [r["metrics"].get("predicted_cost_after", 0) for r in rows]
    names = sorted({name for r in rows for name in r["stages"]})
    detail = []
    for name in names:
        times = [seconds(r, name) for r in rows]
        detail.append(dict(stage=name, mean_seconds=mean(times), max_seconds=max(times),
                           calls_sum=sum(r["stages"].get(name, {}).get("calls", 0) for r in rows)))
    face_bytes = sum(s.get("receive_bytes", 0) for r in rows for name, s in r["stages"].items()
                     if name == "face_allgatherv" or name.startswith("face_sparse_"))
    split = metadata["timing_mode"] == "split"
    result = dict(algorithm=metadata["algorithm"], timing=metadata["timing_mode"], ranks=n,
                  repeat=rows[0]["repeat"], core_seconds=max(r["metrics"]["core_seconds"] for r in rows),
                  face_pipeline_seconds=max(seconds(r, "face_pipeline_total") for r in rows),
                  face_exchange_seconds=max(sum(s["seconds"] for name, s in r["stages"].items()
                      if name in ("face_allgatherv", "face_sparse_match", "face_sparse_directory", "face_sparse_deliver")) for r in rows),
                  vertex_wait_seconds=max(waiting) if split else None,
                  compute_imbalance=max(compute)/mean(compute) if mean(compute) else None,
                  element_imbalance=max(tets)/mean(tets) if mean(tets) else None,
                  volume_elements_sum=sum(tets), face_payload_receive_bytes=face_bytes,
                  predicted_actual_correlation=corr(predicted, compute),
                  compute_wait_correlation=corr(compute, waiting) if split else None,
                  # Root-only global partition diagnostics.
                  cut_before=max(r["metrics"].get("partition_cut_before", 0) for r in rows),
                  cut_after=max(r["metrics"].get("partition_cut_after", 0) for r in rows),
                  partition_moves=max(r["metrics"].get("partition_moves", 0) for r in rows))
    return result, detail

def write_csv(path, rows):
    if not rows:
        return
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    runs, stage_rows, errors = [], [], []
    for path in sorted(args.root.rglob("rank_profiles.jsonl")):
        if not (path.parent/"SUCCESS").exists():
            errors.append(f"Incomplete run: {path.parent}")
            continue
        try:
            result, details = inspect(path)
            if result["repeat"] <= 0:
                continue  # Explicit warmups; no subtraction/estimated timing.
            runs.append(result)
            for detail in details:
                stage_rows.append({k: result[k] for k in ("algorithm", "timing", "ranks", "repeat")} | detail)
        except (ValueError, KeyError, TypeError) as error:
            errors.append(f"{path}: {error}")
    present_runs = {(r["algorithm"], r["timing"], r["ranks"], r["repeat"]) for r in runs}
    for plan_path in args.root.rglob("plan.json"):
        plan = json.loads(plan_path.read_text())
        for algorithm in plan["algorithms"]:
            for timing in plan["timings"]:
                missing = [rep for rep in range(1, plan["repeats"]+1)
                           if (algorithm, timing, plan["ranks"], rep) not in present_runs]
                if missing:
                    errors.append(f"Missing measured repeats: p{plan['ranks']} {algorithm}/{timing}: {missing}")
    groups = defaultdict(list)
    for run in runs:
        groups[run["algorithm"], run["timing"], run["ranks"]].append(run)
    summaries = []
    for (algorithm, timing, ranks), group in sorted(groups.items()):
        values = [r["core_seconds"] for r in group]
        summary = dict(algorithm=algorithm, timing=timing, ranks=ranks, successful_repeats=len(group),
                       core_median=st.median(values), core_cv=st.stdev(values)/mean(values) if len(values)>1 else None)
        for name in runs[0]:
            if name in ("algorithm", "timing", "ranks", "repeat", "core_seconds"):
                continue
            present = [r[name] for r in group if r[name] is not None]
            summary[name+"_median"] = st.median(present) if present else None
        base = groups.get(("baseline", timing, ranks))
        summary["speedup_vs_baseline"] = st.median(r["core_seconds"] for r in base)/summary["core_median"] if base else None
        summaries.append(summary)
    # Failures are retained even when the driver continued to later experiments.
    for path in args.root.rglob("run_status.tsv"):
        with path.open() as f:
            latest = {}
            for row in csv.DictReader(f, delimiter="\t"):
                latest[row["directory"]] = row
            errors.extend(f"Failed run (exit {r['exit_code']}): {r['directory']}" for r in latest.values() if r["exit_code"] != "0")
    requested = args.root/"requested_process_counts.txt"
    if requested.exists():
        present = {r["ranks"] for r in runs}
        errors.extend(f"No successful measured runs for p{n}" for n in map(int, requested.read_text().split()) if n not in present)
    output = args.root/"analysis"
    output.mkdir(exist_ok=True)
    write_csv(output/"runs.csv", runs)
    write_csv(output/"stages.csv", stage_rows)
    write_csv(output/"summary.csv", summaries)
    (output/"issues.txt").write_text("\n".join(errors)+( "\n" if errors else ""))
    print(f"Analyzed {len(runs)} measured runs, {len(errors)} incomplete/failed items. {output}")
    print("Compare natural and split separately. Changed partitions can change Netgen mesh size; check volume_elements_sum and mesh quality.")
    if not runs:
        raise SystemExit(1)

if __name__ == "__main__":
    main()
