#!/usr/bin/env python3
"""Create one report from the core/cache and full-I/O suite modes."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="汇总统一强扩展实验")
    parser.add_argument("suite_dir", type=Path)
    parser.add_argument("--modes", nargs="+", default=["core_cache", "full_io"])
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


def number(row: dict[str, str] | None, key: str) -> float:
    if row is None:
        return math.nan
    value = row.get(key, "")
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


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

    modes = {mode: read_mode(suite_dir, mode) for mode in args.modes}
    core_mode = "core_timing" if modes.get("core_timing") else "core_cache"
    core = modes.get(core_mode, {})
    cache = modes.get("core_cache", {})
    full_io = modes.get("full_io", {})
    jobs_serialized = read_plan_value(suite_dir, "serialize_jobs", "unknown")
    dependency_policy = read_plan_value(
        suite_dir, "dependency_policy", "unknown"
    )
    process_counts = sorted(set(core) | set(cache) | set(full_io))
    if not process_counts:
        raise FileNotFoundError(f"No analyzed mode data below {suite_dir / 'modes'}")

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
        output.write("=" * 116 + "\n")
        output.write(f"Suite directory (实验目录): {suite_dir}\n")
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
            f"{'Comm(s)':>10} {'Comm(%)':>9} "
            f"{'Miss(%)':>10} {'IPC':>8} {'FullIO(s)':>11} {'I/O(%)':>9} "
            f"{'Extra(%)':>10}\n"
        )
        output.write("-" * 113 + "\n")
        for row in rows:
            output.write(
                f"{int(row['processes']):>7}"
                f"{text(float(row['core_warm_s']), 11, 3)}"
                f"{text(float(row['core_warm_speedup']), 10, 3)}"
                f"{text(float(row['core_warm_efficiency_percent']), 9, 2)}"
                f"{text(float(row['core_warm_communication_s']), 10, 3)}"
                f"{text(float(row['core_warm_communication_fraction_percent']), 9, 2)}"
                f"{text(float(row['warm_cache_miss_rate_percent']), 10, 3)}"
                f"{text(float(row['warm_ipc']), 8, 3)}"
                f"{text(float(row['full_io_warm_s']), 11, 3)}"
                f"{text(float(row['full_io_warm_io_fraction_percent']), 9, 2)}"
                f"{text(float(row['full_io_extra_warm_percent']), 10, 2)}\n"
            )

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
        output.write("  suite_report.txt                    本文件\n")
        for mode, data in modes.items():
            if data:
                output.write(f"  ../modes/{mode}/analysis/            {mode} 的详细阶段报告\n")

    print(f"Suite analysis written to: {output_dir}")


if __name__ == "__main__":
    main()
