#!/usr/bin/env python3
"""Compare isolated provenance representations, never compiler semantics."""

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
STORAGES = ("paths", "append", "intern")
CASES = ("chain", "unique", "fan-in", "repeated", "calls", "aggregate", "recursive")
QUERIES = ("build", "witness", "sources", "batch")


def output(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def sample(command, directory, timeout):
    metrics = directory / "rss.txt"
    begin = time.perf_counter()
    process = subprocess.Popen(
        ["/usr/bin/time", "-f", "%M", "-o", str(metrics), *command],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise RuntimeError(f"measurement timed out: {command}") from None
    if process.returncode:
        raise RuntimeError(stderr.decode())
    record = json.loads(stdout)
    record["wall_seconds"] = time.perf_counter() - begin
    record["peak_rss_kib"] = int(metrics.read_text().strip())
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build/release/pagos_provenance_bench")
    parser.add_argument("--sizes", type=int, nargs="+", default=[512, 1024, 2048])
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--storages", nargs="+", choices=STORAGES,
                        default=list(STORAGES))
    parser.add_argument("--queries", nargs="+", choices=QUERIES,
                        default=list(QUERIES))
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (args.repeats < 3 or args.warmups < 1 or args.iterations < 1
            or args.iterations > 1_000_000 or args.timeout <= 0
            or any(size < 1 or size > 1_000_000 for size in args.sizes)):
        parser.error("use >=3 repeats, >=1 warmup, positive timeout, and "
                     "sizes/iterations in 1..1000000")
    binary = args.binary.resolve()
    cache = (binary.parent / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        parser.error("use a Release CMake build")
    sources = ["include/pagos/stage/provenance_prototype.h",
               "lib/stage/provenance_prototype.cpp",
               "tools/provenance_bench/main.cpp",
               "scripts/benchmark_provenance.py"]
    report = {
        "schema": 1,
        "commit": output(["git", "rev-parse", "HEAD"]),
        "dirty": bool(output(["git", "status", "--porcelain"])),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
                          for path in sources},
        "cmake_cache_sha256": hashlib.sha256(cache.encode()).hexdigest(),
        "machine": platform.platform(),
        "cpu": next((line.split(":", 1)[1].strip() for line in
                     Path("/proc/cpuinfo").read_text().splitlines()
                     if line.startswith("model name")), "unknown"),
        "versions": {tool: output([tool, "--version"]).splitlines()[0]
                     for tool in ("clang", "llvm-config", "python3")},
        "warmups": args.warmups, "repeats": args.repeats,
        "iterations": args.iterations,
        "sizes": args.sizes, "cases": args.cases, "queries": args.queries,
        "storages": args.storages,
        "measurement": (
            "Synthetic evidence events, not .pgs compilation. C++ phase clocks "
            "exclude process startup; GNU time RSS includes build and query. "
            "paths retains one source, not a complete source set. batch is "
            "independent cold witness queries for all workload bindings. "
            "store capacity excludes allocator overhead and query temporaries. "
            "C++ allocation counters exclude LLVM DenseMap's direct malloc "
            "and count requested cumulative bytes, not live memory."),
        "results": [],
    }
    with tempfile.TemporaryDirectory(prefix="pagos-provenance-benchmark-") as temporary:
        directory = Path(temporary)
        for case in args.cases:
            for size in args.sizes:
                for query in args.queries:
                    commands = {
                        storage: [str(binary), "--storage", storage, "--case", case,
                                  "--size", str(size), "--query", query,
                                  "--iterations", str(args.iterations)]
                        for storage in args.storages}
                    samples = {storage: [] for storage in args.storages}
                    for repeat in range(args.warmups + args.repeats):
                        # Alternate order to reduce systematic warm-machine bias.
                        order = (args.storages if repeat % 2 == 0
                                 else list(reversed(args.storages)))
                        for storage in order:
                            result = sample(commands[storage], directory, args.timeout)
                            if repeat >= args.warmups:
                                samples[storage].append(result)
                    for storage in args.storages:
                        records = samples[storage]
                        stable = [
                            {key: value for key, value in record.items()
                             if key not in ("build_ns", "query_ns", "wall_seconds",
                                            "peak_rss_kib")}
                            for record in records]
                        if any(record != stable[0] for record in stable[1:]):
                            raise RuntimeError(f"non-repeatable result: {case}/{query}/{storage}")
                        builds = [record["build_ns"] / 1e6 for record in records]
                        queries = [record["query_ns"] / args.iterations / 1e6
                                   for record in records]
                        report["results"].append({
                            **stable[0],
                            "median_build_ms": statistics.median(builds),
                            "build_ms_range": [min(builds), max(builds)],
                            "median_query_ms": statistics.median(queries),
                            "query_ms_range": [min(queries), max(queries)],
                            "peak_rss_kib": max(record["peak_rss_kib"] for record in records),
                            "samples": records,
                        })
                    # Layout may differ, but graph stores must agree on facts.
                    semantic = ("sources", "expected_sources", "sources_complete",
                                "checksum")
                    if ("append" in samples and "intern" in samples
                            and any(samples["append"][0][key] != samples["intern"][0][key]
                                    for key in semantic)):
                        raise RuntimeError(f"graph stores disagree: {case}/{query}/{size}")
                    print(f"{case} size={size} query={query}: measured {','.join(args.storages)}",
                          flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Wrote {args.output}", flush=True)


if __name__ == "__main__":
    main()
