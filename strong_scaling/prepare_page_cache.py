#!/usr/bin/env python3
"""Request eviction of benchmark file-backed pages before a cold run.

This uses POSIX_FADV_DONTNEED, which is an unprivileged kernel hint.  It does
not clear anonymous memory or the whole node cache, so the profiler's physical
read counters remain the final evidence that a run was actually cold.
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="准备强扩展实验的冷页缓存状态")
    parser.add_argument("--input", type=Path, required=True, help="固定输入文件")
    parser.add_argument("--executable", type=Path, required=True, help="被测可执行文件")
    return parser.parse_args()


def linked_libraries(executable: Path) -> set[Path]:
    result = subprocess.run(
        ["ldd", str(executable)],
        check=False,
        capture_output=True,
        text=True,
    )
    libraries: set[Path] = set()
    for line in result.stdout.splitlines():
        match = re.search(r"=>\s+(/\S+)", line)
        if match is None:
            match = re.match(r"\s*(/\S+)", line)
        if match is not None:
            libraries.add(Path(match.group(1)))
    return libraries


def evict(path: Path) -> tuple[bool, str]:
    try:
        descriptor = os.open(path, os.O_RDONLY)
    except OSError as error:
        return False, f"open: {error}"
    try:
        os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
    except (AttributeError, OSError) as error:
        return False, f"posix_fadvise: {error}"
    finally:
        os.close(descriptor)
    return True, "ok"


def main() -> int:
    args = parse_args()
    targets = {args.input.resolve(), args.executable.resolve()}
    targets.update(linked_libraries(args.executable.resolve()))

    succeeded = 0
    failures: list[str] = []
    bytes_advised = 0
    for path in sorted(targets):
        if not path.is_file():
            failures.append(f"{path}: not a file")
            continue
        ok, reason = evict(path)
        if ok:
            succeeded += 1
            bytes_advised += path.stat().st_size
        else:
            failures.append(f"{path}: {reason}")

    rank = next(
        (
            os.environ[name]
            for name in ("SLURM_PROCID", "PMI_RANK", "OMPI_COMM_WORLD_RANK")
            if name in os.environ
        ),
        "?",
    )
    print(
        f"[PAGE-CACHE] host={socket.gethostname()} rank={rank} "
        f"advised_files={succeeded}/{len(targets)} "
        f"advised_mib={bytes_advised / (1024**2):.2f}"
    )
    for failure in failures:
        print(f"[PAGE-CACHE-WARN] {failure}")

    # A partial result is not strong enough to label the next run cold.  The
    # caller will downgrade it to cold_candidate while keeping the experiment
    # running unless PAGE_CACHE_STRICT=1 was requested.
    return 0 if succeeded == len(targets) else 1


if __name__ == "__main__":
    raise SystemExit(main())
