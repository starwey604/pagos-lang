#!/usr/bin/env python3
"""C4a3: independent real Stage export, lazy queries and eager restoration."""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import tempfile
from benchmark_provenance_queries import sample

ROOT = pathlib.Path(__file__).resolve().parents[1]


def distribution(records):
    result = {}
    for name, value in records[0].items():
        if not isinstance(value, (int, float)):
            continue
        values = [record[name] for record in records]
        q = statistics.quantiles(values, n=4)
        result[name] = dict(median=statistics.median(values), min=min(values), max=max(values), iqr=q[2] - q[0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--sizes", type=int, nargs="+", default=[128, 512, 1024])
    args = parser.parse_args()
    if args.repeats < 3 or args.warmups < 1 or any(size < 1 or size > 4096 for size in args.sizes):
        parser.error("requires >=3 repeats, >=1 warmup, sizes 1..4096")
    binary = ROOT / "build/release/pagos_function_evidence_bench"
    cache = (binary.parent / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        parser.error("use Release")
    sources = ["CMakeLists.txt", "tools/function_evidence_bench/main.cpp", "scripts/benchmark_function_evidence.py",
               "scripts/benchmark_provenance_queries.py"]
    sources += [str(path.relative_to(ROOT)) for parent in (ROOT / "include/pagos/stage", ROOT / "lib/stage", ROOT / "include/pagos/cache", ROOT / "lib/cache")
                for path in sorted(parent.glob("*")) if path.suffix in (".h", ".cpp")]
    report = dict(schema=1, date=datetime.date.today().isoformat(), commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                  dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
                  machine=platform.platform(), cpu=next(line.split(":", 1)[1].strip() for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                  versions={name: subprocess.check_output([name, "--version"], text=True).splitlines()[0] for name in ("clang", "llvm-config", "python3")},
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), cmake_cache_sha256=hashlib.sha256(cache.encode()).hexdigest(),
                  source_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
                  repeats=args.repeats, warmups=args.warmups, results=[],
                  measurement="Real parsed/typechecked .pgs with two independently specialized scalar functions, each with N named arithmetic steps, both called once. Export runs Stage in a separate child; lazy/eager children parse/typecheck/current identity but never run Stage. Selected query is aa, twice in each process. Eager constructs a full FrozenGraph plus source metadata with a zero-byte decoder cache, then releases disk closure before querying. Lazy uses bounded SCC leases and never loads b's cold block. Phase clocks include hashing semantic source/call-path output. Per-child wait4 RSS is the high-water mark of the WHOLE child, including startup, AST/typecheck/hot metadata and restoration/query, NOT an isolated query peak or build-time saving. Decoded live/cached capacity omits hot indexes, queue/visited and allocator overhead. Warmup/repeat children share the OS page cache; no physical-disk/cold-page claim. Payload byte counts exclude 49-byte CAS envelopes and query manifest IO.")
    with tempfile.TemporaryDirectory(prefix="pagos-function-bench-") as directory:
        directory = pathlib.Path(directory)
        for size in args.sizes:
            body = "let n0 = x + 1; " + " ".join(f"let n{i} = n{i-1} + 1;" for i in range(1, size)) + f" n{size-1}"
            text = f"fn a(x: u32) -> u32 {{ {body} }} fn b(x: u32) -> u32 {{ {body} }} let x = external_input(); let y = external_input(); let aa = a(x); let bb = b(y);"
            source = directory / f"module-{size}.pgs"
            source.write_text(text)
            store = directory / f"store-{size}"
            published = sample([str(binary), "publish", str(source), str(store)], directory)
            export_samples = []
            for index in range(args.warmups + args.repeats):
                record = sample([str(binary), "publish", str(source), str(directory / f"export-{size}-{index}")], directory)
                if index >= args.warmups:
                    export_samples.append(record)
            report["results"].append(dict(size=size, mode="publish", source_sha256=hashlib.sha256(text.encode()).hexdigest(), seed=published,
                samples=export_samples, distributions=distribution(export_samples)))
            signatures = set()
            for mode in ("lazy", "eager"):
                command = [str(binary), mode, str(source), str(store)]
                for _ in range(args.warmups):
                    sample(command, directory)
                records = [sample(command, directory) for _ in range(args.repeats)]
                for record in records:
                    record["evidence_first_ns"] = sum(record[key] for key in
                        ("hot_load_ns", "view_prepare_ns", "eager_restore_ns", "first_query_ns"))
                    signatures.add(record["signature"])
                    assert record["cold_objects"] == (2 if mode == "lazy" else 3), record
                    assert record["repeat_cold_objects"] == 0
                report["results"].append(dict(size=size, mode=mode, samples=records, distributions=distribution(records),
                    object_sizes=sorted(path.stat().st_size for path in (store / "objects").rglob("*") if path.is_file())))
            assert len(signatures) == 1, "lazy/eager semantic output mismatch"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {len(report['results'])} groups to {args.output}")


if __name__ == "__main__":
    main()
