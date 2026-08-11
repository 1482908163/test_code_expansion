#!/usr/bin/env python3
"""Create one report from the core/cache and full-I/O suite modes."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path


DEFAULT_MODES = ["core_timing", "core_cache", "full_io"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="汇总统一强扩展实验")
    parser.add_argument("suite_dir", type=Path)
    parser.add_argument("--modes", nargs="+", default=DEFAULT_MODES)
    return parser.parse_args()


def read_mode(suite_dir: Path, mode: str) -> dict[int, dict[str, str]]:
    path = suite_dir / "modes" / mode / "analysis" / "cold_warm_summary.csv"
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as stream:
        return {int(row["processes"]): row for row in csv.DictReader(stream)}


def read_plan_value(suite_dir: Path, key: str, default: str = "unknown") -> str:
    path = suite_dir / "run_plan.txt"
    if not path.is_file():
        return default
    prefix = f"{key}="
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith(prefix):
                return line[len(prefix) :].strip()
    return default


def read_planned_processes(suite_dir: Path) -> list[int]:
    value = read_plan_value(suite_dir, "process_counts", "")
    try:
        return [int(item) for item in value.split()]
    except ValueError:
        return []


def read_mode_status(suite_dir: Path) -> dict[tuple[int, str], dict[str, str]]:
    status: dict[tuple[int, str], dict[str, str]] = {}
    for path in sorted((suite_dir / "mode_status").glob("*.tsv")):
        with path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                try:
                    key = (int(row["processes"]), row["mode"])
                except (KeyError, TypeError, ValueError):
                    continue
                status[key] = row
    return status


def read_analysis_status(suite_dir: Path) -> dict[str, dict[str, str]]:
    path = suite_dir / "analysis" / "mode_analysis_status.tsv"
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as stream:
        return {
            row["mode"]: row
            for row in csv.DictReader(stream, delimiter="\t")
            if row.get("mode")
        }


def number(row: dict[str, str] | None, key: str) -> float:
    if row is None:
        return math.nan
    value = row.get(key, "")
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def integer(row: dict[str, str] | None, key: str, default: int = 0) -> int:
    value = number(row, key)
    return int(value) if math.isfinite(value) else default


def safe_percent(numerator: float, denominator: float) -> float:
    if not math.isfinite(numerator) or not math.isfinite(denominator) or denominator == 0.0:
        return math.nan
    return 100.0 * numerator / denominator


def text(value: float, width: int, precision: int = 2) -> str:
    if not math.isfinite(value):
        return f"{'N/A':>{width}}"
    return f"{value:>{width}.{precision}f}"


def main() -> None:
    args = parse_args()
    suite_dir = args.suite_dir.resolve()
    output_dir = suite_dir / "analysis"
    output_dir.mkdir(parents=True, exist_ok=True)

    requested_modes = list(dict.fromkeys(args.modes))
    modes = {mode: read_mode(suite_dir, mode) for mode in requested_modes}
    if modes.get("core_timing"):
        core_mode = "core_timing"
    elif modes.get("core_cache"):
        core_mode = "core_cache"
    else:
        core_mode = "missing"
    core = modes.get(core_mode, {})
    cache = modes.get("core_cache", {})
    full_io = modes.get("full_io", {})
    jobs_serialized = read_plan_value(suite_dir, "serialize_jobs", "unknown")
    dependency_policy = read_plan_value(
        suite_dir, "dependency_policy", "unknown"
    )
    compute_status = read_mode_status(suite_dir)
    analysis_status = read_analysis_status(suite_dir)
    try:
        expected_repetitions = int(read_plan_value(suite_dir, "repeats", "0"))
    except ValueError:
        expected_repetitions = 0
    process_counts = sorted(
        set(read_planned_processes(suite_dir))
        | {processes for processes, _mode in compute_status}
        | set(core)
        | set(cache)
        | set(full_io)
    )
    if not process_counts:
        raise FileNotFoundError(f"No planned or analyzed process counts below {suite_dir}")

    status_rows: list[dict[str, int | str]] = []
    for processes in process_counts:
        for mode in requested_modes:
            recorded = compute_status.get((processes, mode))
            result_row = modes.get(mode, {}).get(processes)
            result_available = result_row is not None
            valid_repetitions = integer(result_row, "cold_repetitions") + integer(
                result_row, "warm_repetitions"
            )
            result_complete = result_available and (
                expected_repetitions <= 0 or valid_repetitions >= expected_repetitions
            )
            mode_analysis = analysis_status.get(mode, {})
            analysis_state = mode_analysis.get("state", "UNKNOWN")
            analysis_exit = mode_analysis.get("exit_code", "")
            if recorded and recorded.get("state") == "FAILED":
                state = "FAILED"
            elif analysis_state == "ANALYSIS_FAILED":
                state = "ANALYSIS_FAILED"
            elif recorded and recorded.get("state") == "COMPLETED" and result_complete:
                state = "COMPLETED"
            elif recorded and recorded.get("state") == "COMPLETED":
                state = "PARTIAL_RESULT" if result_available else "MISSING_RESULT"
            elif result_complete:
                state = "COMPLETED_UNTRACKED"
            elif result_available:
                state = "PARTIAL_RESULT"
            else:
                state = "MISSING"
            status_rows.append(
                {
                    "processes": processes,
                    "mode": mode,
                    "state": state,
                    "compute_exit_code": recorded.get("exit_code", "") if recorded else "",
                    "analysis_state": analysis_state,
                    "analysis_exit_code": analysis_exit,
                    "result_available": int(result_available),
                    "valid_repetitions": valid_repetitions,
                    "expected_repetitions": expected_repetitions or "unknown",
                }
            )

    with (output_dir / "suite_status.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(status_rows[0].keys()))
        writer.writeheader()
        writer.writerows(status_rows)

    rows: list[dict[str, float | int | str]] = []
    for processes in process_counts:
        core_row = core.get(processes)
        cache_row = cache.get(processes)
        io_row = full_io.get(processes)
        core_warm = number(core_row, "warm_total_wall_max_s")
        io_warm = number(io_row, "warm_total_wall_max_s")
        io_extra = io_warm - core_warm if math.isfinite(io_warm) and math.isfinite(core_warm) else math.nan
        rows.append(
            {
                "processes": processes,
                "core_source_mode": core_mode,
                "jobs_serialized": jobs_serialized,
                "dependency_policy": dependency_policy,
                "core_page_cache_policy": (
                    core_row.get("page_cache_policy", "unknown")
                    if core_row
                    else "missing"
                ),
                "full_io_page_cache_policy": (
                    io_row.get("page_cache_policy", "unknown")
                    if io_row
                    else "missing"
                ),
                "core_cold_s": number(core_row, "cold_total_wall_max_s"),
                "core_cold_state": (
                    core_row.get("cold_cache_state", "cold_candidate")
                    if core_row
                    else "missing"
                ),
                "core_warm_s": core_warm,
                "core_cold_penalty_percent": number(core_row, "cold_penalty_percent"),
                "core_warm_speedup": number(core_row, "warm_speedup"),
                "core_warm_efficiency_percent": number(
                    core_row, "warm_parallel_efficiency_percent"
                ),
                "core_warm_communication_s": number(
                    core_row, "warm_communication_max_s"
                ),
                "core_warm_communication_fraction_percent": number(
                    core_row, "warm_communication_fraction_max_percent"
                ),
                "core_warm_arrival_wait_s": number(
                    core_row, "warm_arrival_wait_max_s"
                ),
                "core_warm_arrival_wait_fraction_percent": number(
                    core_row, "warm_arrival_wait_fraction_max_percent"
                ),
                "core_warm_communication_plus_wait_fraction_percent": number(
                    core_row, "warm_communication_plus_wait_fraction_max_percent"
                ),
                "warm_cache_miss_rate_percent": number(
                    cache_row, "warm_cache_miss_rate_percent"
                ),
                "warm_ipc": number(cache_row, "warm_ipc"),
                "full_io_cold_s": number(io_row, "cold_total_wall_max_s"),
                "full_io_cold_state": (
                    io_row.get("cold_cache_state", "cold_candidate")
                    if io_row
                    else "missing"
                ),
                "full_io_warm_s": io_warm,
                "full_io_cold_penalty_percent": number(io_row, "cold_penalty_percent"),
                "full_io_warm_speedup": number(io_row, "warm_speedup"),
                "full_io_warm_efficiency_percent": number(
                    io_row, "warm_parallel_efficiency_percent"
                ),
                "full_io_warm_io_s": number(io_row, "warm_io_max_s"),
                "full_io_warm_io_fraction_percent": number(
                    io_row, "warm_io_fraction_max_percent"
                ),
                "full_io_extra_warm_s": io_extra,
                "full_io_extra_warm_percent": safe_percent(io_extra, core_warm),
                "core_cold_physical_read_gib": number(
                    core_row, "cold_physical_read_total_bytes"
                )
                / (1024**3),
                "core_warm_physical_read_gib": number(
                    core_row, "warm_physical_read_total_bytes"
                )
                / (1024**3),
                "full_io_cold_physical_read_gib": number(
                    io_row, "cold_physical_read_total_bytes"
                )
                / (1024**3),
                "full_io_warm_physical_read_gib": number(
                    io_row, "warm_physical_read_total_bytes"
                )
                / (1024**3),
                "full_io_cold_physical_write_gib": number(
                    io_row, "cold_physical_write_total_bytes"
                )
                / (1024**3),
                "full_io_warm_physical_write_gib": number(
                    io_row, "warm_physical_write_total_bytes"
                )
                / (1024**3),
            }
        )

    fields = list(rows[0].keys())
    with (output_dir / "suite_summary.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    with (output_dir / "suite_report.txt").open("w", encoding="utf-8") as output:
        output.write("Unified Strong-scaling Suite (统一强扩展实验报告)\n")
        output.write("=" * 138 + "\n")
        output.write(f"Suite directory (实验目录): {suite_dir}\n")
        output.write(f"Requested modes (请求模式): {', '.join(requested_modes)}\n")
        output.write(f"Available modes (有效模式): {', '.join(mode for mode, data in modes.items() if data)}\n")
        output.write(f"Core timing source (核心计时来源): {core_mode}\n")
        if jobs_serialized == "1":
            scheduling = f"serialized ({dependency_policy})"
        elif jobs_serialized == "0":
            scheduling = "concurrent"
        else:
            scheduling = "unknown"
        output.write(f"P-job scheduling (不同 P 调度): {scheduling}\n")
        core_policy = next(
            (
                str(row["core_page_cache_policy"])
                for row in rows
                if row["core_page_cache_policy"] != "missing"
            ),
            "missing",
        )
        io_policy = next(
            (
                str(row["full_io_page_cache_policy"])
                for row in rows
                if row["full_io_page_cache_policy"] != "missing"
            ),
            "missing",
        )
        output.write(
            "Page-cache policy (页缓存策略): "
            f"core={core_policy}, full_io={io_policy}\n"
        )
        output.write(
            "主曲线使用 warm（repeat 2+ 中位数）；cold（repeat 1）单独列出，不再混入波动系数。\n\n"
        )
        completed_states = {"COMPLETED", "COMPLETED_UNTRACKED"}
        completed_count = sum(row["state"] in completed_states for row in status_rows)
        output.write(
            f"Execution status (执行状态): {completed_count}/{len(status_rows)} completed; "
            f"details: suite_status.csv\n"
        )
        problem_rows = [row for row in status_rows if row["state"] not in completed_states]
        for row in problem_rows:
            output.write(
                f"  [{row['state']}] P={row['processes']} mode={row['mode']} "
                f"compute_exit={row['compute_exit_code'] or 'N/A'} "
                f"analysis_exit={row['analysis_exit_code'] or 'N/A'} "
                f"repeats={row['valid_repetitions']}/{row['expected_repetitions']}\n"
            )
        if problem_rows:
            output.write("\n")
        core_candidates = [
            str(row["processes"])
            for row in rows
            if row["core_cold_state"] == "cold_candidate"
        ]
        io_candidates = [
            str(row["processes"])
            for row in rows
            if row["full_io_cold_state"] == "cold_candidate"
        ]
        if core_candidates or io_candidates:
            output.write(
                "Cold candidates（未确认清缓存）: "
                f"core P={','.join(core_candidates) or 'none'}; "
                f"full_io P={','.join(io_candidates) or 'none'}。"
                "请结合物理读取量判断。\n\n"
            )
        output.write("Warm strong scaling (热缓存强扩展)\n")
        output.write(
            f"{'P':>7} {'Core(s)':>11} {'Speedup':>10} {'Eff(%)':>9} "
            f"{'Wait(s)':>10} {'Wait(%)':>9} {'Comm(s)':>10} {'Comm(%)':>9} "
            f"{'Miss(%)':>10} {'IPC':>8} {'FullIO(s)':>11} {'I/O(%)':>9} "
            f"{'Extra(%)':>10}\n"
        )
        output.write("-" * 138 + "\n")
        for row in rows:
            output.write(
                f"{int(row['processes']):>7}"
                f"{text(float(row['core_warm_s']), 11, 3)}"
                f"{text(float(row['core_warm_speedup']), 10, 3)}"
                f"{text(float(row['core_warm_efficiency_percent']), 9, 2)}"
                f"{text(float(row['core_warm_arrival_wait_s']), 10, 3)}"
                f"{text(float(row['core_warm_arrival_wait_fraction_percent']), 9, 2)}"
                f"{text(float(row['core_warm_communication_s']), 10, 3)}"
                f"{text(float(row['core_warm_communication_fraction_percent']), 9, 2)}"
                f"{text(float(row['warm_cache_miss_rate_percent']), 10, 3)}"
                f"{text(float(row['warm_ipc']), 8, 3)}"
                f"{text(float(row['full_io_warm_s']), 11, 3)}"
                f"{text(float(row['full_io_warm_io_fraction_percent']), 9, 2)}"
                f"{text(float(row['full_io_extra_warm_percent']), 10, 2)}\n"
            )

        split_available = any(
            math.isfinite(float(row["core_warm_arrival_wait_fraction_percent"]))
            for row in rows
        )
        if split_available:
            output.write(
                "\nWait 是集合通信前置 MPI_Barrier 的到达对齐时间；Comm 是对齐后的集合通信执行"
                "与点对点通信阶段，不是纯网络线速传输，仍包含 MPI 算法和协议开销；"
                "阻塞式点对点调用还可能包含对端就绪等待。\n"
            )
        elif core:
            output.write(
                "\n警告：当前是旧版结果，没有集合通信拆分字段；旧版 Comm(%) 包含到达等待，"
                "不能解释为对齐后的通信执行。\n"
            )
        else:
            output.write("\n当前没有可用的核心计时结果，Wait/Comm 均为 N/A。\n")

        output.write("\nCold vs warm page cache (冷/热页缓存)\n")
        output.write(
            f"{'P':>7} {'CoreCold':>11} {'CoreWarm':>11} {'CorePenalty%':>14} "
            f"{'IOCold':>11} {'IOWarm':>11} {'IOPenalty%':>12} "
            f"{'CorePhysR C/W(GiB)':>22} {'IOPhysR C/W(GiB)':>20}\n"
        )
        output.write("-" * 127 + "\n")
        for row in rows:
            core_physical_pair = (
                f"{float(row['core_cold_physical_read_gib']):.3f}/"
                f"{float(row['core_warm_physical_read_gib']):.3f}"
                if math.isfinite(float(row["core_cold_physical_read_gib"]))
                and math.isfinite(float(row["core_warm_physical_read_gib"]))
                else "N/A"
            )
            io_physical_pair = (
                f"{float(row['full_io_cold_physical_read_gib']):.3f}/"
                f"{float(row['full_io_warm_physical_read_gib']):.3f}"
                if math.isfinite(float(row["full_io_cold_physical_read_gib"]))
                and math.isfinite(float(row["full_io_warm_physical_read_gib"]))
                else "N/A"
            )
            output.write(
                f"{int(row['processes']):>7}"
                f"{text(float(row['core_cold_s']), 11, 3)}"
                f"{text(float(row['core_warm_s']), 11, 3)}"
                f"{text(float(row['core_cold_penalty_percent']), 14, 2)}"
                f"{text(float(row['full_io_cold_s']), 11, 3)}"
                f"{text(float(row['full_io_warm_s']), 11, 3)}"
                f"{text(float(row['full_io_cold_penalty_percent']), 12, 2)}"
                f"{core_physical_pair:>22}"
                f"{io_physical_pair:>20}\n"
            )

        output.write("\nFiles (文件)\n")
        output.write("  suite_summary.csv                   统一绘图数据\n")
        output.write("  suite_status.csv                    每个 P、每种模式的完成/失败/缺失状态\n")
        output.write("  mode_analysis_status.tsv            各模式汇总脚本状态\n")
        output.write("  suite_report.txt                    本文件\n")
        for mode, data in modes.items():
            if data:
                output.write(f"  ../modes/{mode}/analysis/            {mode} 的详细阶段报告\n")

    print(f"Suite analysis written to: {output_dir}")


if __name__ == "__main__":
    main()
