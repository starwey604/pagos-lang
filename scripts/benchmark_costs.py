#!/usr/bin/env python3
"""S2 cost ledger: run existing Release benchmarks and missing CLI workflows."""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

from benchmark import ROOT, LIMITS, digest, disk_usage, output, run_sample
from benchmark_function_evidence import distribution

TARGET = "riscv32-unknown-elf"
CASES = ["tests/lit/stage/crc32.pgs", "benchmarks/closed-generator.pgs",
         "benchmarks/specializations.pgs", "benchmarks/aggregate.pgs",
         "benchmarks/board-profile.pgs"]
BINARIES = ["pagosc", "pagos_snapshot_bench", "pagos_function_evidence_bench",
            "pagos_query_task_bench", "pagos_stage_spill_bench"]


def inventory(directory):
    return {str(p.relative_to(directory)): (p.stat().st_size, p.stat().st_mtime_ns)
            for p in directory.rglob("*") if p.is_file()}


def collect(measure, repeats, warmups):
    for index in range(warmups):
        measure(f"warm-{index}")
    samples = [measure(str(index)) for index in range(repeats)]
    return {"samples": samples, "distributions": distribution(samples)}


def workflows(compiler, work, repeats, warmups):
    cli = [str(compiler), f"--target={TARGET}"]
    report = {"schema": 1, "target": TARGET, "edits": [], "replay": [],
              "recording": [], "warmups": warmups, "repeats": repeats}
    with tempfile.TemporaryDirectory(prefix="cost-workflows-", dir=work) as temporary:
        directory = Path(temporary)
        source = directory / "input.pgs"

        def compile_to(store, mode, command="check", evidence="value", capture=False):
            return run_sample([*cli, *LIMITS, f"--stage-cache={mode}",
                               f"--cache-dir={store}", "--cache-stats",
                               f"--cache-evidence={evidence}", command, str(source)],
                              directory, store, capture=capture)

        original = ("fn square(x: u32) -> u32 { x * x } "
                    "let a = square(7); let result = a + external_input();\n")
        variants = {"unchanged": original,
                    "position": "// source offset edit\n\n" + original,
                    "unrelated": original + "fn unrelated() -> u32 { 9 }\n",
                    "definition": original.replace("x * x", "x + x")}
        edits = [(edit, original, text, 1 if edit in ("unchanged", "position") else 0)
                 for edit, text in variants.items()]
        board = (ROOT / "benchmarks/board-profile.pgs").read_text()
        edits.append(("board-revision", board,
                      board.replace("let revision = 1;", "let revision = 2;"), 0))
        for edit, original, text, expected_hits in edits:
            source.write_text(text)
            fresh = compile_to(directory / "off", "off", "emit-llvm")
            if edit == "board-revision":
                checked = compile_to(directory / "off", "off", "emit-llvm", capture=True)
                assert "@uart_exchange(i32 313)" in checked["stdout"]
            logical = compile_to(directory / "off", "off", "explain-stage", capture=True)
            verified_store = directory / f"verify-{edit}"
            source.write_text(original)
            compile_to(verified_store, "read-write")
            source.write_text(text)
            restored = compile_to(verified_store, "read-only", "explain-stage", capture=True)
            assert logical["stdout"] == restored["stdout"], edit

            def measure(index):
                store = directory / f"edit-{edit}-{index}"
                source.write_text(original)
                compile_to(store, "read-write")
                before = disk_usage(store)
                source.write_text(text)
                sample = compile_to(store, "read-write", "emit-llvm")
                assert sample["output_sha256"] == fresh["output_sha256"], edit
                telemetry = sample["cache_telemetry"]
                assert telemetry["hits"] == expected_hits
                sample["disk_before"] = before
                sample["disk_growth_bytes"] = sample["cache_disk"]["bytes"] - before["bytes"]
                shutil.rmtree(store)
                return sample

            record = collect(measure, repeats, warmups)
            record.update(edit=edit, base_source=original, source=text, logical_output=logical["stdout"],
                          off_sample=fresh)
            report["edits"].append(record)
            print(f"edit {edit}: {repeats} samples", flush=True)

        programs = [("scalar", 1, "fn square(x: u32) -> u32 { x * x } let a = square(7);", "square")]
        for size in (4, 4096):
            programs.append(("generator", size,
                             f"fn table(x: u32) -> [u32; {size}] {{ "
                             f"[for i in 2..{size + 2} {{ i * x + 1 }}] }} let a = table(7);", "table"))
        for kind, size, text, function in programs:
            source.write_text(text)
            capability = f"{kind}-replay"
            for mode, evidence in (("off", "value"), ("read-write", "value"),
                                   ("read-write", capability)):
                def measure(index):
                    store = directory / f"record-{kind}-{size}-{mode}-{evidence}-{index}"
                    sample = compile_to(store, mode, evidence=evidence)
                    if store.exists():
                        shutil.rmtree(store)
                    return sample
                record = collect(measure, repeats, warmups)
                record.update(kind=kind, size=size, mode=mode, evidence=evidence, source=text)
                report["recording"].append(record)

            store = directory / f"replay-{kind}-{size}"
            seed = compile_to(store, "read-write", evidence=capability)
            before = inventory(store)
            indices = [None] if kind == "scalar" else [0, size - 1]
            for element in indices:
                for detail in ("all", "result-only"):
                    selection = [] if detail == "all" else ["--replay-count=0"]
                    command = [*cli, f"--replay-read={store}", f"--replay-function={function}",
                               "--replay-arg=7", "--max-fuel=0", *selection,
                               *([] if element is None else [f"--replay-element={element}"]),
                               "explain-replay", str(source)]

                    def measure(index):
                        sample = run_sample(command, directory, store, capture=True)
                        payload = json.loads(sample.pop("stdout"))
                        assert payload["schema"] == 1 and payload["status"] == "verified"
                        assert payload["stage_runs"] == 0
                        expected = 49 if element is None else (element + 2) * 7 + 1
                        assert payload["result"] == {"type": "u32", "value": str(expected)}
                        if detail == "result-only":
                            assert not payload["details"]
                        sample["payload"] = payload
                        return sample

                    record = collect(measure, repeats, warmups)
                    assert inventory(store) == before, "replay changed read-only storage"
                    record.update(kind=kind, size=size, element=element, detail=detail,
                                  seed=seed, command=command, source=text)
                    report["replay"].append(record)
                    print(f"replay {kind}/{size}/{element}/{detail}: {repeats} samples", flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/s2-release")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/benchmarks")
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--date", default=datetime.date.today().isoformat())
    parser.add_argument("--smoke", action="store_true", help="validate all paths with smaller existing corpora")
    args = parser.parse_args()
    if args.repeats < 3 or args.warmups < 1:
        parser.error("requires >=3 repeats and >=1 warmup")
    build = args.build_dir.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        parser.error("use a Release build")
    destination = args.output_dir.resolve()
    if destination.exists() and any(destination.iterdir()):
        parser.error("output directory must be new or empty; preserve previous reports")
    destination.mkdir(parents=True, exist_ok=True)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    repeats, warmups = (3, 1) if args.smoke else (args.repeats, args.warmups)
    common = ["--repeats", str(repeats), "--warmups", str(warmups)]
    board_ir = output([str(build / "pagosc"), f"--target={TARGET}",
                       "emit-llvm", "benchmarks/board-profile.pgs"])
    assert "@uart_exchange(i32 625)" in board_ir
    ledger = {
        "schema": 1, "date": args.date, "commit": output(["git", "rev-parse", "HEAD"]),
        "dirty": bool(output(["git", "status", "--porcelain"])),
        "tracked_diff": output(["git", "diff", "HEAD"]),
        "build_type": "Release", "build_dir": str(build), "smoke": args.smoke,
        "target": {"cli": TARGET, "cpu": "generic-rv32", "features": "",
                   "resolved_triple": re.search(r'target triple = "([^"]+)"', board_ir)[1],
                   "data_layout": re.search(r'target datalayout = "([^"]+)"', board_ir)[1],
                   "probes": "RV32 semantic context with e-p:32:32; no machine code emitted"},
        "machine": platform.platform(), "cpu": next(line.split(":", 1)[1].strip()
            for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
        "filesystem": {"path": str(work), "device": os.stat(work).st_dev,
                       "type": output(["stat", "-f", "-c", "%T", str(work)])},
        "scheduler": {"affinity": sorted(os.sched_getaffinity(0)),
                      "cgroup_cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text().strip()
                      if Path("/sys/fs/cgroup/cpu.max").exists() else None},
        "versions": {name: output([name, "--version"]).splitlines()[0]
                     for name in ("clang", "llvm-config", "cmake", "python3")},
        "binaries": {name: digest((build / name).read_bytes()) for name in BINARIES},
        "cmake_cache_sha256": digest(cache.encode()),
        "sources": {str(p.relative_to(ROOT)): digest(p.read_bytes())
                    for p in sorted((ROOT / "scripts").glob("benchmark*.py"))},
        "warmups": warmups, "repeats": repeats,
        "limits": {"cache_and_workflows": LIMITS,
                   "snapshot_cli_and_probes": {"fuel": 1000000, "recursion_depth": 128,
                       "specializations": 4096, "residual_specializations": 4096,
                       "array_elements": 65536, "array_bytes": 262144,
                       "aggregate_members": 65536, "aggregate_bytes": 262144},
                   "replay": {"stage_fuel": 0, "fuel": 100000, "value_bytes": 8388608,
                       "output_records": 4096, "execution_output_bytes": 1048576,
                       "render_bytes": 1048576}},
        "measurement": {
            "order": "sequential suites and fresh children after build/test completion",
            "wall": "whole CLI process includes startup/output capture and 1ms wait4 polling; probe phase timers are separate",
            "rss": "wait4 whole-child high-water can include a fork/pre-exec parent-RSS floor; small CLI RSS is descriptive; spill build uses explicit post-exec VmHWM; never sum phase peaks",
            "io": "input_blocks/output_blocks are wait4 filesystem block-operation counters, not physical-device bytes; spill additionally reports proc IO; probe request counts measure logical storage calls",
            "cold": "empty application store per sample; OS page cache uncontrolled; no CPU pinning/fixed frequency",
            "edits": "first read-write compile after each edit; reseeded original store outside measured window; changed output and logical stats checked against uncached current source",
            "replay": "fresh read-only CLI processes, max-fuel=0, independently checked selected result; output bytes include JSON/newline",
            "board": "board-profile.pgs is the pre-S3 configuration workload; physical UART/clock validation is S3",
        }, "commands": [], "reports": []}

    def run(name, script, flags):
        command = [sys.executable, str(ROOT / "scripts" / script), *flags,
                   "--output", str(destination / name)]
        ledger["commands"].append(command)
        print(f"START {name}", flush=True)
        subprocess.run(command, cwd=ROOT, check=True)
        ledger["reports"].append(name)

    cases = CASES[:1] + CASES[-1:] if args.smoke else CASES
    run("cache.json", "benchmark.py", ["--compiler", str(build / "pagosc"),
        "--target", TARGET, "--cache-modes", "all", *common,
        *[part for case in cases for part in ("--case", case)],
        *([] if not args.smoke else ["--command", "check"])])
    run("snapshots.json", "benchmark_evidence.py", ["--build-dir", str(build), "--target", TARGET, *common])
    run("functions.json", "benchmark_function_evidence.py", ["--binary", str(build / BINARIES[2]),
        *common, *([] if not args.smoke else ["--sizes", "32"])])
    run("working-set.json", "benchmark_stage_working_set.py", ["--binary", str(build / BINARIES[4]),
        "--work-dir", str(work), *common, *(["--quick"] if args.smoke else [])])
    run("tasks.json", "benchmark_query_tasks.py", ["--binary", str(build / BINARIES[3]),
        "--work-dir", str(work), "--date", args.date, *common, *(["--smoke"] if args.smoke else [])])
    print("START workflows.json", flush=True)
    data = workflows(build / "pagosc", work, repeats, warmups)
    (destination / "workflows.json").write_text(json.dumps(data, indent=2) + "\n")
    ledger["reports"].append("workflows.json")
    (destination / "ledger.json").write_text(json.dumps(ledger, indent=2) + "\n")
    print(f"PASS: {destination / 'ledger.json'}", flush=True)


if __name__ == "__main__":
    main()
