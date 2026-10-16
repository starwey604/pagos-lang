#!/usr/bin/env python3
"""Measure in-memory provenance continuation and formula preparation separately."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def sample(command, directory):
    with tempfile.TemporaryFile(dir=directory) as out, tempfile.TemporaryFile(dir=directory) as err:
        begin = time.perf_counter()
        process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                process.returncode = os.waitstatus_to_exitcode(status)
                break
            if time.perf_counter() - begin > 60:
                os.killpg(process.pid, signal.SIGKILL)
                _, status, _ = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                raise RuntimeError("benchmark timeout")
            time.sleep(0.001)
        elapsed = time.perf_counter() - begin
        out.seek(0)
        err.seek(0)
        if process.returncode:
            raise RuntimeError(err.read().decode())
        record = json.loads(out.read())
        record.update(wall_seconds=elapsed, peak_rss_kib=usage.ru_maxrss)
        return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/release/pagos_provenance_query_bench")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    args = parser.parse_args()
    if args.repeats < 3 or args.warmups < 1 or not hasattr(os, "wait4"):
        parser.error("requires >=3 repeats, >=1 warmup and Linux wait4")
    binary = args.binary.resolve()
    cache = (binary.parent / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        parser.error("use a Release build")
    sources = ["include/pagos/stage/provenance_graph.h", "include/pagos/stage/provenance_query.h",
               "include/pagos/stage/provenance_topology.h", "lib/stage/provenance_graph.cpp",
               "lib/stage/provenance_query.cpp", "lib/stage/provenance_topology.cpp",
               "tools/provenance_query_bench/main.cpp", "scripts/benchmark_provenance_queries.py"]
    report = {
        "schema": 1, "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
        "machine": platform.platform(),
        "cpu": next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "cmake_cache_sha256": hashlib.sha256(cache.encode()).hexdigest(),
        "source_sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
        "versions": {name: subprocess.check_output([name, "--version"], text=True).splitlines()[0] for name in ("clang", "llvm-config", "python3")},
        "warmups": args.warmups, "repeats": args.repeats,
        "measurement": "Synthetic graph, not language compilation. Phase clocks separate build/freeze, topology/formula preparation, and one query including result copy. Per-child wait4 peak RSS includes all phases and startup; formula capacity excludes hash buckets, task queue/visited and allocator overhead. Wall time includes 1 ms polling. Legacy telemetry only reports compatibility counts. Selected path validation is in query time. No persisted cursor or source-instance pre-expansion.",
        "results": []}
    matrix = [(case, size, mode) for case, size in (("chain", 1000001), ("fan-in", 10000), ("diamond", 40), ("calls", 10))
              for mode in ("legacy", "resumable", "compressed")]
    matrix += [("calls", 40, mode) for mode in ("topology", "selected")]
    with tempfile.TemporaryDirectory(prefix="pagos-query-benchmark-") as directory:
        for case, size, mode in matrix:
            command = [str(binary), mode, case, str(size)]
            for _ in range(args.warmups):
                sample(command, directory)
            samples = [sample(command, directory) for _ in range(args.repeats)]
            distributions = {}
            for key in ("build_ns", "prepare_ns", "query_ns", "wall_seconds", "peak_rss_kib"):
                values = [record[key] for record in samples]
                quartiles = statistics.quantiles(values, n=4)
                distributions[key] = {"median": statistics.median(values), "min": min(values), "max": max(values), "iqr": quartiles[2] - quartiles[0]}
            report["results"].append({"case": case, "size": size, "mode": mode, "distributions": distributions, "samples": samples})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {len(matrix)} groups to {args.output}")


if __name__ == "__main__":
    main()
