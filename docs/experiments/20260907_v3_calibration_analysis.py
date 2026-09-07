#!/usr/bin/env python3
"""Reproduce the v3 calibration diagnosis; never exports a deployable model.

This batch's seeds have identical per-rank features. Treat their nine timings
as repeats, and perform only leave-process-count-out feature ablations. The
production training guard is checked and remains intact. Standard library only.
"""
import argparse
import csv
import gzip
import hashlib
import json
import math
import runpy
import statistics as st
from collections import defaultdict
from pathlib import Path

DATA_COMMIT = "6e9eb64c6170683c21f30992836d7392c03d7b64"
SOURCE_COMMIT = "409bac26f4ba1eb00ab4c993d7801a72914175c3"
BATCH = "strong_scaling_results/mesh_algorithms_20260907-203522"
FIT_SHA256 = "e11c17e22b6c719ecf23e46623cd33b80211649a7347d91d9a1b912c99dbd49b"
SAMPLE_BLOBS = {
    1024: "5dd5c498efc3dbad28dea5c4c2a2024e4d64d704",
    2048: "6aecc37cb93eb28786480019af72753c3435fa76",
    4096: "ca095c00261f2831d6c631b2273d85c45daa3358",
    8192: "451add72c65449c721d6f44bb7ce6298286eddea",
}
SEEDS = [-1, 17, 41]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_csv(path, delimiter=","):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def cv(values):
    return st.pstdev(values) / st.fmean(values)


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=repo / BATCH)
    parser.add_argument("--fit-script", type=Path,
                        default=repo / "strong_scaling/fit_cost_model.py")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(hashlib.sha256(args.fit_script.read_bytes()).hexdigest() == FIT_SHA256,
            "Use fit_cost_model.py from the recorded source commit")
    require(not (args.data / "analysis/issues.txt").read_text().strip(),
            "Aggregate analysis reported issues")
    module = runpy.run_path(str(args.fit_script))
    try:
        module["load_samples"](args.data, 8192, 3, 3)
    except ValueError as exc:
        guard_error = str(exc)
        require(guard_error == "partition seeds produced identical feature multisets; need distinct partitions",
                "Unexpected production training rejection: " + guard_error)
    else:
        raise ValueError("Expected the unchanged production loader to reject duplicate partitions")

    aggregated, integrity, sources, identities = [], [], [], set()
    for p, expected_blob in SAMPLE_BLOBS.items():
        folder = args.data / f"p{p}"
        require(not (folder / "analysis/issues.txt").read_text().strip(),
                f"p{p} analysis reported issues")
        path = folder / "analysis/model_samples.csv.gz"
        payload = path.read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(payload)).encode() + b"\0" + payload).hexdigest()
        require(blob == expected_blob, f"Source file changed: p{p}")
        sources.append(dict(path=f"p{p}/analysis/model_samples.csv.gz", git_blob=blob,
                            sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload)))
        status = read_csv(folder / "run_status.tsv", "\t")
        require(len(status) == 12 and all(r["exit_code"] == "0" for r in status),
                f"p{p} has incomplete/failed executions")
        require({(int(r["partition_seed"]), int(r["repeat"])) for r in status}
                == {(s, n) for s in SEEDS for n in range(4)}, "Unexpected execution matrix")
        groups, seen = defaultdict(list), set()
        with gzip.open(path, "rt", newline="") as stream:
            for raw in csv.DictReader(stream):
                key = tuple(int(raw[k]) for k in ("partition_seed", "rank", "repeat"))
                require(key not in seen, "Duplicate measurement key")
                seen.add(key)
                require(int(raw["ranks"]) == p and raw["algorithm"] == "sparse"
                        and raw["EXPERIMENT_STAGE"] == "calibration"
                        and raw["feature_schema"] == "mesh_phase_v3"
                        and raw["MESH_SOURCE_REVISION"] == SOURCE_COMMIT
                        and raw["MESH_MODEL_SHA256"] == "none", "Unexpected sample provenance")
                identity = tuple(raw[k] for k in module["IDENTITY"])
                identities.add(identity)
                x = [float(raw[f"x{i}"]) for i in range(14)]
                y = [float(raw[f"y{i}"]) for i in range(4)]
                require(all(math.isfinite(v) and v >= 0 for v in x + y)
                        and x[0] == 1 and sum(y) > 0, "Invalid feature/time")
                groups[key[1]].append(dict(x=x, y=y, host=raw["processor_name"]))
        require(seen == {(s, r, n) for s in SEEDS for r in range(p) for n in (1, 2, 3)},
                "Incomplete formal rank samples")
        group_rows, remesh_cvs, host_counts = [], [], []
        for rank, repeats in sorted(groups.items()):
            require(all(r["x"] == repeats[0]["x"] for r in repeats),
                    "This archive diagnosis assumes identical X across all seeds/repeats")
            group_rows.append(dict(ranks=p, rank=rank, seed=-1, x=repeats[0]["x"],
                                   y=[st.median(r["y"][i] for r in repeats) for i in range(4)]))
            remesh_cvs.append(cv([r["y"][1] for r in repeats]))
            host_counts.append(len({r["host"] for r in repeats}))
        aggregated.extend(group_rows)
        coarse = [r["x"][7] for r in group_rows]
        slowest = max(group_rows, key=lambda r: sum(r["y"]))
        integrity.append(dict(
            ranks=p, formal_runs=9, warmups=3, raw_rows=len(seen), rank_rows=p,
            distinct_feature_vectors=len({tuple(r["x"]) for r in group_rows}),
            identical_features_across_seed_and_repeat=True,
            effective_feature_partitions=1,
            hosts=len({r["host"] for repeats in groups.values() for r in repeats}),
            hosts_per_rank_range=[min(host_counts), max(host_counts)],
            coarse_cells_sum=sum(coarse), coarse_cells_mean=st.fmean(coarse),
            coarse_cells_range=[min(coarse), max(coarse)],
            remesh_repeat_cv_median=st.median(remesh_cvs),
            remesh_repeat_cv_p95=sorted(remesh_cvs)[math.ceil(.95 * p) - 1],
            slowest_rank=slowest["rank"], slowest_compute_seconds=sum(slowest["y"]),
            slowest_remesh_seconds=slowest["y"][1],
            slowest_coarse_cells=slowest["x"][7],
            remesh_fraction_of_summed_compute=sum(r["y"][1] for r in group_rows)
                / sum(sum(r["y"]) for r in group_rows)))
    require(len(identities) == 1, "Mixed executable, geometry or mesh parameters")
    identity = dict(zip(module["IDENTITY"], next(iter(identities))))
    require(identity["numlevels"] == identity["numrefine"] == "3"
            and identity["omp_num_threads"] == "1", "Unexpected mesh parameters")

    validation = []
    for p in SAMPLE_BLOBS:
        train = [r for r in aggregated if r["ranks"] != p]
        test = [r for r in aggregated if r["ranks"] == p]
        result = dict(target_ranks=p, training_ranks=sorted({r["ranks"] for r in train}), models={})
        for name, columns, tail in [("v2_control", tuple(range(8)), False),
                                     ("v3_unweighted", None, False),
                                     ("v3_tail", None, True)]:
            fitted = module["fit"](train, columns, tail_weighted=tail)
            result["models"][name] = dict(score=module["score"](fitted, test),
                                          diagnostic_coefficients=fitted)
        validation.append(result)

    runs = read_csv(args.data / "analysis/runs.csv")
    require(len(runs) == 36 and all(r["algorithm"] == "sparse" and r["timing"] == "natural"
                                  for r in runs), "Unexpected formal run summary")
    require(all(r["partition_adopted"] == r["partition_moves"] == r["partition_proposed_moves"] == "0"
                for r in runs), "Balancing was unexpectedly exercised")
    summary = read_csv(args.data / "analysis/summary.csv")
    require(len(summary) == 12 and all(r["successful_repeats"] == "3" for r in summary),
            "Incomplete grouped summary")
    stages = read_csv(args.data / "analysis/stages.csv")
    require(len(stages) == 1080, "Unexpected stage summary row count")
    selected_summary_keys = [
        "algorithm", "timing", "ranks", "partition_seed", "successful_repeats",
        "core_median", "core_cv", "face_pipeline_seconds_median", "face_exchange_seconds_median",
        "compute_imbalance_median", "element_imbalance_median", "volume_elements_sum_median",
        "face_payload_receive_bytes_median", "compute_max_seconds_median", "compute_mean_seconds_median",
        "vertex_arrival_spread_median", "metis_partition_seconds_median",
        "partition_adopted_median", "partition_moves_median", "partition_proposed_moves_median"]
    selected_stages = ["metis_seed", "partition_distribution", "partition_cost_setup",
                       "partition_cost_balance", "local_volume_mesh", "volume_refine"]
    stage_summary = []
    for p in SAMPLE_BLOBS:
        for name in selected_stages:
            measured = [r for r in stages if r["ranks"] == str(p)
                        and r["partition_seed"] == "-1" and r["stage"] == name]
            require(len(measured) == 3, "Incomplete default-seed stage measurements")
            stage_summary.append(dict(ranks=p, stage=name,
                mean_seconds_median=st.median(float(r["mean_seconds"]) for r in measured),
                max_seconds_median=st.median(float(r["max_seconds"]) for r in measured)))
    output = dict(
        diagnostic_only=True, deployable_model=False,
        repository="1482908163/test_code_expansion", data_commit=DATA_COMMIT,
        source_commit=SOURCE_COMMIT, batch=BATCH, fit_script_sha256=FIT_SHA256,
        methodology="Per-rank phase median over 9 repeats with exactly identical X; train on other 3 P groups; no seed holdout, no hyperparameter search, no production model export",
        cv_definition="population standard deviation divided by mean; p95 is nearest-rank quantile",
        production_loader_error=guard_error, identity=identity,
        sources=sources, integrity=integrity, validation=validation,
        grouped_measured_summary=[{k: r[k] for k in selected_summary_keys} for r in summary],
        p8192_seed17_runs=[r for r in runs if r["ranks"] == "8192" and r["partition_seed"] == "17"],
        measured_seed_default_stages=stage_summary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n")
    print(f"Saved diagnostic only: {args.output}")
    print("36 formal runs complete; duplicate partition features block production v3 training.")
    for item in validation:
        s = item["models"]["v3_tail"]["score"]
        print(item["target_ranks"], {k: s[k] for k in (
            "compute_wape", "actual_slowest_rank_relative_error", "slowest_one_percent_recall")})


if __name__ == "__main__":
    main()
