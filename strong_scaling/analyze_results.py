#!/usr/bin/env python3
"""Aggregate profiler runs and calculate strong-scaling efficiency.

Only the Python standard library is required. The script scans each run and
stage CSV below one experiment directory, takes the median across repetitions,
and writes a compact analysis directory.
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Iterable


IDENTITY_FIELDS = {"experiment", "processes", "repeat", "timestamp", "core_only"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="汇总强扩展实验结果")
    parser.add_argument("experiment_dir", type=Path, help="实验目录，例如 profiles/wholewall_l2")
    parser.add_argument("--output", type=Path, default=None, help="分析结果目录，默认 <实验目录>/analysis")
    return parser.parse_args()


def read_runs(experiment_dir: Path) -> list[dict[str, str]]:
    run_states: dict[tuple[int, int], str] = {}
    status_dir = experiment_dir / "status"
    if status_dir.is_dir():
        for status_path in sorted(status_dir.glob("*.tsv")):
            with status_path.open(newline="", encoding="utf-8") as stream:
                for status_row in csv.DictReader(stream, delimiter="\t"):
                    try:
                        key = (
                            int(status_row["processes"]),
                            int(status_row["repeat"]),
                        )
                    except (KeyError, TypeError, ValueError):
                        continue
                    run_states[key] = status_row.get("state", "")

    runs: list[dict[str, str]] = []
    for path in sorted(experiment_dir.rglob("run_summary.csv")):
        if "analysis" in path.parts:
            continue
        with path.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != 1:
            raise ValueError(f"{path} should contain exactly one data row")
        row = rows[0]
        try:
            run_key = (int(row["processes"]), int(row["repeat"]))
        except (KeyError, TypeError, ValueError):
            run_key = None
        if run_key is not None and run_states.get(run_key, "COMPLETED") != "COMPLETED":
            continue
        row["source_file"] = str(path)
        runs.append(row)
    if not runs:
        raise FileNotFoundError(f"No run_summary.csv found below {experiment_dir}")
    return runs


def read_stage_runs(runs: list[dict[str, str]]) -> list[dict[str, str]]:
    stage_runs: list[dict[str, str]] = []
    for run in runs:
        path = Path(run["source_file"]).with_name("stages.csv")
        if not path.is_file():
            raise FileNotFoundError(f"Missing stage data next to {run['source_file']}: {path}")
        with path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                row["experiment"] = run["experiment"]
                row["processes"] = run["processes"]
                row["repeat"] = run["repeat"]
                row["cache_valid"] = (
                    "1"
                    if numeric(run, "cache_available_ranks") >= int(run["processes"])
                    else "0"
                )
                row["io_valid"] = (
                    "1"
                    if numeric(run, "io_counter_available_ranks") >= int(run["processes"])
                    else "0"
                )
                row["source_file"] = str(path)
                stage_runs.append(row)
    return stage_runs


def numeric(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    if value == "":
        return math.nan
    return float(value)


def finite(values: Iterable[float]) -> list[float]:
    return [value for value in values if math.isfinite(value)]


def median_field(rows: list[dict[str, str]], key: str) -> float:
    values = finite(numeric(row, key) for row in rows)
    return statistics.median(values) if values else math.nan


def coefficient_of_variation(values: Iterable[float]) -> float:
    usable = finite(values)
    if len(usable) < 2:
        return 0.0
    mean = statistics.mean(usable)
    return 100.0 * statistics.stdev(usable) / mean if mean else 0.0


def format_number(value: float, width: int = 10, precision: int = 3) -> str:
    if not math.isfinite(value):
        return f"{'N/A':>{width}}"
    return f"{value:>{width}.{precision}f}"


def read_plan_value(experiment_dir: Path, key: str, default: str = "unknown") -> str:
    path = experiment_dir / "run_plan.txt"
    if not path.is_file():
        return default
    prefix = f"{key}="
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith(prefix):
                return line[len(prefix) :].strip()
    return default


def read_cache_states(experiment_dir: Path) -> dict[tuple[int, int], str]:
    """Read the effective cache label written after cache preparation.

    Older result directories have no cache_state column.  Their first run is
    therefore handled as a cold candidate rather than silently promoted to a
    controlled cold run.
    """

    states: dict[tuple[int, int], str] = {}
    status_dir = experiment_dir / "status"
    if not status_dir.is_dir():
        return states
    for path in sorted(status_dir.glob("*.tsv")):
        with path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                state = row.get("cache_state", "")
                if state not in {"cold", "cold_candidate", "warm"}:
                    continue
                try:
                    key = (int(row["processes"]), int(row["repeat"]))
                except (KeyError, TypeError, ValueError):
                    continue
                states[key] = state
    return states


def main() -> None:
    args = parse_args()
    experiment_dir = args.experiment_dir.resolve()
    output_dir = (args.output or experiment_dir / "analysis").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    runs = read_runs(experiment_dir)
    experiments = {row["experiment"] for row in runs}
    modes = {row["core_only"] for row in runs}
    if len(experiments) != 1:
        raise ValueError(f"Mixed experiment names found: {sorted(experiments)}")
    if len(modes) != 1:
        raise ValueError("Core-only and full-I/O runs must be analyzed separately")
    stage_runs = read_stage_runs(runs)
    grouped: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in runs:
        grouped[int(row["processes"])].append(row)

    process_counts = sorted(grouped)
    base_processes = process_counts[0]
    base_time = median_field(grouped[base_processes], "total_wall_max_s")

    numeric_fields = sorted(
        set().union(*(row.keys() for row in runs))
        - IDENTITY_FIELDS
        - {"source_file"}
    )
    summary_rows: list[dict[str, float | int | str]] = []
    for processes in process_counts:
        rows = grouped[processes]
        result: dict[str, float | int | str] = {
            "experiment": rows[0]["experiment"],
            "processes": processes,
            "repetitions": len(rows),
            "core_only": rows[0]["core_only"],
        }
        for key in numeric_fields:
            result[key] = median_field(rows, key)

        cache_rows = [
            row
            for row in rows
            if numeric(row, "cache_available_ranks") >= processes
        ]
        result["cache_valid_repetitions"] = len(cache_rows)
        for key in (
            "cache_references_total",
            "cache_misses_total",
            "cache_miss_rate_percent",
            "instructions_total",
            "cycles_total",
            "ipc",
        ):
            result[key] = median_field(cache_rows, key) if cache_rows else math.nan

        io_rows = [
            row
            for row in rows
            if numeric(row, "io_counter_available_ranks") >= processes
        ]
        result["io_valid_repetitions"] = len(io_rows)
        for key in (
            "logical_read_total_bytes",
            "logical_write_total_bytes",
            "physical_read_total_bytes",
            "physical_write_total_bytes",
        ):
            result[key] = median_field(io_rows, key) if io_rows else math.nan

        elapsed = float(result["total_wall_max_s"])
        speedup = base_time / elapsed if elapsed > 0.0 else math.nan
        ideal_speedup = processes / base_processes
        result["speedup"] = speedup
        result["ideal_speedup"] = ideal_speedup
        result["parallel_efficiency_percent"] = 100.0 * speedup / ideal_speedup
        result["total_time_cv_percent"] = coefficient_of_variation(
            numeric(row, "total_wall_max_s") for row in rows
        )
        summary_rows.append(result)

    page_cache_policy = read_plan_value(experiment_dir, "page_cache_policy", "observe")
    recorded_cache_states = read_cache_states(experiment_dir)
    cold_rows_by_process: dict[int, list[dict[str, str]]] = {}
    warm_rows_by_process: dict[int, list[dict[str, str]]] = {}
    for processes, rows in grouped.items():
        cold_rows_by_process[processes] = [
            row for row in rows if int(row["repeat"]) == 1
        ]
        warm_rows_by_process[processes] = [
            row for row in rows if int(row["repeat"]) > 1
        ]

    cold_base_time = median_field(
        cold_rows_by_process[base_processes], "total_wall_max_s"
    )
    warm_base_time = median_field(
        warm_rows_by_process[base_processes], "total_wall_max_s"
    )
    cold_warm_rows: list[dict[str, float | int | str]] = []
    for processes in process_counts:
        cold_rows = cold_rows_by_process[processes]
        warm_rows = warm_rows_by_process[processes]
        cold_time = median_field(cold_rows, "total_wall_max_s")
        warm_time = median_field(warm_rows, "total_wall_max_s")
        ideal_speedup = processes / base_processes
        cold_speedup = (
            cold_base_time / cold_time
            if math.isfinite(cold_base_time) and cold_time > 0.0
            else math.nan
        )
        warm_speedup = (
            warm_base_time / warm_time
            if math.isfinite(warm_base_time) and warm_time > 0.0
            else math.nan
        )
        cold_cache_rows = [
            row
            for row in cold_rows
            if numeric(row, "cache_available_ranks") >= processes
        ]
        warm_cache_rows = [
            row
            for row in warm_rows
            if numeric(row, "cache_available_ranks") >= processes
        ]
        cold_io_rows = [
            row
            for row in cold_rows
            if numeric(row, "io_counter_available_ranks") >= processes
        ]
        warm_io_rows = [
            row
            for row in warm_rows
            if numeric(row, "io_counter_available_ranks") >= processes
        ]
        fallback_cold_state = (
            "cold" if page_cache_policy == "evict-first" else "cold_candidate"
        )
        effective_cold_states = {
            recorded_cache_states.get(
                (processes, int(row["repeat"])), fallback_cold_state
            )
            for row in cold_rows
        }
        cold_cache_state = (
            "cold" if effective_cold_states == {"cold"} else "cold_candidate"
        )
        cold_arrival_wait_fraction = median_field(
            cold_rows, "arrival_wait_fraction_max_percent"
        )
        warm_arrival_wait_fraction = median_field(
            warm_rows, "arrival_wait_fraction_max_percent"
        )
        cold_arrival_wait = (
            median_field(cold_rows, "synchronization_max_s")
            if math.isfinite(cold_arrival_wait_fraction)
            else math.nan
        )
        warm_arrival_wait = (
            median_field(warm_rows, "synchronization_max_s")
            if math.isfinite(warm_arrival_wait_fraction)
            else math.nan
        )
        cold_warm_rows.append(
            {
                "experiment": cold_rows[0]["experiment"] if cold_rows else warm_rows[0]["experiment"],
                "processes": processes,
                "page_cache_policy": page_cache_policy,
                "cold_cache_state": cold_cache_state,
                "cold_repetitions": len(cold_rows),
                "warm_repetitions": len(warm_rows),
                "cold_total_wall_max_s": cold_time,
                "warm_total_wall_max_s": warm_time,
                "cold_over_warm": (
                    cold_time / warm_time
                    if cold_time > 0.0 and warm_time > 0.0
                    else math.nan
                ),
                "cold_penalty_percent": (
                    100.0 * (cold_time / warm_time - 1.0)
                    if cold_time > 0.0 and warm_time > 0.0
                    else math.nan
                ),
                "cold_speedup": cold_speedup,
                "warm_speedup": warm_speedup,
                "cold_parallel_efficiency_percent": 100.0 * cold_speedup / ideal_speedup,
                "warm_parallel_efficiency_percent": 100.0 * warm_speedup / ideal_speedup,
                "warm_total_time_cv_percent": coefficient_of_variation(
                    numeric(row, "total_wall_max_s") for row in warm_rows
                ),
                "cold_communication_max_s": median_field(cold_rows, "communication_max_s"),
                "warm_communication_max_s": median_field(warm_rows, "communication_max_s"),
                "cold_communication_fraction_max_percent": median_field(
                    cold_rows, "communication_fraction_max_percent"
                ),
                "warm_communication_fraction_max_percent": median_field(
                    warm_rows, "communication_fraction_max_percent"
                ),
                "cold_arrival_wait_max_s": cold_arrival_wait,
                "warm_arrival_wait_max_s": warm_arrival_wait,
                "cold_arrival_wait_fraction_max_percent": cold_arrival_wait_fraction,
                "warm_arrival_wait_fraction_max_percent": warm_arrival_wait_fraction,
                "cold_communication_plus_wait_fraction_max_percent": median_field(
                    cold_rows, "communication_plus_wait_fraction_max_percent"
                ),
                "warm_communication_plus_wait_fraction_max_percent": median_field(
                    warm_rows, "communication_plus_wait_fraction_max_percent"
                ),
                "cold_io_max_s": median_field(cold_rows, "io_max_s"),
                "warm_io_max_s": median_field(warm_rows, "io_max_s"),
                "cold_io_fraction_max_percent": median_field(
                    cold_rows, "io_fraction_max_percent"
                ),
                "warm_io_fraction_max_percent": median_field(
                    warm_rows, "io_fraction_max_percent"
                ),
                "cold_physical_read_total_bytes": median_field(
                    cold_io_rows, "physical_read_total_bytes"
                ),
                "warm_physical_read_total_bytes": median_field(
                    warm_io_rows, "physical_read_total_bytes"
                ),
                "cold_physical_write_total_bytes": median_field(
                    cold_io_rows, "physical_write_total_bytes"
                ),
                "warm_physical_write_total_bytes": median_field(
                    warm_io_rows, "physical_write_total_bytes"
                ),
                "cold_cache_miss_rate_percent": median_field(
                    cold_cache_rows, "cache_miss_rate_percent"
                ),
                "warm_cache_miss_rate_percent": median_field(
                    warm_cache_rows, "cache_miss_rate_percent"
                ),
                "cold_ipc": median_field(cold_cache_rows, "ipc"),
                "warm_ipc": median_field(warm_cache_rows, "ipc"),
            }
        )

    summary_fields = [
        "experiment",
        "processes",
        "repetitions",
        "core_only",
        "total_wall_max_s",
        "speedup",
        "ideal_speedup",
        "parallel_efficiency_percent",
        "total_time_cv_percent",
        "profile_coverage_percent",
        "unprofiled_max_s",
        "communication_fraction_max_percent",
        "arrival_wait_fraction_max_percent",
        "communication_plus_wait_fraction_max_percent",
        "communication_max_s",
        "synchronization_max_s",
        "io_fraction_max_percent",
        "io_max_s",
        "io_valid_repetitions",
        "io_counter_available_ranks",
        "cache_valid_repetitions",
        "cache_available_ranks",
        "cache_miss_rate_percent",
        "ipc",
        "minor_faults_total",
        "major_faults_total",
        "logical_read_total_bytes",
        "logical_write_total_bytes",
        "physical_read_total_bytes",
        "physical_write_total_bytes",
        "peak_rss_max_mib",
        "local_volume_elements_avg",
        "local_volume_elements_max",
        "volume_imbalance_max_over_avg",
        "setup_max_s",
        "compute_max_s",
        "postprocess_max_s",
    ]
    with (output_dir / "scaling_summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=summary_fields)
        writer.writeheader()
        for row in summary_rows:
            writer.writerow({key: row.get(key, "") for key in summary_fields})

    cold_warm_fields = list(cold_warm_rows[0].keys()) if cold_warm_rows else []
    with (output_dir / "cold_warm_summary.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=cold_warm_fields)
        writer.writeheader()
        writer.writerows(cold_warm_rows)

    raw_fields = sorted(set().union(*(row.keys() for row in runs)))
    with (output_dir / "all_runs.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=raw_fields)
        writer.writeheader()
        writer.writerows(runs)

    stage_grouped: dict[tuple[int, str], list[dict[str, str]]] = defaultdict(list)
    for row in stage_runs:
        stage_grouped[(int(row["processes"]), row["stage"])].append(row)

    stage_summary_rows: list[dict[str, float | int | str]] = []
    for (processes, stage), rows in sorted(stage_grouped.items()):
        cache_rows = [row for row in rows if row["cache_valid"] == "1"]
        io_rows = [row for row in rows if row["io_valid"] == "1"]
        stage_summary_rows.append(
            {
                "experiment": rows[0]["experiment"],
                "processes": processes,
                "repetitions": len(rows),
                "stage": stage,
                "category": rows[0]["category"],
                "time_avg_s": median_field(rows, "time_avg_s"),
                "time_max_s": median_field(rows, "time_max_s"),
                "max_over_avg": median_field(rows, "max_over_avg"),
                "max_percent_of_total": median_field(rows, "max_percent_of_total"),
                "cache_valid_repetitions": len(cache_rows),
                "cache_miss_rate_percent": (
                    median_field(cache_rows, "cache_miss_rate_percent")
                    if cache_rows
                    else math.nan
                ),
                "ipc": median_field(cache_rows, "ipc") if cache_rows else math.nan,
                "io_valid_repetitions": len(io_rows),
                "minor_faults_total": median_field(rows, "minor_faults_total"),
                "major_faults_total": median_field(rows, "major_faults_total"),
                "logical_read_total_bytes": (
                    median_field(io_rows, "rchar_total_bytes") if io_rows else math.nan
                ),
                "logical_write_total_bytes": (
                    median_field(io_rows, "wchar_total_bytes") if io_rows else math.nan
                ),
                "physical_read_total_bytes": (
                    median_field(io_rows, "physical_read_total_bytes") if io_rows else math.nan
                ),
                "physical_write_total_bytes": (
                    median_field(io_rows, "physical_write_total_bytes") if io_rows else math.nan
                ),
                "send_messages_total": median_field(rows, "send_messages_total"),
                "receive_messages_total": median_field(rows, "receive_messages_total"),
                "send_total_bytes": median_field(rows, "send_total_bytes"),
                "receive_total_bytes": median_field(rows, "receive_total_bytes"),
                "send_max_rank_bytes": median_field(rows, "send_max_rank_bytes"),
                "receive_max_rank_bytes": median_field(rows, "receive_max_rank_bytes"),
            }
        )

    stage_summary_fields = list(stage_summary_rows[0].keys()) if stage_summary_rows else []
    with (output_dir / "stage_scaling.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=stage_summary_fields)
        writer.writeheader()
        writer.writerows(stage_summary_rows)

    collective_pairs = [
        ("face_allgatherv", "face_pre_collective_wait", "face_allgatherv"),
        (
            "vertex_count_allgather",
            "vertex_count_pre_collective_wait",
            "vertex_count_allgather",
        ),
        (
            "element_count_allgather",
            "element_count_pre_collective_wait",
            "element_count_allgather",
        ),
        (
            "quality_reduce",
            "quality_reduce_pre_collective_wait",
            "quality_reduce",
        ),
    ]
    collective_split_rows: list[dict[str, float | int | str]] = []
    for processes in process_counts:
        for collective, wait_stage, execution_stage in collective_pairs:
            wait_rows = stage_grouped.get((processes, wait_stage), [])
            execution_rows = stage_grouped.get((processes, execution_stage), [])
            # A collective call from an older profiler is not an aligned
            # execution measurement unless its matching wait stage exists.
            if not wait_rows:
                continue
            cold_wait_rows = [row for row in wait_rows if int(row["repeat"]) == 1]
            warm_wait_rows = [row for row in wait_rows if int(row["repeat"]) > 1]
            cold_execution_rows = [
                row for row in execution_rows if int(row["repeat"]) == 1
            ]
            warm_execution_rows = [
                row for row in execution_rows if int(row["repeat"]) > 1
            ]
            cold_wait = median_field(cold_wait_rows, "time_max_s")
            warm_wait = median_field(warm_wait_rows, "time_max_s")
            cold_execution = median_field(cold_execution_rows, "time_max_s")
            warm_execution = median_field(warm_execution_rows, "time_max_s")
            observed = finite(
                [cold_wait, warm_wait, cold_execution, warm_execution]
            )
            if not observed or max(observed) <= 0.0:
                continue
            collective_split_rows.append(
                {
                    "experiment": runs[0]["experiment"],
                    "processes": processes,
                    "collective": collective,
                    "cold_arrival_wait_max_s": cold_wait,
                    "cold_aligned_execution_max_s": cold_execution,
                    "cold_wait_percent_of_pair": (
                        100.0 * cold_wait / (cold_wait + cold_execution)
                        if cold_wait + cold_execution > 0.0
                        else math.nan
                    ),
                    "warm_arrival_wait_max_s": warm_wait,
                    "warm_aligned_execution_max_s": warm_execution,
                    "warm_wait_percent_of_pair": (
                        100.0 * warm_wait / (warm_wait + warm_execution)
                        if warm_wait + warm_execution > 0.0
                        else math.nan
                    ),
                }
            )

    collective_split_fields = [
        "experiment",
        "processes",
        "collective",
        "cold_arrival_wait_max_s",
        "cold_aligned_execution_max_s",
        "cold_wait_percent_of_pair",
        "warm_arrival_wait_max_s",
        "warm_aligned_execution_max_s",
        "warm_wait_percent_of_pair",
    ]
    with (output_dir / "collective_split.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=collective_split_fields)
        writer.writeheader()
        writer.writerows(collective_split_rows)

    first = summary_rows[0]
    last = summary_rows[-1]
    collective_split_available = any(
        math.isfinite(float(row.get("arrival_wait_fraction_max_percent", math.nan)))
        for row in summary_rows
    )
    with (output_dir / "scaling_report.txt").open("w", encoding="utf-8") as output:
        output.write("Strong Scaling Summary (强扩展汇总)\n")
        output.write("=" * 138 + "\n")
        output.write(f"Experiment (实验): {first['experiment']}\n")
        output.write(f"Baseline (基准): {base_processes} processes, {base_time:.6f} s\n")
        output.write(
            "\n"
            f"{'P':>7} {'Runs':>6} {'Time(s)':>11} {'Speedup':>10} {'Eff(%)':>9} "
            f"{'Wait(%)':>10} {'Comm(%)':>10} {'I/O(%)':>9} {'Cover(%)':>9} {'CacheMiss(%)':>14} {'IPC':>8} "
            f"{'Load M/A':>10} {'RSS(MiB)':>11} {'CV(%)':>8}\n"
        )
        output.write("-" * 138 + "\n")
        for row in summary_rows:
            cache_available = int(row["cache_valid_repetitions"]) > 0
            cache_miss_rate = (
                float(row["cache_miss_rate_percent"]) if cache_available else math.nan
            )
            ipc = float(row["ipc"]) if cache_available else math.nan
            output.write(
                f"{int(row['processes']):>7} {int(row['repetitions']):>6}"
                f"{format_number(float(row['total_wall_max_s']), 11)}"
                f"{format_number(float(row['speedup']), 10)}"
                f"{format_number(float(row['parallel_efficiency_percent']), 9, 2)}"
                f"{format_number(float(row.get('arrival_wait_fraction_max_percent', math.nan)), 10, 2)}"
                f"{format_number(float(row['communication_fraction_max_percent']), 10, 2)}"
                f"{format_number(float(row['io_fraction_max_percent']), 9, 2)}"
                f"{format_number(float(row['profile_coverage_percent']), 9, 2)}"
                f"{format_number(cache_miss_rate, 14, 3)}"
                f"{format_number(ipc, 8, 3)}"
                f"{format_number(float(row['volume_imbalance_max_over_avg']), 10, 3)}"
                f"{format_number(float(row['peak_rss_max_mib']), 11, 2)}"
                f"{format_number(float(row['total_time_cv_percent']), 8, 2)}\n"
            )

        output.write("\nObserved change (首末规模变化)\n")
        output.write(
            f"  Parallel efficiency (并行效率): "
            f"{float(first['parallel_efficiency_percent']):.2f}% -> "
            f"{float(last['parallel_efficiency_percent']):.2f}%\n"
        )
        if collective_split_available:
            output.write(
                f"  Collective arrival-wait fraction (集合通信到达等待占比): "
                f"{float(first.get('arrival_wait_fraction_max_percent', math.nan)):.2f}% -> "
                f"{float(last.get('arrival_wait_fraction_max_percent', math.nan)):.2f}%\n"
            )
            output.write(
                f"  Aligned communication execution fraction (对齐后通信执行占比): "
                f"{float(first['communication_fraction_max_percent']):.2f}% -> "
                f"{float(last['communication_fraction_max_percent']):.2f}%\n"
            )
        else:
            output.write(
                "  Collective split (集合通信拆分): N/A（旧版结果）；"
                "旧版 Comm(%) 包含到达等待，不能解释为对齐后的通信执行。\n"
            )
        output.write(
            f"  I/O fraction (I/O 占比): "
            f"{float(first['io_fraction_max_percent']):.2f}% -> "
            f"{float(last['io_fraction_max_percent']):.2f}%\n"
        )
        if (
            int(first["cache_valid_repetitions"]) > 0
            and int(last["cache_valid_repetitions"]) > 0
        ):
            output.write(
                f"  Cache miss rate (缓存未命中率): "
                f"{float(first['cache_miss_rate_percent']):.3f}% -> "
                f"{float(last['cache_miss_rate_percent']):.3f}%\n"
            )
        else:
            output.write("  Cache miss rate (缓存未命中率): N/A（硬件计数器不可用）\n")
        output.write(
            f"  Volume load imbalance max/avg (体单元负载不均衡): "
            f"{float(first['volume_imbalance_max_over_avg']):.3f} -> "
            f"{float(last['volume_imbalance_max_over_avg']):.3f}\n"
        )
        if any(int(row["cache_valid_repetitions"]) < int(row["repetitions"]) for row in summary_rows):
            output.write(
                "\n说明：至少一次运行未请求硬件缓存计数，或有一个或多个进程的计数不可用；"
                "缓存指标中位数只使用全部进程计数均有效的运行。\n"
            )
        if any(int(row["io_valid_repetitions"]) < int(row["repetitions"]) for row in summary_rows):
            output.write(
                "说明：至少一次运行无法读取 /proc/self/io；其逻辑/物理 I/O 字节数记为 N/A，"
                "但 I/O 阶段计时仍然有效。\n"
            )

        output.write("\nFiles (文件)\n")
        output.write("  scaling_summary.csv  各进程数的中位数、加速比和并行效率\n")
        output.write("  stage_scaling.csv    各进程数逐阶段的中位数指标\n")
        output.write("  collective_split.csv 各集合通信的到达等待/对齐执行拆分\n")
        output.write("  collective_split_report.txt 集合通信拆分可读报告\n")
        output.write("  all_runs.csv         所有重复实验的原始总指标\n")
        output.write("  cold_warm_summary.csv  冷/热页缓存分离汇总\n")
        output.write("  cold_warm_report.txt   冷/热页缓存可读报告\n")
        output.write("  scaling_report.txt   本文件\n")

    with (output_dir / "cold_warm_report.txt").open("w", encoding="utf-8") as output:
        output.write("Cold/Warm Page-cache Summary (冷/热页缓存汇总)\n")
        output.write("=" * 123 + "\n")
        output.write(f"Experiment (实验): {runs[0]['experiment']}\n")
        output.write(f"Page-cache policy (页缓存策略): {page_cache_policy}\n")
        output.write(
            "Cold 使用 repeat 1；Warm 使用 repeat 2+ 的中位数。"
            "cold_candidate 表示没有成功清缓存的证据。POSIX_FADV_DONTNEED 是内核提示，"
            "是否真正冷启动应结合 PhysR 判断。\n\n"
        )
        output.write(
            f"{'P':>7} {'ColdState':>14} {'Cold(s)':>11} {'Warm(s)':>11} {'Penalty(%)':>12} "
            f"{'ColdEff(%)':>12} {'WarmEff(%)':>12} {'ColdPhysR(GiB)':>16} "
            f"{'WarmPhysR(GiB)':>16} {'WarmCV(%)':>11}\n"
        )
        output.write("-" * 123 + "\n")
        for row in cold_warm_rows:
            output.write(
                f"{int(row['processes']):>7}"
                f"{str(row['cold_cache_state']):>14}"
                f"{format_number(float(row['cold_total_wall_max_s']), 11)}"
                f"{format_number(float(row['warm_total_wall_max_s']), 11)}"
                f"{format_number(float(row['cold_penalty_percent']), 12, 2)}"
                f"{format_number(float(row['cold_parallel_efficiency_percent']), 12, 2)}"
                f"{format_number(float(row['warm_parallel_efficiency_percent']), 12, 2)}"
                f"{format_number(float(row['cold_physical_read_total_bytes']) / (1024**3), 16, 3)}"
                f"{format_number(float(row['warm_physical_read_total_bytes']) / (1024**3), 16, 3)}"
                f"{format_number(float(row['warm_total_time_cv_percent']), 11, 2)}\n"
            )

    with (output_dir / "collective_split_report.txt").open(
        "w", encoding="utf-8"
    ) as output:
        output.write("Collective Communication Split (集合通信拆分)\n")
        output.write("=" * 109 + "\n")
        output.write(
            "Wait 为集合通信前的到达对齐 Barrier；Exec 为随后原集合通信调用的执行时间。"
            "Exec 仍包含 MPI 算法、协议和内存复制，并非纯网络时间。"
            "旧版中没有匹配 Wait 阶段的集合通信不会列入本表。\n\n"
        )
        output.write(
            f"{'P':>7} {'Collective':<30} {'ColdWait(s)':>13} {'ColdExec(s)':>13} "
            f"{'ColdWait%':>11} {'WarmWait(s)':>13} {'WarmExec(s)':>13} {'WarmWait%':>11}\n"
        )
        output.write("-" * 109 + "\n")
        for row in collective_split_rows:
            output.write(
                f"{int(row['processes']):>7} {str(row['collective']):<30}"
                f"{format_number(float(row['cold_arrival_wait_max_s']), 13)}"
                f"{format_number(float(row['cold_aligned_execution_max_s']), 13)}"
                f"{format_number(float(row['cold_wait_percent_of_pair']), 11, 2)}"
                f"{format_number(float(row['warm_arrival_wait_max_s']), 13)}"
                f"{format_number(float(row['warm_aligned_execution_max_s']), 13)}"
                f"{format_number(float(row['warm_wait_percent_of_pair']), 11, 2)}\n"
            )

        output.write("\nTotal communication categories (通信类别总计)\n")
        output.write(
            f"{'P':>7} {'ColdWait(s)':>13} {'ColdWait(%)':>13} {'ColdComm(s)':>13} "
            f"{'ColdComm(%)':>13} {'WarmWait(s)':>13} {'WarmWait(%)':>13} "
            f"{'WarmComm(s)':>13} {'WarmComm(%)':>13}\n"
        )
        output.write("-" * 124 + "\n")
        for row in cold_warm_rows:
            output.write(
                f"{int(row['processes']):>7}"
                f"{format_number(float(row['cold_arrival_wait_max_s']), 13)}"
                f"{format_number(float(row['cold_arrival_wait_fraction_max_percent']), 13, 2)}"
                f"{format_number(float(row['cold_communication_max_s']), 13)}"
                f"{format_number(float(row['cold_communication_fraction_max_percent']), 13, 2)}"
                f"{format_number(float(row['warm_arrival_wait_max_s']), 13)}"
                f"{format_number(float(row['warm_arrival_wait_fraction_max_percent']), 13, 2)}"
                f"{format_number(float(row['warm_communication_max_s']), 13)}"
                f"{format_number(float(row['warm_communication_fraction_max_percent']), 13, 2)}\n"
            )

    scaling_by_process = {int(row["processes"]): row for row in summary_rows}
    with (output_dir / "stage_bottlenecks.txt").open("w", encoding="utf-8") as output:
        output.write("Stage Bottlenecks (阶段瓶颈)\n")
        output.write("=" * 154 + "\n")
        output.write(
            "每个进程数列出最大耗时最高的 10 个阶段；Total(%) 是阶段最大时间占端到端最大时间的比例。\n"
        )
        for processes in process_counts:
            process_rows = [
                row
                for row in stage_summary_rows
                if int(row["processes"]) == processes and float(row["time_max_s"]) > 0.0
            ]
            process_rows.sort(key=lambda row: float(row["time_max_s"]), reverse=True)
            cache_available = int(
                scaling_by_process[processes]["cache_valid_repetitions"]
            ) > 0
            io_available = int(
                scaling_by_process[processes]["io_valid_repetitions"]
            ) > 0
            output.write(f"\nP={processes}\n")
            output.write(
                f"{'Stage':<30} {'Category':<16} {'Max(s)':>10} {'Total(%)':>10} "
                f"{'M/A':>8} {'Miss(%)':>10} {'SendMiB':>11} {'RecvMiB':>11} "
                f"{'LogRMiB':>11} {'LogWMiB':>11} {'PhysRMiB':>11} {'PhysWMiB':>11}\n"
            )
            output.write("-" * 154 + "\n")
            for row in process_rows[:10]:
                cache_miss = (
                    float(row["cache_miss_rate_percent"]) if cache_available else math.nan
                )
                logical_read = float(row["logical_read_total_bytes"]) if io_available else math.nan
                logical_write = float(row["logical_write_total_bytes"]) if io_available else math.nan
                physical_read = float(row["physical_read_total_bytes"]) if io_available else math.nan
                physical_write = float(row["physical_write_total_bytes"]) if io_available else math.nan
                output.write(
                    f"{str(row['stage']):<30} {str(row['category']):<16}"
                    f"{format_number(float(row['time_max_s']), 10)}"
                    f"{format_number(float(row['max_percent_of_total']), 10, 2)}"
                    f"{format_number(float(row['max_over_avg']), 8, 3)}"
                    f"{format_number(cache_miss, 10, 3)}"
                    f"{format_number(float(row['send_total_bytes']) / (1024**2), 11, 3)}"
                    f"{format_number(float(row['receive_total_bytes']) / (1024**2), 11, 3)}"
                    f"{format_number(logical_read / (1024**2), 11, 3)}"
                    f"{format_number(logical_write / (1024**2), 11, 3)}"
                    f"{format_number(physical_read / (1024**2), 11, 3)}"
                    f"{format_number(physical_write / (1024**2), 11, 3)}\n"
                )

        output.write("\n说明：集合通信的字节数是应用层逻辑缓冲区规模，并非网络硬件实际传输量。\n")

    print(f"Analysis written to: {output_dir}")


if __name__ == "__main__":
    main()
