#!/usr/bin/env python3
"""Measure independent compiler processes, including off/cold/hot Stage caches."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = ["tests/lit/stage/crc32.pgs", "benchmarks/cache-hits.pgs",
         "benchmarks/specializations.pgs", "benchmarks/aggregate.pgs",
         "benchmarks/member-projections.pgs", "benchmarks/closed-generator.pgs",
         "tests/lit/mir/nested-range-loop.pgs"]
COMMANDS = ("check", "emit-mir", "emit-llvm")
LIMITS = ["--max-fuel=1000000", "--max-specializations=8192",
          "--max-recursion-depth=64", "--max-array-elements=65536",
          "--max-array-bytes=262144", "--max-aggregate-members=65536",
          "--max-aggregate-bytes=262144"]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def output(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def disk_usage(directory):
    files = [p for p in directory.rglob("*") if p.is_file()]
    return {"files": len(files), "bytes": sum(p.stat().st_size for p in files),
            "objects": sum(p.is_relative_to(directory / "objects") for p in files),
            "manifests": sum(p.is_relative_to(directory / "queries") and
                             p.suffix == ".manifest" for p in files)}


def run_sample(command, directory, cache=None):
    """wait4 reports this child's RSS, not accumulated earlier children."""
    # Regular temporary files avoid a full output pipe while wait4 owns reaping.
    # No resource.getrusage(RUSAGE_CHILDREN): its maximum spans prior samples.
    with tempfile.TemporaryFile(dir=directory) as out, tempfile.TemporaryFile(dir=directory) as err:
        start = time.perf_counter()
        process = subprocess.Popen(command, stdout=out, stderr=err, cwd=ROOT,
                                   start_new_session=True)
        deadline = time.monotonic() + 30
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                process.returncode = os.waitstatus_to_exitcode(status)
                break
            if time.monotonic() >= deadline:
                os.killpg(process.pid, signal.SIGKILL)
                _, status, _ = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                raise subprocess.TimeoutExpired(command, 30)
            time.sleep(0.001)
        elapsed = time.perf_counter() - start
        out.seek(0)
        err.seek(0)
        stdout, stderr = out.read(), err.read()
    if process.returncode:
        raise RuntimeError(stderr.decode())
    telemetry_line = next((line for line in stderr.decode().splitlines()
                           if line.startswith("stage-cache: ")), "")
    telemetry = {key: int(value) for key, value in
                 re.findall(r"([a-z][a-z-]*)=(\d+)", telemetry_line)}
    storage = disk_usage(cache) if cache else None
    if telemetry.get("hits") and storage and not storage["manifests"]:
        raise RuntimeError("cache hits without measured manifests")
    return {"seconds": elapsed, "peak_rss_kib": int(usage.ru_maxrss),
            "output_bytes": len(stdout), "output_sha256": digest(stdout),
            "cache_telemetry": telemetry,
            "cache_disk": storage}


