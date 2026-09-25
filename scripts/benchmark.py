#!/usr/bin/env python3
"""Measure whole-command compiler costs; no performance pass/fail thresholds."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = ["tests/lit/stage/crc32.pgs", "benchmarks/cache-hits.pgs",
         "benchmarks/specializations.pgs", "benchmarks/aggregate.pgs",
         "tests/lit/mir/nested-range-loop.pgs"]
LIMITS = ["--max-fuel=1000000", "--max-specializations=8192",
          "--max-recursion-depth=64", "--max-array-elements=65536",
          "--max-array-bytes=262144", "--max-aggregate-members=65536",
          "--max-aggregate-bytes=262144"]


def output(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def run_sample(command, directory):
    """GNU time reports this child's RSS, not accumulated earlier children."""
    metrics = directory / "rss.txt"
    start = time.perf_counter()
    process = subprocess.Popen(["/usr/bin/time", "-f", "%M", "-o", str(metrics),
                                *command], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, cwd=ROOT, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise
    elapsed = time.perf_counter() - start
    if process.returncode:
        raise RuntimeError(stderr.decode())
    return elapsed, int(metrics.read_text().strip()), len(stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/release/pagosc")
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 3 or args.warmups < 1:
        parser.error("use at least 3 repeats and 1 warmup")
    compiler = args.compiler.resolve()
    cache = (compiler.parent / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        parser.error("benchmark compiler must have a Release CMake build")
    report = {"schema": 1, "commit": output(["git", "rev-parse", "HEAD"]),
              "dirty": bool(output(["git", "status", "--porcelain"])),
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "build_type": "Release", "machine": platform.platform(),
              "cpu": next((line.split(":", 1)[1].strip() for line in
                           Path("/proc/cpuinfo").read_text().splitlines()
                           if line.startswith("model name")), "unknown"),
              "cmake_cache_sha256": hashlib.sha256(cache.encode()).hexdigest(),
              "versions": {tool: output([tool, "--version"]).splitlines()[0]
                           for tool in ("clang", "llvm-config", "cmake", "python3")},
              "warmups": args.warmups, "repeats": args.repeats, "limits": LIMITS,
              "measurement": "wall seconds include GNU time/process-launch overhead; RSS in KiB",
              "results": []}
    with tempfile.TemporaryDirectory(prefix="pagos-benchmark-") as temporary:
        directory = Path(temporary)
        for case in CASES:
            source = ROOT / case
            explanation = output([str(compiler), *LIMITS, "explain-stage", str(source)])
            stats = {key: int(value) for key, value in
                     re.findall(r"([a-z][a-z-]*)=(\d+)", explanation)}
            for mode in ("check", "emit-mir", "emit-llvm"):
                command = [str(compiler), *LIMITS, mode, str(source)]
                for _ in range(args.warmups):
                    run_sample(command, directory)
                samples = [run_sample(command, directory) for _ in range(args.repeats)]
                sizes = {sample[2] for sample in samples}
                if len(sizes) != 1:
                    raise RuntimeError(f"non-repeatable output size: {case}/{mode}")
                seconds = [sample[0] for sample in samples]
                report["results"].append({"case": case, "mode": mode,
                    "source_bytes": source.stat().st_size,
                    "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "median_seconds": statistics.median(seconds),
                    "min_seconds": min(seconds), "max_seconds": max(seconds),
                    "peak_rss_kib": max(sample[1] for sample in samples),
                    "output_bytes": samples[0][2], "stage_stats": stats})
                print(f"{case} {mode}: {statistics.median(seconds):.6f}s", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
