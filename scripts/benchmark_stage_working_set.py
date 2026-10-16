#!/usr/bin/env python3
"""C4b3b: real Stage working sets, spill IO and native queries."""
import argparse
import datetime
import hashlib
import json
import pathlib
import os
import shutil
import platform
import subprocess
import tempfile
from benchmark_function_evidence import distribution
from benchmark_provenance_queries import sample
from benchmark_stage_spill import identity

ROOT = pathlib.Path(__file__).resolve().parents[1]


def program(case, functions, steps):
    if case == "contracts":
        return (f"let values = [for i in 0..{steps} {{ comptime let k = if i == 0 {{ 7 }} else {{ 9 }}; k }}]; "
                "let x = external_input(); let arr = [7, x]; comptime let known = arr[0]; let aa = x + values[0];")
    if case == "array-arguments":
        text = "fn first(a: [u32; 64]) -> u32 { a[0] } let x = external_input(); "
        for index in range(functions):
            values = ", ".join([str(index), *["7"] * 63])
            text += f"let r{index} = first([{values}]); "
        return text + "let aa = x + r0;"
    if case == "specializations":
        parameters = ", ".join(f"b{index}: u32" for index in range(steps))
        body = "x + " + " + ".join(f"b{index}" for index in range(steps))
        text = f"fn f(x: u32, {parameters}) -> u32 {{ {body} }} let x = external_input(); "
        for index in range(functions):
            values = ", ".join([str(index), *["7"] * (steps-1)])
            text += f"let r{index} = f(x, {values}); "
        return text + "let aa = r0;"
    body = "let n0 = x + bias; "
    for index in range(1, steps):
        value = f"n{index-1} + bias"
        if case == "mixed":
            value = f"if x == 0 {{ {value} }} else {{ n{index-1} + 2 }}"
        body += f"let n{index} = {value}; "
    body += f"n{steps-1}"
    text = " ".join(f"fn f{index}(x: u32, bias: u32) -> u32 {{ {body} }}" for index in range(functions))
    text += " let x = external_input(); "
    text += " ".join(f"let r{index} = f{index}(x, {index+1});" for index in range(functions))
    return text + " let aa = r0;"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, default=ROOT / "build/release/pagos_stage_spill_bench")
    parser.add_argument("--work-dir", type=pathlib.Path, default=ROOT / "build/benchmarks", help="temporary source/spill files on the target filesystem")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--quick", action="store_true", help="small validation corpus only")
    args = parser.parse_args()
    if args.repeats < 3 or args.warmups < 1:
        parser.error("requires >=3 repeats and >=1 warmup")
    binary = args.binary.resolve()
    build = identity(binary)
    build["source_sha256"][str(pathlib.Path(__file__).resolve().relative_to(ROOT))] = hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    filesystem = dict(path=str(work), device=os.stat(work).st_dev,
        type=subprocess.check_output(["findmnt", "-n", "-o", "FSTYPE", "-T", str(work)], text=True).strip() if shutil.which("findmnt") else "unavailable")
    report = dict(schema=1, filesystem=filesystem, date=datetime.date.today().isoformat(), identity=build,
                  machine=platform.platform(), cpu=next(line.split(":", 1)[1].strip() for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                  versions={name: subprocess.check_output([name, "--version"], text=True).splitlines()[0] for name in ("clang", "llvm-config", "python3")},
                  warmups=args.warmups, repeats=args.repeats,
                  config=dict(block_bytes=32768, resident_bytes=262144, cache_bytes=65536, disk_bytes=268435456, semantic_cache="off", definition_catalog=True),
                  measurement="Sequential fresh Release children after builds/tests stop. Every sample parses/typechecks real .pgs and runs Stage WITH Recorder definition identity, semantic cache off. Build-only never queries/exports/restores; query children additionally execute native aa source query with front end and HIR retained. For query children /proc/self/clear_refs=5 resets the RSS high-water mark after capturing build VmHWM. Query VmHWM is sampled immediately after the query, including the retained front end/HIR baseline (reported separately), not a fresh query-only reader. Whole-child peak is max(pre-reset build VmHWM, exit wait4 RSS), including startup/front end/Stage/enumeration/destruction and potentially a fork/pre-exec parent-RSS floor on Linux; use build VmHWM for compilation comparisons. Read/write counters include this measurement IO. Census children are separate, instrument end-of-recording maps and before/after Analyzer cleanup, then traverse reachable HIR. Capacities include vectors, map value objects/buckets and owned heap strings/constants; exclude map links, shared_ptr control blocks, allocator overhead and census indexes. Snapshot/sampled categories do not sum to a complete peak RSS. Kernel /proc/self/io counters are sampled after file enumeration, before output/destructors, including loader/source and FileStore verification read/write traffic; counters' own reads add small overhead. read_bytes/write_bytes are kernel storage accounting with warm OS caches, NOT physical-device traffic; directory/unlink metadata operations and later cleanup are outside syscall-byte counters. File enumeration includes the session lock marker; CAS bytes/files are separate. No page-cache flushing, core pinning or fixed frequency. Relative to C4b3a, this corpus uses definition identity and additional workloads, so do not attribute cross-report differences to spill or cleanup.", results=[], census=[])
    matrix = [("chain", 2, 2048), ("wide", 32, 512), ("wide", 128, 512),
              ("mixed", 32, 128), ("contracts", 1, 4096), ("specializations", 512, 8), ("array-arguments", 512, 64)]
    if args.quick:
        matrix = [("chain", 2, 32), ("mixed", 2, 16), ("contracts", 1, 32), ("specializations", 8, 8), ("array-arguments", 8, 64)]
    with tempfile.TemporaryDirectory(prefix="pagos-working-set-", dir=work) as temporary:
        directory = pathlib.Path(temporary)
        for case, functions, steps in matrix:
            name = f"{case}-{functions}-{steps}"
            source = directory / f"{name}.pgs"
            text = program(case, functions, steps)
            source.write_text(text)
            shapes, signatures = set(), set()
            for phase in ("build", "query"):
                for mode in ("memory", "spill"):
                    records = []
                    for index in range(args.warmups + args.repeats):
                        spill = directory / f"{name}-{phase}-{mode}-{index}"
                        record = sample([str(binary), mode, phase, str(source), str(spill), "identity"], directory)
                        assert not spill.exists() or not any(spill.iterdir()), "temporary task leaked"
                        record["exit_peak_rss_kib"] = record["peak_rss_kib"]
                        record["peak_rss_kib"] = max(record["build_peak_rss_kib"], record["peak_rss_kib"])
                        if phase == "query":
                            assert record["query_peak_reset"]
                            record["query_peak_growth_kib"] = max(0, record["query_peak_rss_kib"] - record["query_baseline_rss_kib"])
                        assert record["io_available"]
                        shapes.add(tuple(record[key] for key in ("nodes", "sites", "roots", "decisions", "fuel")))
                        if phase == "query":
                            signatures.add(tuple(record[key] for key in ("signature", "query_states", "query_sources")))
                        if mode == "spill":
                            assert record["materialized"] == 0 and record["released_bytes"] > 0
                            assert record["cas_files"] == record["written_blocks"]
                            assert record["cas_bytes"] == record["written_payload_bytes"] + 49 * record["cas_files"]
                            assert record["files"] == record["cas_files"] + 1
                            assert record["io_wchar"] >= record["file_bytes"]
                        if index >= args.warmups:
                            records.append(record)
                    report["results"].append(dict(case=case, functions=functions, steps=steps, phase=phase, mode=mode,
                        source_sha256=hashlib.sha256(text.encode()).hexdigest(), samples=records, distributions=distribution(records)))
            assert len(shapes) == len(signatures) == 1, "Stage/query semantics differ"
            for mode in ("memory", "spill"):
                spill = directory / f"{name}-census-{mode}"
                record = sample([str(binary), mode, "census", str(source), str(spill), "identity"], directory)
                assert all(record[key] == 0 for key in ("analyzer_tables_after", "analyzer_arguments_after", "analyzer_cache_after"))
                assert not spill.exists() or not any(spill.iterdir())
                if case in ("specializations", "array-arguments"):
                    assert record["analyzer_arguments_before"] > 0
                if case == "specializations":
                    assert record["specialization_bytes"] > 0
                report["census"].append(dict(case=case, functions=functions, steps=steps, mode=mode, sample=record))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {len(report['results'])} groups to {args.output}")


if __name__ == "__main__":
    main()