def summarize(samples):
    if len({(s["output_bytes"], s["output_sha256"]) for s in samples}) != 1:
        raise RuntimeError("non-repeatable compiler output")
    seconds = [sample["seconds"] for sample in samples]
    return {"median_seconds": statistics.median(seconds),
            "min_seconds": min(seconds), "max_seconds": max(seconds),
            "peak_rss_kib": max(s["peak_rss_kib"] for s in samples),
            "output_bytes": samples[0]["output_bytes"], "samples": samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/release/pagosc")
    parser.add_argument("--reference-compiler", type=Path,
                        help="alternate cache-off samples with an earlier Release binary")
    parser.add_argument("--cache-modes", choices=("off", "cold", "hot", "all"), default="off")
    parser.add_argument("--case", action="append", help="source path; repeat to select cases")
    parser.add_argument("--command", action="append", choices=COMMANDS)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not hasattr(os, "wait4"):
        parser.error("benchmark requires Linux wait4 for per-child peak RSS")
    if args.repeats < 3 or args.warmups < 1:
        parser.error("use at least 3 repeats and 1 warmup")
    if args.reference_compiler and args.cache_modes != "off":
        parser.error("reference comparison requires --cache-modes=off")
    compiler = args.compiler.resolve()
    reference = args.reference_compiler.resolve() if args.reference_compiler else None
    cmake_cache = (compiler.parent / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cmake_cache:
        parser.error("benchmark compiler must have a Release CMake build")
    if reference and "CMAKE_BUILD_TYPE:STRING=Release" not in (
            reference.parent / "CMakeCache.txt").read_text():
        parser.error("reference compiler must also have a Release CMake build")
    policies = ("off", "cold", "hot") if args.cache_modes == "all" else (args.cache_modes,)
    report = {"schema": 2, "commit": output(["git", "rev-parse", "HEAD"]),
              "dirty": bool(output(["git", "status", "--porcelain"])),
              "compiler_sha256": digest(compiler.read_bytes()),
              "tracked_diff_sha256": digest(subprocess.check_output(["git", "diff", "HEAD"], cwd=ROOT)),
              "build_type": "Release", "machine": platform.platform(),
              "cpu": next((line.split(":", 1)[1].strip() for line in
                           Path("/proc/cpuinfo").read_text().splitlines()
                           if line.startswith("model name")), "unknown"),
              "cmake_cache_sha256": digest(cmake_cache.encode()),
              "versions": {tool: output([tool, "--version"]).splitlines()[0]
                           for tool in ("clang", "llvm-config", "cmake", "python3")},
              "warmups": args.warmups, "repeats": args.repeats, "limits": LIMITS,
              "cache_modes": policies,
              "measurement": {
                  "wall": "seconds include direct process launch, output capture and wait4 polling (1 ms interval)",
                  "rss": "per-process peak in KiB; no memory-reduction claim",
                  "cache": "cold clears disk cache before every child; hot is independently seeded then read-only; OS page cache is uncontrolled",
                  "phases": "nanoseconds, collected with --cache-stats: dependency builds module identity; query includes admission/key/recipe; read and publish include filesystem, checksums, manifest codecs and locking; decode includes result validation; encode excludes publish; evaluate is inclusive function-body time (nested bodies overlap); resource and restore are separate hit phases"
              }, "results": []}
    if reference:
        report["reference"] = {
            "path": str(reference), "compiler_sha256": digest(reference.read_bytes()),
            "commit": subprocess.check_output(
                ["git", "-C", str(reference.parent), "rev-parse", "HEAD"], text=True).strip()}
    with tempfile.TemporaryDirectory(prefix="pagos-benchmark-") as temporary:
        directory = Path(temporary)
        for case_index, case in enumerate(args.case or CASES):
            source = (ROOT / case).resolve()
            stage_cache = directory / f"stage-verification-{case_index}"
            verification = {}
            explanation = None
            for policy, cache_mode in (("off", "off"), ("cold", "read-write"),
                                       ("hot", "read-only")):
                verified = subprocess.run(
                    [str(compiler), *LIMITS, f"--stage-cache={cache_mode}",
                     f"--cache-dir={stage_cache}", "--cache-stats", "explain-stage", str(source)],
                    cwd=ROOT, capture_output=True, text=True, check=True, timeout=30)
                if explanation is not None and verified.stdout != explanation:
                    raise RuntimeError(f"cache modes changed Stage/stats: {case}")
                explanation = verified.stdout
                verification[policy] = {key: int(value) for key, value in
                                        re.findall(r"([a-z][a-z-]*)=(\d+)", verified.stderr)}
            stats = {key: int(value) for key, value in
                     re.findall(r"([a-z][a-z-]*)=(\d+)", explanation)}
            for mode in args.command or COMMANDS:
                entries = {}
                commands = {}
                for policy in policies:
                    cache = directory / f"cache-{case_index}-{mode}-{policy}"
                    flags = [f"--stage-cache={'read-only' if policy == 'hot' else 'read-write' if policy == 'cold' else 'off'}",
                             f"--cache-dir={cache}", "--cache-stats"]
                    commands[policy] = [str(compiler), *LIMITS, *flags, mode, str(source)]
                    entry = {"case": case, "mode": mode, "cache_mode": policy,
                             "source_bytes": source.stat().st_size,
                             "source_sha256": digest(source.read_bytes()),
                             "stage_stats": stats, "stage_verification": verification,
                             "warmup_samples": [], "samples": []}
                    entries[policy] = (entry, cache)
                    if policy == "hot":
                        seed = [str(compiler), *LIMITS, "--stage-cache=read-write",
                                f"--cache-dir={cache}", "--cache-stats", mode, str(source)]
                        entry["seed_sample"] = run_sample(seed, directory, cache)
                for index in range(args.warmups + args.repeats):
                    order = list(policies)
                    # Rotate/reverse order to limit drift between cache modes.
                    order = order[index % len(order):] + order[:index % len(order)]
                    if index % 2:
                        order.reverse()
                    for policy in order:
                        entry, cache = entries[policy]
                        if policy == "cold" and cache.exists():
                            shutil.rmtree(cache)
                        destination = entry["warmup_samples"] if index < args.warmups else entry["samples"]
                        if reference:
                            # Older binaries need not implement cache controls.
                            reference_command = [str(reference), *LIMITS, mode, str(source)]
                            pair = [(commands[policy], destination, cache),
                                    (reference_command, entry.setdefault("reference_warmups" if index < args.warmups else "reference_samples", []), None)]
                            if index % 2:
                                pair.reverse()
                            for command, samples, disk in pair:
                                samples.append(run_sample(command, directory, disk))
                        else:
                            destination.append(run_sample(commands[policy], directory, cache))
                hashes = set()
                for policy in policies:
                    entry, _ = entries[policy]
                    entry.update(summarize(entry["samples"]))
                    hashes.add(entry["samples"][0]["output_sha256"])
                    if reference:
                        prior = entry.pop("reference_samples")
                        entry["reference"] = summarize(prior)
                        entry["reference"]["median_paired_ratio"] = statistics.median(
                            current["seconds"] / previous["seconds"] for current, previous in
                            zip(entry["samples"], prior, strict=True))
                    report["results"].append(entry)
                    print(f"{case} {mode} {policy}: {entry['median_seconds']:.6f}s", flush=True)
                if len(hashes) != 1:
                    raise RuntimeError(f"cache modes changed output: {case}/{mode}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
