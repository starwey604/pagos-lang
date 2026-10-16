#!/usr/bin/env python3
"""Measure C3 snapshot phases/IO and scalar residual template reuse separately."""
import argparse
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import tempfile
from benchmark_provenance_queries import sample
from benchmark import run_sample

ROOT = pathlib.Path(__file__).resolve().parents[1]


def distribution(samples, names):
    result = {}
    for name in names:
        values = [record[name] for record in samples]
        quartiles = statistics.quantiles(values, n=4)
        result[name] = dict(median=statistics.median(values), min=min(values), max=max(values), iqr=quartiles[2] - quartiles[0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    args = parser.parse_args()
    if args.repeats < 3 or args.warmups < 1:
        parser.error("requires >=3 repeats and >=1 warmup")
    binary = ROOT / "build/release/pagosc"
    probe = ROOT / "build/release/pagos_snapshot_bench"
    cache = (binary.parent / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        parser.error("use a Release build")
    sources = ["include/pagos/stage/provenance_snapshot.h", "lib/stage/provenance_snapshot.cpp",
               "lib/stage/residual_template.cpp", "lib/stage/scalar_cache.cpp", "lib/stage/stage_analyzer.cpp",
               "lib/cache/key.cpp", "lib/cache/store.cpp", "tools/pagosc/main.cpp",
               "tools/snapshot_bench/main.cpp", "scripts/benchmark_evidence.py",
               "scripts/benchmark.py", "scripts/benchmark_provenance_queries.py"]
    report = dict(schema=1, commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                  dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
                  machine=platform.platform(),
                  cpu=next(line.split(":", 1)[1].strip() for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                  versions={name: subprocess.check_output([name, "--version"], text=True).splitlines()[0] for name in ("clang", "llvm-config", "python3")},
                  binaries={str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary, probe)},
                  source_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
                  cmake_cache_sha256=hashlib.sha256(cache.encode()).hexdigest(), warmups=args.warmups, repeats=args.repeats,
                  measurement="Release independent children, per-child wait4 peak RSS includes startup/all phases. In-memory probe separates checked identity, Stage/recording, encoding, whole checksum/open, full restoration and one query. CLI snapshot read includes filesystem read/checksum/decoding plus current parsing/typecheck/identity; Stage is skipped with max-fuel=0. Residual off/cold/hot includes all compiler startup and check work; hot uses a seeded read-only cache, warm output hashes must equal off. No CPU/page-cache controls, no claim of lazy IO or reduced compiler peak RSS.",
                  snapshot_phases=[], snapshot_io=[], residual=[])
    with tempfile.TemporaryDirectory(prefix="pagos-evidence-benchmark-") as directory:
        directory = pathlib.Path(directory)
        for size in (128, 512, 1024):
            command = [str(probe), str(size)]
            for _ in range(args.warmups): sample(command, directory)
            samples = [sample(command, directory) for _ in range(args.repeats)]
            report["snapshot_phases"].append(dict(size=size, samples=samples, distributions=distribution(samples,
                ("checked_identity_ns", "stage_ns", "encode_ns", "checksum_open_ns", "restore_ns", "query_ns", "peak_rss_kib"))))
        source = directory / "module.pgs"
        source.write_text("fn large(x: u32) -> u32 { let n0 = x + 1; " + " ".join(f"let n{i} = n{i-1} + 1;" for i in range(1, 512)) + " n511 } let result = large(external_input());")
        snapshots = directory / "snapshots"
        exported = subprocess.run([str(binary), f"--snapshot-write={snapshots}", "explain-provenance", str(source)], capture_output=True, check=True)
        report["snapshot_object_sizes"] = sorted(p.stat().st_size for p in (snapshots / "objects").rglob("*") if p.is_file())
        for mode in ("analyze", "read"):
            command = [str(binary)] + ([] if mode == "analyze" else [f"--snapshot-read={snapshots}", "--max-fuel=0"]) + ["explain-provenance", str(source)]
            for _ in range(args.warmups): run_sample(command, directory)
            samples = [run_sample(command, directory) for _ in range(args.repeats)]
            expected = hashlib.sha256(exported.stdout).hexdigest()
            assert all(record["output_sha256"] == expected for record in samples)
            report["snapshot_io"].append(dict(mode=mode, samples=samples, distributions=distribution(samples, ("seconds", "peak_rss_kib"))))
        source.write_text("fn scale(k: u32, x: u32) -> u32 { k * x } let a = external_input(); let b = external_input(); let aa = scale(4, a); let bb = scale(4, b);")
        store = directory / "residual-cache"
        subprocess.run([str(binary), "--stage-cache=read-write", f"--cache-dir={store}", "check", str(source)], check=True)
        for mode in ("off", "cold", "hot"):
            def measured(index):
                target = store if mode == "hot" else directory / f"{mode}-{index}"
                command = [str(binary), f"--stage-cache={dict(off='off', cold='read-write', hot='read-only')[mode]}", f"--cache-dir={target}", "--cache-stats", "check", str(source)]
                return run_sample(command, directory, target)
            for index in range(args.warmups): measured(f"warmup-{index}")
            samples = [measured(index) for index in range(args.repeats)]
            if mode == "hot":
                assert all(record["cache_telemetry"]["residual-hits"] == 1 and record["cache_telemetry"]["body-evaluations"] == 0 for record in samples)
            report["residual"].append(dict(mode=mode, samples=samples, distributions=distribution(samples, ("seconds", "peak_rss_kib"))))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote 8 groups to {args.output}")


if __name__ == "__main__":
    main()
