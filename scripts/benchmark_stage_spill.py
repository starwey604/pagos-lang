#!/usr/bin/env python3
"""C4b3a: real Stage build/query, copied-path baseline versus shared paths."""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import subprocess
import tempfile
from benchmark_function_evidence import distribution
from benchmark_provenance_queries import sample

ROOT = pathlib.Path(__file__).resolve().parents[1]


def identity(binary):
    cache = (binary.parent / "CMakeCache.txt").read_bytes()
    if b"CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise ValueError("use Release")
    sources = ["CMakeLists.txt", "tools/stage_spill_bench/main.cpp",
               "scripts/benchmark_stage_spill.py", "scripts/benchmark_function_evidence.py",
               "scripts/benchmark_provenance_queries.py"]
    sources += [str(path.relative_to(ROOT)) for parent in (ROOT / "include", ROOT / "lib")
                for path in sorted(parent.rglob("*")) if path.suffix in (".h", ".cpp")]
    return dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                cmake_cache_sha256=hashlib.sha256(cache).hexdigest(),
                source_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
                commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, default=ROOT / "build/release/pagos_stage_spill_bench")
    parser.add_argument("--baseline", type=pathlib.Path, help="preserved pre-change Release probe")
    parser.add_argument("--baseline-identity", type=pathlib.Path, help="identity JSON captured with baseline binary")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--sizes", type=int, nargs="+", default=[128, 512, 1024, 2048])
    args = parser.parse_args()
    if (args.repeats < 3 or args.warmups < 1 or any(size < 1 or size > 2048 for size in args.sizes)
            or bool(args.baseline) != bool(args.baseline_identity)):
        parser.error("requires >=3 repeats, >=1 warmup, sizes 1..2048, paired baseline/identity")
    binary = args.binary.resolve()
    identities = {"shared": identity(binary)}
    binaries = {"shared": binary}
    if args.baseline:
        baseline = args.baseline.resolve()
        original = json.loads(args.baseline_identity.read_text())
        if hashlib.sha256(baseline.read_bytes()).hexdigest() != original["binary_sha256"]:
            parser.error("baseline identity does not match binary")
        identities["copied"] = original
        binaries = {"copied": baseline, **binaries}
    report = dict(schema=1, date=datetime.date.today().isoformat(),
                  machine=platform.platform(), cpu=next(line.split(":", 1)[1].strip() for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                  versions={name: subprocess.check_output([name, "--version"], text=True).splitlines()[0] for name in ("clang", "llvm-config", "python3")},
                  identities=identities, warmups=args.warmups, repeats=args.repeats,
                  config=dict(block_bytes=32768, resident_bytes=262144, cache_bytes=65536, disk_bytes=268435456, semantic_cache="off"),
                  measurement="Two real scalar functions a/b with N named arithmetic bindings, each specialized once. Separate children for build-only (no query, export or whole-graph restoration) and build+native aa source query (front end/analyzer/HIR retained). Linux wait4 whole-child RSS high water includes startup, front end, Stage, file enumeration, cleanup and query if selected; query RSS is NOT isolated. Census is a separate child with an allocating traversal index and is excluded from build/query distributions. Paths census covers reachable module HIR and binding traces, not analyzer-only retained expressions. Counts Step objects and heap string capacity once; excludes shared_ptr control blocks, allocator overhead and path handles. Raw logical steps measure expanded path length, not allocated strings. IO counters are successful CAS payload reads/writes and file sizes, NOT total syscall/physical disk IO; omit validation reads, failed operations, directory IO. No cold-page claim; warm/repeated children share OS page cache. All runs sequential after builds/tests finish; no core pinning or fixed frequency. Baseline and shared probes differ only in path implementation/adapters, build identity and added opt-in census; census is never run on the old probe. Baseline is preserved from a dirty pre-change tree and identified by full source and binary hashes, not reproducible from the HEAD commit alone.", results=[], census=[])
    with tempfile.TemporaryDirectory(prefix="pagos-stage-spill-bench-") as temporary:
        directory = pathlib.Path(temporary)
        for size in args.sizes:
            body = "let n0 = x + 1; " + " ".join(f"let n{i} = n{i-1} + 1;" for i in range(1, size)) + f" n{size-1}"
            text = f"fn a(x: u32) -> u32 {{ {body} }} fn b(x: u32) -> u32 {{ {body} }} let x = external_input(); let y = external_input(); let aa = a(x); let bb = b(y);"
            source = directory / f"module-{size}.pgs"
            source.write_text(text)
            signatures = set()
            shapes = set()
            for version, executable in binaries.items():
                for phase in ("build", "query"):
                    for mode in ("memory", "spill"):
                        records = []
                        for index in range(args.warmups + args.repeats):
                            spill = directory / f"{version}-{size}-{phase}-{mode}-{index}"
                            record = sample([str(executable), mode, phase, str(source), str(spill)], directory)
                            assert not spill.exists() or not any(spill.iterdir()), "task namespace leaked"
                            shapes.add(tuple(record[key] for key in ("nodes", "sites", "roots", "decisions", "fuel")))
                            if phase == "query":
                                signatures.add((record["signature"], record["query_states"], record["query_sources"]))
                            if mode == "spill":
                                assert record["materialized"] == 0 and record["written_blocks"] > 0 and record["released_bytes"] > 0
                                assert record.get("cas_files", record["files"]) == record["written_blocks"]
                                assert record.get("cas_bytes", record["file_bytes"]) == record["written_payload_bytes"] + 49 * record.get("cas_files", record["files"])
                            if index >= args.warmups:
                                records.append(record)
                        report["results"].append(dict(size=size, version=version, phase=phase, mode=mode,
                            source_sha256=hashlib.sha256(text.encode()).hexdigest(), samples=records, distributions=distribution(records)))
            assert len(shapes) == 1 and len(signatures) == 1, "logical Stage/query mismatch"
            for mode in ("memory", "spill"):
                spill = directory / f"census-{size}-{mode}"
                record = sample([str(binary), mode, "census", str(source), str(spill)], directory)
                assert not spill.exists() or not any(spill.iterdir())
                report["census"].append(dict(size=size, mode=mode, sample=record))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {len(report['results'])} groups to {args.output}")


if __name__ == "__main__":
    main()
