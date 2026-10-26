#!/usr/bin/env python3
"""C4c3b: independent real-source query tasks, saved state, IO and continuation."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from benchmark_provenance_queries import sample
from benchmark_function_evidence import distribution

ROOT = Path(__file__).resolve().parents[1]


def program(case, size):
    if case == "wide":
        text = " ".join(f"let a{i} = external_input();" for i in range(size))
        return text + " let aa = [" + ", ".join(f"a{i}" for i in range(size)) + "];"
    body = "let a0 = x + external_input(); "
    body += " ".join(f"let a{i} = a{i-1} + x;" for i in range(1, size))
    return f"fn f(x: u32) -> u32 {{ {body} a{size-1} }} let x = external_input(); let aa = f(x);"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/release/pagos_query_task_bench")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/benchmarks")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--date", default=datetime.date.today().isoformat())
    args = parser.parse_args()
    binary = args.binary.resolve()
    cache = (binary.parent / "CMakeCache.txt").read_bytes()
    if not args.smoke and (b"CMAKE_BUILD_TYPE:STRING=Release" not in cache or args.repeats < 3 or args.warmups < 1):
        parser.error("measurements require Release, >=3 repeats and >=1 warmup")
    repeats, warmups = (1, 0) if args.smoke else (args.repeats, args.warmups)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    sources = ["include/pagos/stage/provenance_topology.h", "include/pagos/stage/provenance_query.h",
               "lib/stage/provenance_topology.cpp", "lib/stage/provenance_query.cpp", "lib/stage/query_storage.cpp",
               "lib/cache/task_store.cpp", "lib/stage/function_evidence_view.cpp", "tools/query_task_bench/main.cpp",
               "scripts/benchmark_query_tasks.py", "scripts/benchmark_provenance_queries.py", "scripts/benchmark_function_evidence.py"]
    report = dict(schema=1, date=args.date,
        commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        dirty=bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), cmake_cache_sha256=hashlib.sha256(cache).hexdigest(),
        source_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
        machine=platform.platform(), cpu=next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
        filesystem=dict(path=str(work), device=os.stat(work).st_dev, type=subprocess.check_output(["findmnt", "-n", "-o", "FSTYPE", "-T", str(work)], text=True).strip() if shutil.which("findmnt") else "unavailable"),
        versions={name: subprocess.check_output([name, "--version"], text=True).splitlines()[0] for name in ("clang", "llvm-config", "python3")},
        warmups=warmups, repeats=repeats, smoke=args.smoke,
        config=dict(segment_bytes=16384, paging_resident_bytes=262144, paging_cache_bytes=65536, evidence_cache_bytes=4194304),
        measurement="Sequential fresh children after builds/tests stop. One separate Stage/Recorder publisher per real .pgs; all query children only parse/typecheck and load that evidence (Stage=0). Native cuts one 64-state/512-edge/64-binding slice with no output. Topology frontier cuts one 64/512/64/256 slice; formula cuts after half the physical-node count in formula states. Save clocks exclude prefix work; original task/view destroyed before a fresh view opens, and restore clocks exclude fresh-view metadata loading. Restore must preserve all logical counters; continuation uses 64/512/64/256 slices. Memory mode retains state after v1 native/v3 topology save; paged native and v4 topology offload drop task-owned buffers before destruction. Owned capacity is sampled at advance boundaries, not continuous allocation peak; native directory is separate, topology hash accounting includes values/buckets but excludes links. Codec buffer high-water is retained vector capacity observed in segmented topology IO, excluding CAS envelope scratch and directory metadata. Task read requests include missing probes, head and dedup verification; they are not physical device IO. Disk includes CAS envelopes, old/unpublished objects, head and marker; native paging may append leaves after the last saved head. Topology save_cas_added counts new CAS bytes, excluding head replacement; amplification ratios use its fixed-record logical_table_bytes, excluding control/directories. Native paging read/write telemetry covers the restored backend only. wait4 whole-child RSS includes front end, graph cache, saved/restored state, result digest/compatibility copies and destruction; no RSS or constant-time restore claim. Warm OS page cache, no cache flushing, pinning or fixed frequency. Smoke is correctness only.", results=[])
    matrix = [("wide", 32), ("function", 32)] if args.smoke else [("wide", 257), ("wide", 2049), ("function", 256), ("function", 1024)]
    with tempfile.TemporaryDirectory(prefix="pagos-task-bench-", dir=work) as temporary:
        directory = Path(temporary)
        for case, size in matrix:
            source = directory / f"{case}-{size}.pgs"
            text = program(case, size)
            source.write_text(text)
            evidence = directory / f"evidence-{case}-{size}"
            assert sample([str(binary), "publish", str(source), str(evidence), str(directory / "unused"), "frontier"], directory)["stage_runs"] == 1
            signatures = {}
            for mode, phase in [(mode, "frontier") for mode in ("native-memory", "native-paged", "topology-resident", "topology-segmented")] + [(mode, "formula") for mode in ("topology-resident", "topology-segmented")]:
                records = []
                for iteration in range(warmups + repeats):
                    parent = directory / f"task-{case}-{size}-{mode}-{phase}-{iteration}"
                    record = sample([str(binary), mode, str(source), str(evidence), str(parent), phase], directory)
                    assert record["stage_runs"] == 0 and record["files"] > 0
                    if mode in ("native-paged", "topology-segmented"):
                        assert record["after_save_bytes"] == 0
                    if mode == "native-paged":
                        assert record["sampled_peak_owned"] <= report["config"]["paging_resident_bytes"]
                    if mode == "topology-segmented":
                        assert record["save_segments"] == record["restore_segments"] > 0
                        record["save_amplification"] = record["save_cas_added"] / max(1, record["logical_table_bytes"])
                    key = "topology" if mode.startswith("topology") else "native"
                    signature = (record["result"], tuple(record["work"]))
                    assert signatures.setdefault(key, signature) == signature, (case, size, mode, phase)
                    if iteration >= warmups:
                        records.append(record)
                    shutil.rmtree(parent)
                report["results"].append(dict(case=case, size=size, mode=mode, phase=phase,
                    source_sha256=hashlib.sha256(text.encode()).hexdigest(), samples=records,
                    distributions=distribution(records) if repeats >= 3 else {}))
                print(f"{case}-{size} {mode}/{phase}: {repeats} samples", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
